#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/CloningOptionsPage.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ExportDialog.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/ScalarClipboard.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QListView>
#include <QMimeData>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QStandardItemModel>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QVulkanInstance>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
int failures{};
QVulkanInstance* nativeInstance{};
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

void numericNavigationRestoresColor()
{
    auto color = c::Layer::raster("Colour", std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {8,8}, c::Rgba8 {20,40,80,255}));
    auto scalar = c::Layer::raster("Channel", std::make_shared<c::ScalarRasterSurface>(c::Extent2u {8,8}, 100));
    u::AdjustmentsPanel panel;
    unsigned mutations = 0;
    panel.onPreview = [&](auto) { ++mutations; };
    panel.setTarget(&color, false);
    auto* tabs = panel.findChild<QTabWidget*>("AdjustmentTabs");
    auto* levels = panel.findChild<QComboBox*>("AdjustmentLevelsChannel");
    auto* curves = panel.findChild<QComboBox*>("AdjustmentCurvesChannel");
    auto* filters = panel.findChild<QComboBox*>("FilterNavigation");
    CHECK(tabs && levels && curves && filters);
    if (!tabs || !levels || !curves || !filters) return;
    levels->setCurrentIndex(2); curves->setCurrentIndex(3); filters->setCurrentIndex(1); tabs->setCurrentIndex(1);
    panel.setTarget(&scalar, false);
    CHECK(levels->currentIndex() == 0 && curves->currentIndex() == 0 && filters->currentIndex() == 0);
    CHECK(tabs->currentIndex() == 0 && !tabs->isTabEnabled(1) && !tabs->isTabEnabled(4));
    panel.setTarget(&color, false);
    CHECK(levels->currentIndex() == 2 && curves->currentIndex() == 3 && filters->currentIndex() == 1);
    CHECK(tabs->currentIndex() == 1 && tabs->isTabEnabled(4));
    CHECK(mutations == 0);
}

