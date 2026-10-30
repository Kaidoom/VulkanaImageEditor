#include "imageeditor/ui/McpController.hpp"
#include "McpToolHelpers.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/RasterLimits.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/GifImport.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QApplication>
#include <QBuffer>
#include <QDialog>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QPainter>
#include <QScopedValueRollback>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>

namespace imageeditor::ui {
using namespace mcpdetail;
namespace {
QString localPath(const QJsonObject& args)
{
    const auto path = args.value("path").toString();
    require(!path.contains(QChar::Null) && QFileInfo(path).isAbsolute(), "path must be an absolute local filesystem path");
    return QDir::cleanPath(path);
}
void writableDestination(const QString& path, bool overwrite)
{
    const QFileInfo info(path);
    require(QDir(info.absolutePath()).exists(), "The output directory does not exist");
    require((!info.exists() && !info.isSymLink()) || (info.isFile() && overwrite), "Destination exists; supply overwrite:true to explicitly replace it");
}
// Stage new files beside their destination. QFile::rename refuses an existing
// destination, including one created while encoding; never truncate a newly
// appeared file just because the caller declined overwrite.
void writeDestination(const QString& path, bool overwrite, const std::function<void(const QString&)>& write)
{
    writableDestination(path, overwrite);
    if (overwrite) { write(path); return; }
    QTemporaryDir stage(QFileInfo(path).absolutePath() + "/.vulkana-mcp-XXXXXX");
    require(stage.isValid(), "Could not create output staging directory");
    const auto temporary = stage.filePath(QFileInfo(path).fileName());
    write(temporary);
    writableDestination(path, false);
    require(QFile::rename(temporary, path), "Could not publish output without overwriting; the destination may have appeared meanwhile");
}
QString png(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    require(image.save(&buffer, "PNG"), "Could not encode the preview");
    return QString::fromLatin1(bytes.toBase64());
}
}
void McpController::registerDocumentTools()
{
    add("vulkana_get_state", "Query open documents, active canvas, stable layer IDs, bottom-to-top tree, selection, undo, snapshots and UI blockers. Safe while dialogs or edits are active. Start here.",
        {{"includeLayers", boolean("Include the active document's layer tree; default true.")}}, {}, Access::Query,
        [this](const QJsonObject& args) { return state(args.value("includeLayers").toBool(true)); });
    add("vulkana_document_create", "Create a new document tab with one raster layer. Only the untouched launch placeholder may be replaced; explicit or edited documents remain open. Coordinates and dimensions are pixels. Background defaults to transparent.",
        {{"width", integer(1, int(kMaximumRasterDimension))}, {"height", integer(1, int(kMaximumRasterDimension))},
            {"name", string()}, {"background", colorSchema()}, {"dpi", number(1, 9600)}}, {"width", "height"}, Access::Session,
        [this](const QJsonObject& args) {
            const core::Extent2u extent{std::uint32_t(args["width"].toInt()), std::uint32_t(args["height"].toInt())};
            require(rasterExtentWithinLimits(extent.width, extent.height), "Canvas exceeds the raster allocation limit");
            auto doc = std::make_unique<core::Document>(core::CanvasSpec{extent, args.value("dpi").toDouble(96)});
            const auto background = args.contains("background") ? color(args["background"].toString()) : core::Rgba8{};
            auto layer = core::Layer::raster("Background", std::make_shared<core::ContiguousRasterSurface>(extent, background));
            require(doc->insertLayer(0, std::move(layer)), "Could not create the initial layer");
            require(window_.initializeDocument(std::move(doc), args.value("name").toString("Untitled")), "Could not activate the new document");
            return state();
        });
    add("vulkana_document_activate", "Activate an existing document tab by its documentId. Does not discard any document.",
        {{"documentId", identifier()}}, {"documentId"}, Access::Session,
        [this](const QJsonObject& args) {
            require(window_.documentContext(id(args, "documentId")) != nullptr, "Unknown document ID");
            require(window_.activateDocument(id(args, "documentId")), "Could not activate document");
            return state();
        });
    add("vulkana_document_open", "Open an existing .vulkana project, GIF animation, or raster image in a new tab. PSD/PDF review imports are not automated; open those in the editor. Only the untouched launch placeholder may be replaced.",
        {{"path", string("Absolute existing local file path.")}}, {"path"}, Access::Session,
        [this](const QJsonObject& args) {
            const auto path = localPath(args);
            require(QFileInfo(path).isFile(), "Document file does not exist");
            const auto suffix = QFileInfo(path).suffix().toLower();
            require(suffix != "psd" && suffix != "psb" && suffix != "pdf", "PSD/PDF imports require interactive review; open them in Vulkana and then query state");
            if (suffix == "vulkana") {
                auto loaded = loadProject(path);
                require(bool(loaded), loaded.error);
                require(window_.initializeDocument(std::move(loaded.document), QFileInfo(path).fileName(), path, loaded.metadata), "Could not open project");
                auto data = state(); data["warnings"] = QJsonArray::fromStringList(loaded.warnings); return data;
            }
            if (isGifFile(path)) {
                auto loaded = loadGifDocument(path, "Frame");
                require(bool(loaded), loaded.error);
                require(window_.initializeDocument(std::move(loaded.document), QFileInfo(path).fileName(), {}, {}, path), "Could not open GIF");
            } else {
                auto loaded = loadRasterDocument(path);
                require(bool(loaded), loaded.error);
                require(window_.initializeDocument(std::move(loaded.document), QFileInfo(path).fileName(), {}, {}, path), "Could not open image");
            }
            return state();
        });
    add("vulkana_import_layer", "Import a raster image as a new layer at the top of the active document, preserving existing layers and selection. For GIF animation use document_open instead.",
        {{"path", string()}, {"name", string()}}, {"path"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto path = localPath(args), suffix = QFileInfo(path).suffix().toLower();
            require(QFileInfo(path).isFile(), "Image file does not exist");
            require(suffix != "psd" && suffix != "psb" && suffix != "pdf" && suffix != "vulkana" && !isGifFile(path), "This tool accepts static raster images only");
            auto loaded = loadRasterLayer(path);
            require(bool(loaded), loaded.error);
            if (args.contains("name")) loaded.layer->name = args["name"].toString().toStdString();
            const auto layerId = loaded.layer->id;
            require(execute(std::make_unique<core::AddLayerCommand>(std::move(*loaded.layer), document().layers().size(), session().activeLayer())), "Could not add image layer");
            session().setActiveLayer(layerId); changed();
            auto data = result(); data["layerId"] = id(layerId); return data;
        });
    add("vulkana_document_save", "Save the active document as a native .vulkana project. Explicit path required; overwrite defaults false. This writes a file and marks the document saved, not an undo action.",
        {{"path", string()}, {"overwrite", boolean()}}, {"path"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto path = localPath(args);
            require(QFileInfo(path).suffix().compare("vulkana", Qt::CaseInsensitive) == 0, "Native projects require a .vulkana extension");
            writableDestination(path, args.value("overwrite").toBool());
            auto metadata = window_.fileState().metadata;
            auto ui = metadata["ui"].toObject(), layers = ui["layers"].toObject();
            QJsonArray collapsed;
            for (const auto layerId : window_.layerModel_->collapsedFolderIds()) collapsed.append(id(layerId));
            layers["collapsedFolders"] = collapsed; ui["layers"] = layers; metadata["ui"] = ui;
            {
                const QScopedValueRollback busy(window_.fileBusy_, true);
                writeDestination(path, args.value("overwrite").toBool(), [&](const QString& destination) {
                    const auto saved = saveProject(destination, document(), metadata);
                    require(bool(saved), saved.error);
                });
            }
            window_.fileState().projectPath = path; window_.fileState().metadata = metadata;
            window_.fileState().displayName = QFileInfo(path).fileName();
            document().markSaved(); window_.updateDocumentTitle(); window_.refreshDocumentTabs();
            auto data = result(false); data["path"] = path; data["saved"] = true; return data;
        }, true);
    add("vulkana_document_close", "Close the active document. Modified documents are refused unless discardChanges:true is explicitly supplied. Saving first is recommended. Not undoable.",
        {{"discardChanges", boolean()}}, {}, Access::Document,
        [this](const QJsonObject& args) {
            require(!document().isModified() || args.value("discardChanges").toBool(), "Document has unsaved changes; save it or explicitly request discardChanges:true");
            const QScopedValueRollback prompts(window_.unsavedPromptEnabled_, false);
            require(window_.closeDocument(window_.activeDocumentId()), "Could not close document");
            return state();
        }, true);
    add("vulkana_history", "Undo or redo one document action. Tool changes, view changes and filesystem writes are not undoable.",
        {{"operation", choice({"undo", "redo"})}}, {"operation"}, Access::Document,
        [this](const QJsonObject& args) {
            const auto before = document().canvas();
            const bool applied = args["operation"] == "undo" ? session().undo() : session().redo();
            if (applied) { window_.adjustViewAfterCanvasResize(before); changed(); }
            auto data = result(applied); data["undoDepth"] = int(session().history().undoDepth()); data["redoDepth"] = int(session().history().redoDepth()); return data;
        }, true);
    add("vulkana_canvas_resize", "Resize canvas boundaries without resampling layers. x/y specify the new top-left boundary in old document pixels; artwork is rebased to stay in place. One undo action.",
        {{"width", integer(1, int(kMaximumRasterDimension))}, {"height", integer(1, int(kMaximumRasterDimension))},
            {"x", integer(-int(kMaximumRasterDimension), int(kMaximumRasterDimension))}, {"y", integer(-int(kMaximumRasterDimension), int(kMaximumRasterDimension))}},
        {"width", "height"}, Access::Document, [this](const QJsonObject& args) {
            require(rasterExtentWithinLimits(std::uint32_t(args["width"].toInt()), std::uint32_t(args["height"].toInt())), "Canvas exceeds the raster limit");
            const auto before = document().canvas();
            const bool applied = execute(std::make_unique<core::ReframeCanvasCommand>(core::RectI{args["x"].toInt(), args["y"].toInt(), args["width"].toInt(), args["height"].toInt()}));
            if (applied) { window_.adjustViewAfterCanvasResize(before); changed(); }
            return result(applied);
        });
    add("vulkana_canvas_view", "Change only the canvas view: fit, reset to 100%, or zoom and center on a document point. Does not change image pixels.",
        {{"operation", choice({"fit", "reset", "set"})}, {"zoom", number(.01, 64)}, {"centerX", number(-1e7, 1e7)}, {"centerY", number(-1e7, 1e7)}},
        {"operation"}, Access::Document, [this](const QJsonObject& args) {
            const auto op = args["operation"].toString();
            if (op == "set") {
                require(args.contains("zoom") && args.contains("centerX") && args.contains("centerY"), "set requires zoom, centerX and centerY");
                const auto size = window_.canvasWindow_->scene().logicalViewport;
                const QPointF center(size.width * .5, size.height * .5);
                window_.canvasWindow_->setZoomAt(args["zoom"].toDouble(), center);
                window_.canvasWindow_->centerDocumentAt({args["centerX"].toDouble(), args["centerY"].toDouble()}, center);
            } else {
                require(!args.contains("zoom") && !args.contains("centerX") && !args.contains("centerY"), "Zoom/center fields are only valid with operation:set");
                if (op == "fit") window_.canvasWindow_->fitDocumentToView(); else window_.canvasWindow_->resetTo100Percent();
            }
            return state(false);
        });
    add("vulkana_canvas_preview", "Return a composited PNG image for visual verification, without UI chrome. Layers, masks, blends and effects are included. Overview is fast; exact uses the export compositor. The returned dimensions may differ from document pixels.",
        {{"maxSize", integer(32, int(maximumOverviewDimension), "Longest output side in pixels, 32–2048 for both qualities; aspect ratio is preserved. Default: 1024.")},
            {"quality", choice({"overview", "exact"}, "Default: overview. Both qualities support the full maxSize range.")},
            {"background", choice({"checkerboard", "transparent", "white", "black"}, "Default: checkerboard.")}}, {}, Access::DocumentQuery,
        [this](const QJsonObject& args) {
            const auto extent = document().canvas().extent;
            const int maximum = args.value("maxSize").toInt(1024);
            const auto size = QSize(int(extent.width), int(extent.height)).scaled(maximum, maximum, Qt::KeepAspectRatio);
            const core::Extent2u output{std::uint32_t(std::max(1, size.width())), std::uint32_t(std::max(1, size.height()))};
            auto preview = args.value("quality").toString() == "exact" ? flattenDocumentAtSize(document(), output) : renderDocumentOverview(document(), output);
            require(bool(preview), preview.error);
            const auto background = args.value("background").toString("checkerboard");
            if (background != "transparent") {
                QImage image(preview.image.size(), QImage::Format_RGBA8888);
                image.fill(background == "black" ? Qt::black : Qt::white);
                QPainter painter(&image);
                if (background == "checkerboard") for (int y = 0; y < image.height(); y += 12)
                    for (int x = 0; x < image.width(); x += 12)
                        painter.fillRect(x, y, 12, 12, ((x / 12 + y / 12) % 2) ? QColor(120, 120, 120) : QColor(170, 170, 170));
                painter.drawImage(0, 0, preview.image); painter.end(); preview.image = image;
            }
            auto data = result(false); data["width"] = preview.image.width(); data["height"] = preview.image.height();
            data["documentWidth"] = int(extent.width); data["documentHeight"] = int(extent.height);
            data["_mcpImage"] = png(preview.image); return data;
        });
    add("vulkana_export_image", "Export the composited canvas to PNG, JPEG or WebP without a dialog. Explicit overwrite required for existing files; does not save or mark the project saved. PSD/PDF/GIF export stays interactive in this version.",
        {{"path", string()}, {"format", choice({"png", "jpeg", "webp"})}, {"overwrite", boolean()},
            {"width", integer(1, kMaximumExportDimension)}, {"height", integer(1, kMaximumExportDimension)},
            {"quality", integer(1, 100)}, {"lossless", boolean("WebP only.")}, {"matte", colorSchema()}}, {"path", "format"}, Access::Document,
        [this](const QJsonObject& args) {
            ExportSettings settings;
            settings.destination = localPath(args);
            writableDestination(settings.destination, args.value("overwrite").toBool());
            const auto format = args["format"].toString();
            settings.format = format == "png" ? ExportFormat::Png : format == "jpeg" ? ExportFormat::Jpeg : ExportFormat::WebP;
            require(!args.contains("lossless") || format == "webp", "lossless is only valid for WebP");
            require(!args.contains("quality") || format != "png", "PNG is lossless and does not use quality");
            require(args.contains("width") == args.contains("height"), "Specify both width and height or neither");
            const auto extent = document().canvas().extent;
            settings.size = args.contains("width") ? QSize(args["width"].toInt(), args["height"].toInt()) : QSize(int(extent.width), int(extent.height));
            settings.jpegQuality = args.value("quality").toInt(92); settings.webpQuality = args.value("quality").toInt(90);
            settings.webpLossless = args.value("lossless").toBool();
            if (args.contains("matte")) {
                require(format != "webp", "Matte is supported for PNG/JPEG; WebP preserves transparency");
                const auto value = color(args["matte"].toString());
                require(value.alpha == 255, "The export matte must be opaque");
                settings.pngMatte = true; settings.pngMatteColor = settings.jpegMatteColor = QColor(value.red, value.green, value.blue);
            }
            const auto destinationError = validateExportDestination(settings.destination, window_.projectPath());
            require(destinationError.isEmpty(), destinationError);
            const auto error = validateExport(settings, {int(extent.width), int(extent.height)});
            require(error.isEmpty(), error);
            const auto image = renderExport(document(), settings); require(bool(image), image.error);
            const std::atomic_bool cancel{false};
            const auto encoded = encodeExport(image.image, settings, cancel); require(bool(encoded), encoded.error);
            writeDestination(settings.destination, args.value("overwrite").toBool(), [&](const QString& destination) {
                const auto written = writeExportAtomically(destination, encoded.bytes, cancel); require(bool(written), written.error);
            });
            auto data = result(false); data["path"] = settings.destination; data["bytes"] = encoded.bytes.size(); return data;
        }, true);
    add("vulkana_finish_text", "Explicitly finish interactive text editing, retaining the current text. Other pending interactions are not accepted or discarded.", {}, {}, Access::Interaction,
        [this](const QJsonObject&) {
            require(!window_.fileBusy_ && !window_.graphicsStartupPending_ && !window_.workspace_->hasModalOverlay()
                && !QApplication::activeModalWidget() && !QApplication::activePopupWidget(), "A dialog or file operation must be resolved first");
            require(window_.textController_ && window_.textController_->active(), "No text edit is active");
            window_.textController_->finish(); return state();
        });
    add("vulkana_cancel_interaction", "Explicitly cancel a known preview or editor dialog. Never accepts a confirmation, discards a document, or cancels running I/O. Query blockers first; kind must match. Text finishing is separate.",
        {{"kind", choice({"workspace_dialog", "snapshot_preview", "refine_selection", "background_removal", "transform"})}}, {"kind"}, Access::Interaction,
        [this](const QJsonObject& args) {
            require(!window_.fileBusy_ && !window_.graphicsStartupPending_ && !window_.cloneProcessing_ && !window_.localBlurProcessing_ && !window_.spotHealJob_, "Processing must finish before cancelling an interaction");
            const auto kind = args["kind"].toString();
            if (kind == "workspace_dialog") {
                require(window_.workspaceDialog_ && window_.workspaceDialog_->isVisible(), "No workspace dialog is open");
                window_.workspaceDialog_->reject();
            } else if (kind == "snapshot_preview") { require(bool(window_.snapshotPreview_), "No snapshot preview"); window_.finishSnapshotPreview(); }
            else if (kind == "refine_selection") { require(bool(window_.refinement_), "No refinement preview"); window_.finishRefinement(false); }
            else if (kind == "background_removal") { require(bool(window_.backgroundRemoval_), "No background removal preview"); window_.finishBackgroundRemoval(false); }
            else {
                require(bool(window_.layerTransform_) || bool(window_.selectionTransform_) || bool(window_.canvasResize_), "No pending transform");
                if (window_.layerTransform_) window_.finishLayerTransform(false);
                if (window_.selectionTransform_) window_.finishSelectionTransform(false);
                if (window_.canvasResize_) window_.finishQuickCanvasResize(false);
            }
            return state();
        }, true);
}
} // namespace imageeditor::ui
