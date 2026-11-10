#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/ScalarClipboard.hpp"
#include "imageeditor/ui/SwatchesPanel.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QScopedValueRollback>
#include <QPushButton>
#include <QStatusBar>

namespace imageeditor::ui {
void MainWindow::createChannelGroupActions()
{
    const auto action = [this](const char* name, const char* label, ToolGlyph glyph, const auto& callback) {
        auto* value = new QAction(toolGlyph(glyph), QString::fromUtf8(label), this);
        value->setObjectName(QString::fromLatin1(name));
        connect(value, &QAction::triggered, this, callback);
        registerEditorWindowAction(value);
        return value;
    };
    convertChannelGroupAction_ = action("ConvertChannelGroupAction", "Convert to Channel Group", ToolGlyph::Group,
        [this] { convertSelectedChannelGroup(false); });
    recombineChannelGroupAction_ = action("RecombineChannelGroupAction", "Convert Channel Group to Raster", ToolGlyph::LayerRaster,
        [this] { convertSelectedChannelGroup(true); });
    inspectChannelAlphaAction_ = action("InspectChannelAlphaAction", "Show alpha as transparency", ToolGlyph::LayerMask,
        [this] { refreshChannelGroupControls(); });
    inspectChannelAlphaAction_->setCheckable(true);
    copyScalarChannelAction_ = action("CopyScalarChannelAction", "Copy channel pixels", ToolGlyph::Copy,
        [this] { copyScalarChannel(false); });
    cutScalarChannelAction_ = action("CutScalarChannelAction", "Cut channel pixels", ToolGlyph::Subtract,
        [this] { copyScalarChannel(true); });
    copyScalarChannelAction_->setShortcut(QKeySequence::Copy);
    cutScalarChannelAction_->setShortcut(QKeySequence::Cut);
    action("OpenDataTextureAction", "Open data texture…", ToolGlyph::Import, [this] { openDataTexture(); });
    exportChannelDataAction_ = action("ExportChannelDataAction", "Export channel data…", ToolGlyph::Export,
        [this] { exportChannelData(); });
}

void MainWindow::refreshChannelGroupControls()
{
    if (!convertChannelGroupAction_) return;
    const auto* doc = session().document();
    const auto id = session().activeLayer();
    const auto* layer = doc && id ? doc->layer(*id) : nullptr;
    const auto* container = doc && id ? doc->tree().container(*id) : nullptr;
    const bool scalar = layer && core::isScalarLayer(*layer);
    const bool group = container && container->kind == core::ContainerKind::ChannelGroup;
    const bool data = scalar || group || (layer && core::isPackedDataLayer(*layer));
    const bool available = doc && id && session().selectedLayers().size() == 1
        && !fileBusy_ && !workspaceDialog_ && !refinement_ && !backgroundRemoval_;
    std::string reason;
    convertChannelGroupAction_->setEnabled(available && core::canConvertToChannelGroup(*doc, *id, &reason));
    convertChannelGroupAction_->setToolTip(QString::fromStdString(reason));
    recombineChannelGroupAction_->setEnabled(available && group);
    inspectChannelAlphaAction_->setEnabled(available && group);
    copyScalarChannelAction_->setEnabled(available && scalar && !session().editingLayerMask());
    cutScalarChannelAction_->setEnabled(copyScalarChannelAction_->isEnabled());
    exportChannelDataAction_->setEnabled(available && data);
    if (scalar != scalarColorMode_) {
        if (scalar) {
            ordinaryColors_ = session().colors(); scalarColorMode_ = true;
            setColors(scalarColors_);
        } else {
            scalarColors_ = session().colors(); scalarColorMode_ = false;
            if (ordinaryColors_) setColors(*ordinaryColors_);
            ordinaryColors_.reset();
        }
    }
    if (colorPanel_) colorPanel_->setScalarMode(scalar);
    canvasWindow_->setChannelInspection((scalar || group) ? id : std::nullopt,
        group && inspectChannelAlphaAction_->isChecked());

    // Actions remain visible with their normal layout, but unsupported numeric
    // operations cannot enter a colour-processing pipeline accidentally.
    for (auto [tool, action] : toolActionMap_) {
        const bool allowed = tool == core::ToolId::Move || tool == core::ToolId::Marquee
            || tool == core::ToolId::Lasso || tool == core::ToolId::Brush
            || tool == core::ToolId::Eraser || tool == core::ToolId::Fill || tool == core::ToolId::Measure
            || tool == core::ToolId::Cloning || tool == core::ToolId::LocalBlur;
        if (scalar && !allowed) {
            if (!action->property("channelDisabled").toBool()) {
                action->setProperty("channelWasEnabled", action->isEnabled());
                action->setProperty("channelToolTip", action->toolTip());
                action->setToolTip(action->toolTip() + QStringLiteral("\nUnavailable for numeric channels."));
            }
            action->setProperty("channelDisabled", true); action->setEnabled(false);
        } else if (action->property("channelDisabled").toBool()) {
            action->setProperty("channelDisabled", false);
            action->setToolTip(action->property("channelToolTip").toString());
            action->setEnabled(action->property("channelWasEnabled").toBool()
                && !fileBusy_ && !workspaceDialog_ && !refinement_ && !backgroundRemoval_);
        }
    }
    if (scalar) {
        for (auto* action : {transformAction_, transformPixelsAction_, duplicateLayersAction_, deleteLayerAction_,
                 groupLayersAction_, clippingGroupAction_, mergeLayersAction_, rasterizeLayersAction_,
                 newFolderAction_, newAdjustmentLayerAction_, selectionActions_[3]})
            if (action) action->setEnabled(false);
        for (auto* action : layerVisibilityActions_) if (action) action->setEnabled(false);
        for (auto* action : maskActions_) if (action) action->setEnabled(false);
        if (deleteLayerButton_) deleteLayerButton_->setEnabled(false);
        if (effectsButton_) effectsButton_->setEnabled(false);
        if (addMaskButton_) addMaskButton_->setEnabled(false);
        if (swatchesPanel_) swatchesPanel_->setGradientEnabled(false);
        if (moveOptionsPage_) moveOptionsPage_->setEnabled(false);
    }
    if (group) {
        for (auto* action : {ungroupLayersAction_, clippingGroupAction_, mergeLayersAction_, rasterizeLayersAction_})
            if (action) action->setEnabled(false);
    }
    if (adjustmentsPanel_) adjustmentsPanel_->setEnabled(!scalar);
    if (scalar) {
        const auto active = toolActionMap_.find(session().activeTool());
        if (active != toolActionMap_.end() && active->second->property("channelDisabled").toBool())
            setActiveTool(core::ToolId::Brush);
    }
}

void MainWindow::convertSelectedChannelGroup(bool recombine)
{
    if (fileBusy_ || workspaceDialog_ || !session().document() || !settleForFileOperation()) return;
    try {
        auto command = recombine
            ? core::convertChannelGroupToRaster(*session().document(), session().layerSelectionState())
            : core::convertToChannelGroup(*session().document(), session().layerSelectionState());
        if (command && session().execute(std::move(command))) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
    }
}

void MainWindow::copyScalarChannel(bool cut)
{
    if (fileBusy_ || workspaceDialog_ || !session().document() || !session().activeLayer()
        || editorTextInputActive() || (textController_ && textController_->active()) || !settleForFileOperation()) return;
    try {
        const auto owner = activeDocument_;
        const auto target = session().activeLayer();
        const auto revision = session().document()->revision();
        const auto selection = session().document()->selection();
        const auto fragment = core::captureScalarFragment(*session().document(), *session().activeLayer());
        if (!fragment) return;
        const auto bytes = encodeScalarClipboard(*fragment);
        if (bytes.isEmpty()) return;
        auto mime = std::make_unique<QMimeData>();
        mime->setData(scalarClipboardMime, bytes);
        QImage image(int(fragment->extent.width), int(fragment->extent.height), QImage::Format_RGBA8888);
        if (!image.isNull()) {
            for (std::uint32_t y = 0; y < fragment->extent.height; ++y) {
                auto* row = image.scanLine(int(y));
                for (std::uint32_t x = 0; x < fragment->extent.width; ++x) {
                    const auto i = std::size_t(y) * fragment->extent.width + x;
                    row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = std::to_integer<uchar>(fragment->values[i]);
                    row[x * 4 + 3] = std::to_integer<uchar>(fragment->coverage[i]);
                }
            }
            mime->setImageData(image);
        }
        QApplication::clipboard()->setMimeData(mime.release());
        if (cut && activeDocument_ == owner && !owner->closed && session().activeLayer() == target
            && session().document()->revision() == revision && session().document()->selection() == selection)
            startFill(false, true, {}, true);
        else statusBar()->showMessage(QStringLiteral("Channel pixels copied."), 3000);
    } catch (const std::exception& error) { statusBar()->showMessage(QString::fromUtf8(error.what()), 6000); }
}

bool MainWindow::pasteScalarChannel()
{
    const auto* doc = session().document();
    const auto* layer = doc && session().activeLayer() ? doc->layer(*session().activeLayer()) : nullptr;
    if (!layer || !core::isScalarLayer(*layer)) return false;
    try {
        const auto owner = activeDocument_;
        const auto target = layer->id;
        const auto revision = doc->revision();
        const auto placement = core::intrinsicTransform(*layer);
        const auto* mime = QApplication::clipboard()->mimeData();
        auto fragment = mime ? decodeScalarClipboard(mime->data(scalarClipboardMime)) : std::nullopt;
        if (!fragment && mime && mime->hasImage() && !mime->hasFormat(scalarClipboardMime)) {
            QImage image = qvariant_cast<QImage>(mime->imageData());
            auto loaded = !image.isNull() && image.allGray()
                ? rasterLayerFromImage(image, QStringLiteral("Clipboard channel"), RasterImportMode::Data)
                : RasterLayerLoadResult{};
            if (loaded) {
                const auto& source = *std::get<core::RasterLayer>(loaded.layer->payload).surface;
                core::ScalarFragment decoded;
                decoded.extent = source.extent();
                decoded.pixelsToDocument = placement;
                decoded.values.resize(std::size_t(decoded.extent.width) * decoded.extent.height);
                decoded.coverage.resize(decoded.values.size());
                std::vector<std::byte> row(std::size_t(decoded.extent.width) * 4);
                for (std::uint32_t y = 0; y < decoded.extent.height; ++y) {
                    source.copyRgba8({0, int(y), int(decoded.extent.width), 1}, row, row.size());
                    for (std::uint32_t x = 0; x < decoded.extent.width; ++x) {
                        const auto i = std::size_t(y) * decoded.extent.width + x;
                        decoded.values[i] = row[std::size_t(x) * 4];
                        decoded.coverage[i] = row[std::size_t(x) * 4 + 3];
                    }
                }
                if (core::validScalarFragment(decoded)) fragment = std::move(decoded);
            }
        }
        if (!fragment) {
            statusBar()->showMessage(QStringLiteral("Copy channel pixels or an 8-bit straight grayscale image to paste into a channel."), 6000);
            return true;
        }
        if (!settleForFileOperation()) return true;
        if (activeDocument_ != owner || owner->closed || session().activeLayer() != target
            || session().document()->revision() != revision) {
            statusBar()->showMessage(QStringLiteral("The paste target changed. Try pasting again."), 4000);
            return true;
        }
        const auto result = core::pasteScalarFragment(*session().document(), target, *fragment, session().history());
        if (result == core::RasterEditCommitResult::Committed) {
            fileState().untouched = false; synchronizeUi(false, false);
        } else if (result != core::RasterEditCommitResult::NoChanges)
            statusBar()->showMessage(QStringLiteral("Could not paste into the channel. No pixels were changed."), 6000);
    } catch (const std::exception& error) { statusBar()->showMessage(QString::fromUtf8(error.what()), 6000); }
    return true;
}

void MainWindow::openDataTexture()
{
    if (fileBusy_ || workspaceDialog_ || !settleForFileOperation()) return;
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Open data texture"), openDocumentDirectory(),
        QStringLiteral("Lossless images (*.png *.tif *.tiff *.bmp *.tga);;All files (*)"));
    if (path.isEmpty()) return;
    auto loaded = loadRasterDocument(path, RasterImportMode::Data);
    if (!loaded) { reportFileError(loaded.error); return; }
    loaded.document->markUnsaved();
    if (initializeDocument(std::move(loaded.document), QFileInfo(path).fileName())) {
        fileState().untouched = false; rememberOpenedDocument(path); synchronizeUi(true, true);
    }
}

void MainWindow::exportChannelData()
{
    if (!exportChannelDataAction_ || !exportChannelDataAction_->isEnabled()) return;
    const QScopedValueRollback explicitData(explicitChannelDataExport_, true);
    exportImage(false);
}
}