void workflow()
{
    const bool keyboardOnly = QCoreApplication::arguments().contains(QStringLiteral("--keyboard-only"));
    QTemporaryDir temporary;
    const auto path = temporary.filePath("channels.vulkana");
    c::Document source(c::CanvasSpec {{32, 24}});
    auto pixels = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u {32, 24}, c::Rgba8 {20, 100, 220, 128});
    auto layer = c::Layer::raster("Texture", pixels); const auto sourceId = layer.id;
    CHECK(source.insertLayer(0, std::move(layer))); CHECK(u::saveProject(path, source));
    u::MainWindow window(nativeInstance, false, false); window.setUnsavedPromptEnabled(false);
    window.resize(1280, 800); window.show(); CHECK(window.openImageFromPath(path)); QTest::qWait(30);
    if (nativeInstance) for (int i = 0; i < 400 && !window.startupGraphicsReady(); ++i) QTest::qWait(50);
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
    auto* cloneTool = find("ToolAction_cloning");
    auto* blurTool = find("ToolAction_local_blur");
    auto* cloning = dynamic_cast<u::CloningOptionsPage*>(window.findChild<QWidget*>("CloningOptionsPage")); CHECK(cloning);
    auto* rawSource = window.findChild<QAbstractButton*>("CloneSourceLayer");
    auto* belowSource = window.findChild<QAbstractButton*>("CloneSourceCurrentBelow");
    auto* allSource = window.findChild<QAbstractButton*>("CloneSourceAllVisible");
    CHECK(rawSource && belowSource && allSource);
    if (!keyboardOnly) {
        if (cloneTool) cloneTool->trigger();
        if (allSource) allSource->click();
        CHECK(cloning && cloning->cloneSettings().source == c::CloneSampleSource::AllVisible);
    }
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
    if (!keyboardOnly) {
        CHECK(cloneTool && cloneTool->isEnabled() && blurTool && blurTool->isEnabled());
        CHECK(rawSource && rawSource->isEnabled() && rawSource->isChecked());
        CHECK(belowSource && !belowSource->isEnabled() && allSource && !allSource->isEnabled());
        if (rawSource) rawSource->click();
        CHECK(cloning && cloning->cloneSettings().source == c::CloneSampleSource::AllVisible);
        if (canvas && canvas->onCloneSourcePicked) canvas->onCloneSourcePicked({4.5,4.5});
        choose(roles[1]);
        CHECK(rawSource && rawSource->isChecked());
        CHECK(rawSource && rawSource->accessibleDescription().contains(QStringLiteral("Red")));
        choose(sourceId);
        CHECK(allSource && allSource->isEnabled() && allSource->isChecked());
        CHECK(belowSource && belowSource->isEnabled());
        choose(roles[0]);
        CHECK(rawSource && rawSource->isChecked() && allSource && !allSource->isEnabled());
    }
    auto* panel = dynamic_cast<u::ColorPanel*>(window.findChild<QWidget*>("ColorPanelContent")); CHECK(panel && panel->scalarMode());
    auto* value = window.findChild<QDoubleSpinBox*>("ScalarChannelValue"); CHECK(value);
    if (value) {
        QTest::qWait(50);
        QApplication::setActiveWindow(&window); window.activateWindow();
        QTest::mouseClick(value, Qt::LeftButton, {}, value->rect().center());
        const auto revision = doc->revision();
        QKeyEvent key(QEvent::KeyPress, Qt::Key_7, Qt::NoModifier, QStringLiteral("7"));
        QCoreApplication::sendEvent(window.windowHandle(), &key);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_7, Qt::NoModifier, QStringLiteral("7"));
        QCoreApplication::sendEvent(window.windowHandle(), &release);
        QInputMethodEvent ime; ime.setCommitString(QStringLiteral("7"));
        QCoreApplication::sendEvent(window.windowHandle(), &ime);
        QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(window.windowHandle(), &accept);
        CHECK(value->value() == 77); CHECK(doc->revision() == revision);
    }
    CHECK(session.foregroundColor() == (c::Rgba8 {77, 77, 77, 255}));
    if (canvas) CHECK(canvas->scene().channelInspectionId == roles[0]);
    // Wayland clipboard ownership requires a compositor input serial. The
    // focused host-window keyboard/IME check does not use the clipboard or
    // synthesize retouch operations, and can run independently on Wayland.
    if (keyboardOnly) { window.close(); return; }
    const auto row = model->index(model->rowForLayer(roles[0]));
    CHECK(row.data(Qt::DisplayRole).toString().startsWith("R · "));
    CHECK(row.data(Qt::EditRole).toString() == "Red");
    CHECK(!row.data(Qt::CheckStateRole).isValid());
    CHECK(!(row.flags() & Qt::ItemIsDragEnabled));
    CHECK(!(row.flags() & Qt::ItemIsUserCheckable));
    CHECK(model->renameItem(roles[0], "Roughness"));
    CHECK(doc->tree().container(sourceId)->channelRoles == roles);
    CHECK(doc->layer(roles[0])->name == "Roughness");
    const std::array channelValues {20, 100, 220, 128};
    for (std::size_t i = 0; i < roles.size(); ++i) {
        // Exercise the regular row decoration path, then inspect its cached
        // content icon without row chrome or selection tint. Letterboxing is
        // transparent; the channel itself must remain opaque grayscale.
        (void)model->data(model->index(model->rowForLayer(roles[i])), Qt::DecorationRole);
        const auto image = model->cachedContentThumbnail(roles[i]).pixmap(96, 96).toImage();
        CHECK(!image.isNull());
        if (!image.isNull()) {
            const auto pixel = image.pixelColor(image.width() / 2, image.height() / 2);
            CHECK(pixel == QColor(channelValues[i], channelValues[i], channelValues[i], 255));
            CHECK(image.pixelColor(image.width() / 2, 0).alpha() == 0);
        }
    }
    CHECK(copy->isEnabled() && cut->isEnabled());
    auto* info = window.findChild<QLabel*>("PropertiesTargetName"); CHECK(info && info->text().contains("Numeric channel"));
    for (auto* label : window.findChildren<QLabel*>("ShortcutKeycap"))
        CHECK(label->property("shortcutAction") != QStringLiteral("DuplicateLayersAction"));
    auto* tabs = window.findChild<QTabWidget*>("AdjustmentTabs"); CHECK(tabs);
    CHECK(tabs && tabs->isTabEnabled(0) && !tabs->isTabEnabled(1) && tabs->isTabEnabled(3) && !tabs->isTabEnabled(4));
    auto* levelsChannel = window.findChild<QComboBox*>("AdjustmentLevelsChannel");
    auto* curvesChannel = window.findChild<QComboBox*>("AdjustmentCurvesChannel");
    for (auto* choice : {levelsChannel, curvesChannel}) {
        CHECK(choice && choice->currentIndex() == 0 && choice->itemText(0) == "Value");
        auto* items = choice ? qobject_cast<QStandardItemModel*>(choice->model()) : nullptr;
        CHECK(items && !items->item(1)->isEnabled() && !items->item(2)->isEnabled() && !items->item(3)->isEnabled());
    }
    auto* gamma = window.findChild<QDoubleSpinBox*>("AdjustmentLevelsGamma"); CHECK(gamma && gamma->isEnabled());
    const auto numericBefore = doc->contentState();
    if (gamma) gamma->setValue(1.4);
    CHECK(doc->layer(roles[0])->adjustments && doc->layer(roles[0])->adjustments->items[std::size_t(c::AdjustmentType::Levels)].enabled);
    find("UndoAction")->trigger(); CHECK(doc->contentState() == numericBefore);
    auto* invert = window.findChild<QCheckBox*>(QStringLiteral("AdjustmentEnabled%1").arg(int(c::AdjustmentType::Invert)));
    CHECK(invert && invert->isEnabled()); if (invert) invert->click();
    CHECK(doc->layer(roles[0])->adjustments && doc->layer(roles[0])->adjustments->items[std::size_t(c::AdjustmentType::Invert)].enabled);
    find("UndoAction")->trigger(); CHECK(doc->contentState() == numericBefore);
    auto* gaussian = window.findChild<QDoubleSpinBox*>("FilterGaussianRadiusX"); CHECK(gaussian && gaussian->isEnabled());
    if (gaussian) gaussian->setValue(1.2);
    CHECK(doc->layer(roles[0])->filters && doc->layer(roles[0])->filters->items[0].enabled);
    find("UndoAction")->trigger(); CHECK(doc->contentState() == numericBefore);
    auto* filterChoice = window.findChild<QComboBox*>("FilterNavigation");
    auto* filterItems = filterChoice ? qobject_cast<QStandardItemModel*>(filterChoice->model()) : nullptr;
    CHECK(filterItems && filterItems->item(0)->isEnabled() && !filterItems->item(1)->isEnabled());
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
    CHECK(!find("ToolAction_smartselect")->isEnabled());
    CHECK(!find("ToolAction_brush")->isEnabled());
    CHECK(find("ToolAction_brush")->toolTip().contains("Channel Group"));
    if (canvas) CHECK(!canvas->scene().channelInspectionId);
    window.close();
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv); QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app, {});
    QCoreApplication::setOrganizationName("VulkanaChannelTests");
    QCoreApplication::setApplicationName("ChannelUi");
    QVulkanInstance instance;
    if (app.arguments().contains("--native")) {
        instance.setApiVersion(QVersionNumber(1, 2));
        CHECK(instance.create()); if (!instance.isValid()) return 1;
        nativeInstance = &instance;
    }
    try {
        if (!app.arguments().contains(QStringLiteral("--keyboard-only"))) {
            clipboardCodec(); dataExportControls(); numericNavigationRestoresColor();
        }
        workflow();
    }
    catch (const std::exception& e) { CHECK(false); std::cerr << e.what() << '\n'; }
    return failures ? 1 : 0;
}
