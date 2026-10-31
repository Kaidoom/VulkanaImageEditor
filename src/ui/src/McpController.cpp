#include "imageeditor/ui/McpController.hpp"
#include "McpToolHelpers.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <QApplication>
#include <QAbstractSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QThread>

namespace imageeditor::ui {
using namespace mcpdetail;
namespace {
QString revisionToken(const core::Document& document)
{
    // Geometry revision alone excludes journalled raster edits; content state
    // alone excludes pixel selections. All three are needed for safe painting.
    return id(document.contentState()) + '-' + id(document.revision()) + '-' + id(document.selectionRevision());
}
QString activeToolName(core::ToolId tool)
{
    switch (tool) {
    case core::ToolId::Move: return "move";
    case core::ToolId::Marquee: return "rectangle_selection";
    case core::ToolId::Lasso: return "lasso";
    case core::ToolId::Brush: return "brush";
    case core::ToolId::Eraser: return "eraser";
    case core::ToolId::Fill: return "fill";
    case core::ToolId::Eyedropper: return "eyedropper";
    case core::ToolId::Text: return "text";
    case core::ToolId::Transform: return "transform";
    case core::ToolId::Shape: return "shape";
    case core::ToolId::Measure: return "measure";
    case core::ToolId::SelectByColor: return "color_select";
    case core::ToolId::Crop: return "crop";
    case core::ToolId::SmartSelect: return "smart_select";
    case core::ToolId::Cloning: return "clone";
    case core::ToolId::LocalBlur: return "local_blur";
    case core::ToolId::CanvasResize: return "canvas_resize";
    }
    return "unknown";
}
}
McpController::McpController(MainWindow& window)
    : QObject(&window), window_(window), server_(registry_, QCoreApplication::applicationVersion(), this)
{
    registerDocumentTools();
    registerLayerTools();
    registerPaintingTools();
    registerLayoutTools();
    server_.onStateChanged = [this] { refreshStatus(); };
}
McpController::~McpController()
{
    server_.onStateChanged = {};
    server_.close();
    delete indicator_.data(); delete stopButton_.data();
}
bool McpController::start(const QString& path, QString* error)
{
    if (running()) {
        if (error) *error = "Stop the current MCP listener before changing its endpoint.";
        return false;
    }
    error_.clear();
    if (!server_.listen(path.isEmpty() ? mcp::defaultSocketPath() : path, &error_)) {
        if (error) *error = error_;
        refreshStatus(); return false;
    }
    if (error) error->clear();
    refreshStatus();
    return true;
}
void McpController::stop()
{
    error_.clear();
    server_.close();
    refreshStatus();
}
McpController::Status McpController::status() const
{
    if (server_.stopping()) return Status::Stopping;
    if (!error_.isEmpty()) return Status::Error;
    if (server_.connected()) return Status::Connected;
    return server_.listening() ? Status::Listening : Status::Stopped;
}
void McpController::refreshStatus()
{
    const StatusSnapshot next{status(), clientCount(), endpoint(), error_, running()};
    if (lastStatus_ && *lastStatus_ == next) return;
    lastStatus_ = next;
    const bool visible = next.running || !next.error.isEmpty();
    if (visible && !indicator_) {
        indicator_ = new QLabel(&window_); indicator_->setObjectName("McpConnectionStatus");
        window_.statusBar()->addPermanentWidget(indicator_);
        stopButton_ = new QPushButton(QStringLiteral("Stop MCP"), &window_); stopButton_->setObjectName("McpStopButton");
        stopButton_->setAutoDefault(false);
        window_.statusBar()->addPermanentWidget(stopButton_);
        connect(stopButton_, &QPushButton::clicked, this, &McpController::stop);
    }
    if (indicator_) {
        indicator_->setText(next.status == Status::Connected ? QStringLiteral("MCP connected (%1)").arg(next.clients) : next.status == Status::Stopping ? "MCP stopping…"
            : next.status == Status::Error ? "MCP error" : "MCP enabled");
        indicator_->setToolTip(next.error.isEmpty() ? QStringLiteral("Local document automation. Endpoint: ") + next.endpoint : next.error);
        indicator_->setVisible(visible); stopButton_->setVisible(visible);
        stopButton_->setEnabled(next.status != Status::Stopping);
    }
    emit statusChanged();
}
core::EditorSession& McpController::session() const { return window_.session(); }
core::Document& McpController::document() const
{
    require(session().document() != nullptr, "No active document");
    return *session().document();
}
core::Layer& McpController::layer(const QJsonObject& args, const char* key) const
{
    auto* value = document().layer(id(args, key));
    require(value != nullptr, QStringLiteral("No editable leaf layer with this ID: ") + args.value(QLatin1String(key)).toString());
    return *value;
}
bool McpController::execute(std::unique_ptr<core::Command> command)
{
    if (inValidationPreflight())
        throw std::logic_error("Document edits cannot execute inside MCP validation/preparation");
    publicationAttempted_ = true;
    const bool applied = window_.executeDocumentCommand(std::move(command));
    if (applied) changed();
    return applied;
}
void McpController::changed(bool layers)
{
    window_.fileState().untouched = false;
    window_.synchronizeUi(layers, false);
    window_.refreshDocumentTabs();
}
QJsonObject McpController::result(bool didChange) const
{
    return {{"documentId", id(window_.activeDocumentId())},
        {"revision", session().document() ? revisionToken(document()) : QString()}, {"changed", didChange}};
}
NewDocumentDialog* McpController::newDocumentDialog() const
{
    auto* presenter = window_.workspaceDialog_;
    if (!presenter || !presenter->active() || !presenter->isVisible()
        || window_.workspace_->modalOverlay() != presenter) return nullptr;
    for (auto* child : presenter->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        if (auto* dialog = dynamic_cast<NewDocumentDialog*>(child);
            dialog && dialog->isVisible() && dialog->mode() == NewDocumentDialog::Mode::CreateDocument)
            return dialog;
    return nullptr;
}
QJsonArray McpController::blockers(bool allowNewDocumentDialog) const
{
    const auto* chooser = newDocumentDialog();
    const auto* creationDialog = allowNewDocumentDialog ? chooser : nullptr;
    QJsonArray values;
    const auto add = [&](const char* code, QString message, const char* resolution = "Finish or cancel this interaction in Vulkana, then query state again.") {
        values.append(QJsonObject{{"code", QLatin1String(code)}, {"message", message}, {"resolution", QLatin1String(resolution)}});
    };
    if (window_.graphicsStartupPending_) add("graphics_initializing", "Graphics are initializing.", "Wait and query state again.");
    if (window_.fileBusy_) add("file_operation", "A file operation is in progress.", "Wait and query state again.");
    if (window_.workspaceDialog_ && window_.workspaceDialog_->isVisible() && !creationDialog) {
        QString title;
        for (auto* child : window_.workspaceDialog_->findChildren<QWidget*>())
            if (!child->windowTitle().isEmpty()) { title = child->windowTitle(); break; }
        add("workspace_dialog", title.isEmpty() ? QStringLiteral("An editor dialog is open.") : title,
            chooser ? "vulkana_document_create can dismiss New Canvas and create a document directly when canCreateDocument is true. Other edits require closing it or using vulkana_cancel_interaction."
                    : "Resolve the dialog in Vulkana or explicitly cancel it with vulkana_cancel_interaction.");
    }
    if (auto* modal = QApplication::activeModalWidget()) add("modal_dialog", modal->windowTitle());
    if (auto* popup = QApplication::activePopupWidget()) add("popup_menu", popup->objectName(), "Close the menu or popup in Vulkana.");
    if (window_.textController_ && window_.textController_->active()) add("text_edit", "Interactive text editing is active.", "Finish text editing in Vulkana or use vulkana_finish_text.");
    if (window_.refinement_) add("refine_selection", "Refine Selection has an unapplied preview.");
    if (window_.backgroundRemoval_) add("background_removal", "Remove Background has an unapplied preview.");
    if (window_.snapshotPreview_) add("snapshot_preview", "A snapshot preview or playback is active.");
    if (window_.layerTransform_ || window_.selectionTransform_ || window_.canvasResize_)
        add("transform", "A transform or canvas resize is awaiting Apply/Cancel.");
    if (window_.layerRenameEditor_) add("layer_rename", "A layer is being renamed.");
    const auto* focus = QApplication::focusWidget();
    const auto* field = qobject_cast<const QLineEdit*>(focus);
    if (!field && qobject_cast<const QAbstractSpinBox*>(focus)) field = focus->findChild<QLineEdit*>();
    if (field && field->isModified() && (!creationDialog || !creationDialog->isAncestorOf(field)))
        add("field_edit", "A focused text or numeric field contains an uncommitted edit.");
    if (window_.cloneProcessing_ || window_.localBlurProcessing_ || window_.spotHealJob_)
        add("image_processing", "Image processing is in progress.", "Wait and query state again.");
    if (!window_.shortcutGestureIdle(true)) add("editing_gesture", "A brush, selection, drag or numeric edit is in progress.");
    if (window_.workspace_->hasModalOverlay() && values.isEmpty() && !creationDialog) add("modal_overlay", "A canvas overlay is blocking editing.");
    return values;
}
QJsonObject McpController::state(bool includeLayers) const
{
    const auto blocked = blockers();
    QJsonArray documents;
    for (const auto documentId : window_.documentIds()) {
        const auto* context = window_.documentContext(documentId);
        documents.append(QJsonObject{{"documentId", id(documentId)}, {"name", context->displayName},
            {"path", context->projectPath}, {"modified", context->session.document()->isModified()},
            {"active", documentId == window_.activeDocumentId()}});
    }
    QJsonObject out {{"documents", documents}, {"blockers", blocked}, {"canEdit", session().document() && blocked.isEmpty()},
        {"canCreateDocument", blocked.isEmpty() || (newDocumentDialog() && blockers(true).isEmpty())},
        {"connected", server_.connected()}, {"connectedClients", clientCount()}, {"coordinates", "Document pixels, origin top-left, x right, y down. IDs are decimal strings; revision is an opaque string."}};
    if (!session().document()) { out["document"] = QJsonValue::Null; return out; }
    const auto& doc = document();
    const auto extent = doc.canvas().extent;
    QJsonArray selected;
    for (auto value : session().selectedLayers()) selected.append(id(value));
    QJsonObject active {{"documentId", id(window_.activeDocumentId())}, {"revision", revisionToken(doc)},
        {"width", int(extent.width)}, {"height", int(extent.height)}, {"dpi", doc.canvas().dotsPerInch},
        {"modified", doc.isModified()}, {"selectedLayers", selected},
        {"activeLayer", session().activeLayer() ? QJsonValue(id(*session().activeLayer())) : QJsonValue::Null},
        {"editingMask", session().editingLayerMask()}, {"undoDepth", int(session().history().undoDepth())},
        {"redoDepth", int(session().history().redoDepth())}, {"hasSelection", bool(doc.selection())},
        {"foreground", color(session().colors().foreground())}, {"background", color(session().colors().background())}};
    active["primaryColor"] = color(session().colors().primary);
    active["secondaryColor"] = color(session().colors().secondary);
    active["activeColorSlot"] = session().colors().active == core::ColorSlot::Primary ? "primary" : "secondary";
    active["activeTool"] = activeToolName(session().activeTool());
    QJsonObject selection{{"unrestricted", !doc.selection()}};
    if (doc.selection()) {
        const auto bounds = doc.selection()->bounds();
        selection["empty"] = bounds.empty();
        selection["bounds"] = QJsonObject{{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}};
    }
    active["selection"] = selection;
    const auto& viewport = window_.canvasWindow_->scene().viewport;
    active["view"] = QJsonObject{{"zoom", viewport.zoom()}, {"panX", viewport.pan().x}, {"panY", viewport.pan().y}};
    if (includeLayers) {
        QJsonArray layers, containers, roots;
        for (const auto root : doc.tree().roots) roots.append(id(root));
        for (const auto& container : doc.tree().containers) {
            QJsonArray children;
            for (const auto child : container.children) children.append(id(child));
            containers.append(QJsonObject{{"layerId", id(container.id)}, {"name", QString::fromStdString(container.name)},
                {"type", container.kind == core::ContainerKind::Folder ? "folder" : container.kind == core::ContainerKind::Group ? "group" : "clipping_group"},
                {"visible", container.visible}, {"children", children}});
        }
        for (const auto& layer : doc.layers()) {
            const auto size = core::layerGeometryExtent(layer);
            const auto& m = layer.localToDocument;
            layers.append(QJsonObject{{"layerId", id(layer.id)}, {"name", QString::fromStdString(layer.name)},
                {"type", std::holds_alternative<core::RasterLayer>(layer.payload) ? "raster" : std::holds_alternative<core::TextLayer>(layer.payload) ? "text" : std::holds_alternative<core::ShapeLayer>(layer.payload) ? "shape" : "adjustment"},
                {"visible", layer.visible}, {"effectivelyVisible", doc.isEffectivelyVisible(layer.id)},
                {"opacity", layer.opacity}, {"fillOpacity", layer.fillOpacity}, {"width", double(size.width)}, {"height", double(size.height)},
                {"transform", QJsonArray{m.m00, m.m01, m.m02, m.m10, m.m11, m.m12, m.m20, m.m21, m.m22}}, {"hasMask", bool(layer.mask)}});
        }
        active["layers"] = layers; active["containers"] = containers; active["rootsBottomToTop"] = roots;
    }
    active["snapshotCount"] = double(doc.snapshotTimeline().frames.size());
    if (includeLayers) {
        QJsonArray frames;
        for (const auto& frame : doc.snapshotTimeline().frames)
            frames.append(QJsonObject{{"frameId", id(frame.id)}, {"durationMs", int(frame.durationMs)}, {"captured", bool(frame.image)}});
        active["snapshots"] = frames;
    }
    out["document"] = active;
    return out;
}
void McpController::add(QString name, QString description, QJsonObject properties, QJsonArray required,
    Access access, Handler handler, bool destructive)
{
    const bool documentScoped = access == Access::Document || access == Access::DocumentQuery || access == Access::Interaction;
    const bool readOnly = access == Access::Query || access == Access::DocumentQuery;
    if (documentScoped) {
        properties["documentId"] = identifier("Active document ID from vulkana_get_state. Never inferred from the layer name.");
        properties["expectedRevision"] = QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128},
            {"description", "Optional opaque revision token from state/result; covers content, geometry and pixel selection. Copy it exactly. Stale revisions reject without changes."}};
        required.append("documentId");
    }
    registry_.add({name, description, object(properties, required),
        {{"readOnlyHint", readOnly}, {"destructiveHint", destructive}, {"idempotentHint", readOnly}, {"openWorldHint", false}},
        [this, name, access, documentScoped, readOnly, handler = std::move(handler)](const QJsonObject& args) {
            if (QThread::currentThread() != thread()) return mcp::toolFailure("wrong_thread", "Editor tools must run on the UI thread.");
            if (executing_) return mcp::toolFailure("busy", "Another MCP command is still executing; query state before retrying.");
            if (documentScoped) {
                if (!session().document() || id(args, "documentId") != window_.activeDocumentId())
                    return mcp::toolFailure("wrong_document", "The active document changed. Query state and explicitly activate the intended document.", state(false));
                if (args.contains("expectedRevision") && args["expectedRevision"].toString() != revisionToken(document())) {
                    auto details = state(false);
                    details["changed"] = false;
                    details["noChanges"] = true;
                    details["recovery"] = "No changes were made. Query current state and reconsider the edit using its current revision before retrying.";
                    return mcp::toolFailure("stale_revision", "The document changed. Query its current state before editing.", details);
                }
            }
            if (access != Access::Query && access != Access::Interaction) {
                const auto blocked = blockers(access == Access::CreateDocument);
                if (!blocked.isEmpty()) return mcp::toolFailure("blocked", "Editing is blocked by an active interaction.", {{"blockers", blocked}});
            }
            if (access == Access::Interaction && (QApplication::activeModalWidget() || QApplication::activePopupWidget()))
                return mcp::toolFailure("blocked", "Resolve the native modal dialog or popup before changing its underlying interaction.", {{"blockers", blockers()}});
            const QScopedValueRollback guard(executing_, true);
            const QScopedValueRollback publicationGuard(publicationAttempted_, false);
            // Session changes also matter: selection, colors and view do not
            // change the document revision. This is a defensive check on the
            // explicit preflight contract, never proof about filesystem work.
            const auto previousState = state(false);
            const auto failure = [&](const std::exception& error, const PreflightError* preflight = nullptr) {
                const bool didChange = state(false) != previousState;
                auto details = result(didChange);
                if (preflight && !publicationAttempted_ && !didChange) {
                    details["noChanges"] = true;
                    details["recovery"] = preflight->invalidArguments()
                        ? "No changes were made. Correct the named arguments and retry."
                        : "No changes were made. Resolve the preparation failure before retrying.";
                    return mcp::toolFailure(preflight->invalidArguments() ? "invalid_arguments" : "operation_failed",
                        QString::fromUtf8(error.what()), details);
                }
                details["recovery"] = "Query current state before retrying; a command may have committed before presentation failed.";
                return mcp::toolFailure("operation_failed", QString::fromUtf8(error.what()), details);
            };
            try {
                auto data = handler(args);
                const auto image = data.take("_mcpImage").toString();
                auto response = mcp::toolSuccess(data);
                if (!image.isEmpty()) {
                    auto content = response["content"].toArray();
                    content.append(QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", image}});
                    response["content"] = content;
                }
                if (!readOnly) window_.statusBar()->showMessage(QStringLiteral("MCP · ") + name, 2500);
                return response;
            } catch (const PreflightError& error) {
                return failure(error, &error);
            } catch (const std::exception& error) {
                return failure(error);
            }
        }});
}
} // namespace imageeditor::ui
