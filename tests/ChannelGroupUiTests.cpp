#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/ScalarClipboard.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QListView>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int failures{};
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

void clipboardCodec()
{
    c::ScalarFragment fragment;
    fragment.extent = {16, 16}; fragment.pixelsToDocument = {1, 0, 7, 0, 1, -3};
    fragment.values.resize(256); fragment.coverage.resize(256);
    for (int i = 0; i < 256; ++i) { fragment.values[std::size_t(i)] = std::byte(i); fragment.coverage[std::size_t(i)] = std::byte(255 - i); }
    const auto encoded = u::encodeScalarClipboard(fragment);
    const auto decoded = u::decodeScalarClipboard(encoded);
    CHECK(decoded);
    if (decoded) {
        CHECK(decoded->extent == fragment.extent); CHECK(decoded->pixelsToDocument == fragment.pixelsToDocument);
        CHECK(decoded->values == fragment.values); CHECK(decoded->coverage == fragment.coverage);
    }
    CHECK(!u::decodeScalarClipboard(encoded.first(encoded.size() - 1)));
    CHECK(!u::decodeScalarClipboard(encoded + "x"));
    auto damaged = encoded; damaged[0] = 0; CHECK(!u::decodeScalarClipboard(damaged));
    damaged = encoded; for (int i = 8; i < 16; ++i) damaged[i] = char(255);
    CHECK(!u::decodeScalarClipboard(damaged));
}

void dataExportControls()
{
    u::ExportSettings settings;
    settings.size = {32, 24}; settings.preserveData = true; settings.dataLayerId = 123;
    QWidget owner;
    u::ExportDialog dialog(settings, settings.size, &owner);
    CHECK(dialog.windowType() == Qt::SubWindow);
    CHECK(!dialog.isWindow() && dialog.parentWidget() == &owner);
    auto* format = dialog.findChild<QComboBox*>("ExportFormat"); CHECK(format && !format->isEnabled());
    const auto before = dialog.settings();
    for (auto* spin : dialog.findChildren<QDoubleSpinBox*>()) {
        if (spin->objectName() == "ExportWidth" || spin->objectName() == "ExportHeight" || spin->objectName() == "ExportScale")
            CHECK(!spin->isEnabled());
    }
    CHECK(dialog.settings().preserveData && dialog.settings().dataLayerId == before.dataLayerId);
}

void workflow()
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("channels.vulkana");
    c::Document source(c::CanvasSpec {{32, 24}});
    auto pixels = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {32, 24}, c::Rgba8 {20, 100, 220, 128});
    auto layer = c::Layer::raster("Texture", pixels); const auto sourceId = layer.id;
    CHECK(source.insertLayer(0, std::move(layer))); CHECK(u::saveProject(path, source));
    u::MainWindow window(nullptr, false, false); window.setUnsavedPromptEnabled(false);
    window.resize(1280, 800); window.show(); CHECK(window.openImageFromPath(path)); QTest::qWait(30);
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    auto* doc = session.document(); CHECK(doc); if (!doc) return;
    const auto find = [&](const char* name) { auto* a = window.findChild<QAction*>(name); CHECK(a); return a; };
    auto* convert = find("ConvertChannelGroupAction");
    auto* recombine = find("RecombineChannelGroupAction");
    auto* alpha = find("InspectChannelAlphaAction");
    auto* copy = find("CopyScalarChannelAction");
    auto* cut = find("CutScalarChannelAction");
    auto* paste = find("PasteImageAsLayerAction");
    auto* view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList")); CHECK(view); if (!view) return;
    auto* model = dynamic_cast<u::LayerListModel*>(view->model()); CHECK(model); if (!model) return;
    const auto choose = [&](c::LayerId id) { view->onRowSelectionRequested(model->rowForLayer(id), Qt::NoModifier); };
    choose(sourceId); CHECK(convert && convert->isEnabled()); if (!convert) return;
    const auto originalColors = session.colors();
    convert->trigger();
    const auto* group = doc->tree().container(sourceId); CHECK(group); if (!group) return;
    CHECK(group->kind == c::ContainerKind::ChannelGroup);
    const auto roles = group->channelRoles;
    imageeditor::render::CanvasWindow* canvas = nullptr;
    for (auto* candidate : QGuiApplication::allWindows())
        if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
            canvas = dynamic_cast<imageeditor::render::CanvasWindow*>(candidate);
    CHECK(canvas);
    CHECK(recombine->isEnabled()); CHECK(alpha->isEnabled()); CHECK(!alpha->isChecked());
    if (canvas) CHECK(canvas->scene().channelInspectionId == sourceId);
    choose(roles[0]);
    auto* panel = dynamic_cast<u::ColorPanel*>(window.findChild<QWidget*>("ColorPanelContent")); CHECK(panel && panel->scalarMode());
    auto* value = window.findChild<QDoubleSpinBox*>("ScalarChannelValue"); CHECK(value);
    if (value) value->setValue(77);
    CHECK(session.foregroundColor() == (c::Rgba8 {77, 77, 77, 255}));
    if (canvas) CHECK(canvas->scene().channelInspectionId == roles[0]);
    const auto row = model->index(model->rowForLayer(roles[0]));
    CHECK(row.data(Qt::DisplayRole).toString().startsWith("R · "));
    CHECK(row.data(Qt::EditRole).toString() == "Red");
    CHECK(!row.data(Qt::CheckStateRole).isValid());
    CHECK(!(row.flags() & Qt::ItemIsDragEnabled));
    CHECK(!(row.flags() & Qt::ItemIsUserCheckable));
    CHECK(model->renameItem(roles[0], "Roughness"));
    CHECK(doc->tree().container(sourceId)->channelRoles == roles);
    CHECK(doc->layer(roles[0])->name == "Roughness");
    CHECK(copy->isEnabled() && cut->isEnabled());
    const auto screenshot = qEnvironmentVariable("VULKANA_CHANNEL_UI_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QTest::qWait(150); // Settle scroll-body layout and asynchronous thumbnail updates.
        auto* overlay = window.findChild<QWidget*>("PanelOverlaySurface"); CHECK(overlay);
        if (overlay) CHECK(overlay->grab().save(screenshot));
    }
    CHECK(!find("AddLayerMaskAction")->isEnabled());
    CHECK(!find("ToolAction_smartselect")->isEnabled());
    CHECK(!find("ToolAction_colorselect")->isEnabled());
    CHECK(!find("DeleteSelectedLayersAction")->isEnabled());
    auto* opacity = window.findChild<QDoubleSpinBox*>("LayerOpacitySlider"); CHECK(opacity && !opacity->isEnabled());
    auto* blend = window.findChild<QComboBox*>("LayerBlendModeCombo"); CHECK(blend && !blend->isEnabled());
    const auto state = doc->contentState(); const auto depth = session.history().undoDepth();
    copy->trigger();
    const auto* mime = QApplication::clipboard()->mimeData(); CHECK(mime && mime->hasFormat(u::scalarClipboardMime));
    CHECK(doc->contentState() == state && session.history().undoDepth() == depth);
    cut->trigger();
    const auto red = std::get<c::RasterLayer>(doc->layer(roles[0])->payload).surface;
    std::byte cutValue {20};
    for (int i = 0; i < 100 && cutValue != std::byte{0}; ++i) {
        QTest::qWait(5); red->copyNative({0, 0, 1, 1}, {&cutValue, 1}, 1);
    }
    CHECK(cutValue == std::byte{0});
    CHECK(session.history().undoDepth() == depth + 1);
    find("UndoAction")->trigger();
    choose(roles[1]); paste->trigger();
    CHECK(session.history().undoDepth() == depth + 1);
    const auto green = std::get<c::RasterLayer>(doc->layer(roles[1])->payload).surface;
    std::byte sample{}; green->copyNative({0, 0, 1, 1}, {&sample, 1}, 1); CHECK(sample == std::byte{20});
    find("UndoAction")->trigger();
    green->copyNative({0, 0, 1, 1}, {&sample, 1}, 1); CHECK(sample == std::byte{100});
    const auto untouched = doc->contentState();
    QApplication::clipboard()->setText(QStringLiteral("not channel data")); paste->trigger();
    CHECK(doc->contentState() == untouched);
    view->onAlphaSelectionRequested(model->rowForLayer(roles[0]));
    for (int i = 0; i < 100 && !doc->selection(); ++i) QTest::qWait(5);
    CHECK(doc->selection() && doc->selection()->coverageAtDocumentPixel(4, 4) == 20);
    choose(sourceId); CHECK(panel && !panel->scalarMode()); CHECK(session.colors() == originalColors);
    CHECK(recombine->isEnabled()); recombine->trigger();
    CHECK(doc->layer(sourceId) && c::isPackedDataLayer(*doc->layer(sourceId)));
    CHECK(!doc->tree().container(sourceId));
    CHECK(find("ToolAction_smartselect")->isEnabled());
    if (canvas) CHECK(!canvas->scene().channelInspectionId);
    window.close();
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv); QStandardPaths::setTestModeEnabled(true);
    u::applyEditorTheme(app, {});
    QCoreApplication::setOrganizationName("VulkanaChannelTests");
    QCoreApplication::setApplicationName("ChannelUi");
    try { clipboardCodec(); dataExportControls(); workflow(); }
    catch (const std::exception& e) { CHECK(false); std::cerr << e.what() << '\n'; }
    return failures ? 1 : 0;
}
