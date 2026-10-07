#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/Theme.hpp"

#include <QAction>
#include <QApplication>
#include "imageeditor/ui/ColorDialog.hpp"
#include <QComboBox>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QMenuBar>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVulkanInstance>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;
namespace {
int failures = 0;
class StyleChangeCounter final : public QObject {
public:
    int changes {0};
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::StyleChange && qobject_cast<QWidget*>(object)) ++changes;
        return false;
    }
};
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
template<class Predicate> bool waitUntil(Predicate predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) QTest::qWait(10);
    return predicate();
}
core::Rgba8 rgba(QColor color)
{
    return {static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
        static_cast<std::uint8_t>(color.blue()), static_cast<std::uint8_t>(color.alpha())};
}
void choosePreset(QComboBox& combo, ui::ThemePreset preset)
{
    for(auto* p=combo.parentWidget();p;p=p->parentWidget())
        if(auto* tabs=qobject_cast<QTabWidget*>(p)) { tabs->setCurrentWidget(combo.parentWidget()); break; }
    const auto index = combo.findData(int(preset));
    CHECK(index >= 0);
    if (index >= 0) combo.setCurrentIndex(index);
    settle();
}
ui::PreferencesDialog* visiblePreferences(ui::MainWindow& window)
{
    for (auto* candidate : window.findChildren<QDialog*>(QStringLiteral("PreferencesDialog"))) {
        if (auto* dialog = dynamic_cast<ui::PreferencesDialog*>(candidate); dialog && dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

void pickColor(ui::PreferencesDialog& dialog, ui::ThemeColor role, QColor color)
{
    auto* swatch = dialog.findChild<QPushButton*>(QStringLiteral("ThemeColor_")
        + QString::fromLatin1(ui::themeColorKey(role)));
    CHECK(swatch && swatch->isEnabled());
    if (!swatch || !swatch->isEnabled()) return;
    bool picked = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto* host = dialog.parentWidget();
        while (host->parentWidget()) host = host->parentWidget();
        auto* chooser = host->findChild<imageeditor::ui::ColorDialog*>("ThemeColorDialog");
        CHECK(chooser);
        if (!chooser) return;
        CHECK(chooser->objectName() == QStringLiteral("ThemeColorDialog"));
        CHECK(!chooser->isWindow() && !chooser->windowHandle());
        CHECK(!dialog.isVisible());
        CHECK(!chooser->testOption(imageeditor::ui::ColorDialog::ShowAlphaChannel));
        chooser->setCurrentColor(color);
        chooser->accept();
        picked = true;
    });
    swatch->click();
    CHECK(picked);
    settle();
}

void optionalScreenshot(ui::MainWindow& window, const QString& suffix)
{
    auto prefix = qEnvironmentVariable("IMAGEEDITOR_THEME_SCREENSHOT");
    if (prefix.isEmpty()) return;
    if (prefix.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) prefix.chop(4);
    const auto path = prefix + suffix + QStringLiteral(".png");
    window.activateWindow();
    QTest::qWait(200);
    QProcess capture;
    capture.start(QStringLiteral("spectacle"), {QStringLiteral("--background"), QStringLiteral("--nonotify"),
        QStringLiteral("--activewindow"), QStringLiteral("--output"), path});
    CHECK(capture.waitForFinished(5000));
    CHECK(capture.exitStatus() == QProcess::NormalExit && capture.exitCode() == 0);
    std::cout << "Theme review screenshot: " << path.toStdString() << '\n';
}

void preferencesUseTheWorkspaceModalPlaneAndOnlyInvalidatePresentation(QVulkanInstance& instance)
{
    QTemporaryDir assets;
    CHECK(assets.isValid());
    ui::MainWindow window(&instance, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1440, 920);
    window.show();
    window.activateWindow();
    settle();
    render::CanvasWindow* canvas = nullptr;
    for (auto* candidate : QGuiApplication::allWindows()) {
        if (candidate->objectName() == QStringLiteral("VulkanCanvasWindow"))
            canvas = dynamic_cast<render::CanvasWindow*>(candidate);
    }
    CHECK(canvas);
    if (!canvas) { window.close(); return; }
    QImage fixture(640, 420, QImage::Format_RGBA8888);
    fixture.fill(Qt::transparent);
    for (int y = 40; y < 380; ++y) for (int x = 40; x < 600; ++x) {
        fixture.setPixelColor(x, y, QColor(220, 80 + y / 4, 35 + x / 6, (x + y) % 180 + 50));
    }
    const auto path = assets.filePath(QStringLiteral("theme-fixture.png"));
    CHECK(fixture.save(path));
    CHECK(window.openImageFromPath(path));
    auto& session = const_cast<core::EditorSession&>(window.editorSession());
    CHECK(session.document() && session.activeLayer());
    if (!session.document() || !session.activeLayer()) { window.close(); return; }
    auto& document = *session.document();
    const auto id = *session.activeLayer();
    // Preserve a real undo and redo branch across both preview and persistence.
    CHECK(session.execute(std::make_unique<core::SetLayerOpacityCommand>(id, 0.75F)));
    CHECK(session.execute(std::make_unique<core::SetLayerOpacityCommand>(id, 0.5F)));
    CHECK(session.undo());
    document.markSaved();
    canvas->setDocument(document.snapshot(), false);
    // Establish the fixture through the actual QAction, independent of the
    // compositor's asynchronous activation and any initially focused field.
    auto* brushAction = window.findChild<QAction*>(QStringLiteral("ToolAction_brush"));
    CHECK(brushAction); if (brushAction) brushAction->trigger();
    settle();
    CHECK(session.activeTool() == core::ToolId::Brush);
    CHECK(waitUntil([&] { return canvas->rendererStats().framesSubmitted > 0
        && canvas->rendererStats().fullUploads > 0; }));
    auto observed = canvas->rendererStats().uploadedBytes;
    QElapsedTimer stable;
    stable.start();
    CHECK(waitUntil([&] {
        const auto current = canvas->rendererStats().uploadedBytes;
        if (current != observed) { observed = current; stable.restart(); }
        return stable.elapsed() >= 200;
    }));

    const auto stats = canvas->rendererStats();
    const auto content = document.contentState();
    const auto revision = document.revision();
    const auto historyDepth = session.history().undoDepth();
    const auto redoDepth = session.history().redoDepth();
    const auto historyMemory = session.history().memoryUsed();
    const auto colors = session.colors();
    const auto selection = session.layerSelectionState();
    const auto surface = std::get<core::RasterLayer>(document.layer(id)->payload).surface;
    const auto surfaceRevision = surface->revision();
    const auto matrix = document.layer(id)->localToDocument;
    const auto size = canvas->size();
    const auto zoom = canvas->zoom();
    const auto pan = canvas->scene().viewport.pan();
    const auto* style = QApplication::style();
    const auto font = QApplication::font();
    CHECK(historyDepth > 0 && redoDepth > 0);

    const auto unchanged = [&] {
        CHECK(document.contentState() == content && document.revision() == revision);
        CHECK(!document.isModified());
        CHECK(session.history().undoDepth() == historyDepth && session.history().redoDepth() == redoDepth);
        CHECK(session.history().memoryUsed() == historyMemory);
        CHECK(session.colors() == colors && session.layerSelectionState() == selection);
        CHECK(std::get<core::RasterLayer>(document.layer(id)->payload).surface == surface);
        CHECK(surface->revision() == surfaceRevision && surface->dirtySince(surfaceRevision).empty());
        CHECK(document.layer(id)->localToDocument == matrix);
        CHECK(canvas->size() == size && canvas->zoom() == zoom && canvas->scene().viewport.pan() == pan);
        CHECK(QApplication::style() == style && QApplication::font() == font);
    };
    const auto renderedWithoutUploads = [&] {
        const auto frame = canvas->rendererStats().framesSubmitted;
        canvas->scheduleFrame();
        CHECK(waitUntil([&] { return canvas->rendererStats().framesSubmitted > frame; }));
        const auto now = canvas->rendererStats();
        CHECK(now.fullUploads == stats.fullUploads && now.regionalUploads == stats.regionalUploads);
        CHECK(now.regionalUploadBatches == stats.regionalUploadBatches && now.uploadedBytes == stats.uploadedBytes);
        CHECK(now.resourceGeneration == stats.resourceGeneration && now.swapchainGeneration == stats.swapchainGeneration);
        CHECK(now.swapchainWidth == stats.swapchainWidth && now.swapchainHeight == stats.swapchainHeight);
        CHECK(now.stagingBufferAllocations == stats.stagingBufferAllocations);
        unchanged();
    };
    const auto sceneUsesTheme = [&] {
        CHECK(canvas->scene().overlayAccent == ui::editorAccent());
        CHECK(canvas->scene().canvasBackground == rgba(ui::themeColor(ui::ThemeColor::Canvas)));
        CHECK(canvas->scene().checkerLight == rgba(ui::themeColor(ui::ThemeColor::CheckerLight)));
        CHECK(canvas->scene().checkerDark == rgba(ui::themeColor(ui::ThemeColor::CheckerDark)));
    };
    const auto* preferences = window.findChild<QAction*>(QStringLiteral("PreferencesAction"));
    CHECK(preferences && preferences->isEnabled());
    if (!preferences || !preferences->isEnabled()) { window.close(); return; }
    auto* preferencesAction = const_cast<QAction*>(preferences);

    // An unchanged Cancel must not repaint/repolish the entire application.
    StyleChangeCounter styleChanges;
    QElapsedTimer closeTimer;
    double clickMs=0;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog=visiblePreferences(window); CHECK(dialog); if(!dialog)return;
        qApp->installEventFilter(&styleChanges);
        closeTimer.start();
        dialog->findChild<QPushButton*>("PreferencesCancel")->click();
        clickMs=double(closeTimer.nsecsElapsed())/1e6;
    });
    preferencesAction->trigger();
    const double closeMs=closeTimer.isValid()?double(closeTimer.nsecsElapsed())/1e6:0;
    qApp->removeEventFilter(&styleChanges);
    CHECK(styleChanges.changes==0);
    std::cout<<"Unchanged Preferences Cancel: click="<<clickMs<<" ms, closed="<<closeMs
             <<" ms, widget StyleChange events="<<styleChanges.changes<<'\n';
    unchanged();

    // General-only previews and their rollback must not reapply the theme.
    styleChanges.changes=0;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog=visiblePreferences(window); CHECK(dialog); if(!dialog)return;
        qApp->installEventFilter(&styleChanges);
        dialog->findChild<QCheckBox*>("AdvancedMeasurementReadout")->click();
        dialog->reject();
    });
    preferencesAction->trigger();
    qApp->removeEventFilter(&styleChanges);
    CHECK(styleChanges.changes==0);
    unchanged();

    ui::ThemeSettings applied;
    bool opened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = visiblePreferences(window);
        CHECK(dialog && dialog->isVisible());
        if (!dialog) return;
        opened = true;
        CHECK(!dialog->isWindow() && dialog->parentWidget());
        CHECK(dialog->parentWidget()->objectName() == QStringLiteral("WorkspaceDialogShield"));
        CHECK(dialog->minimumSize() == dialog->maximumSize());
        CHECK((dialog->geometry().center() - dialog->parentWidget()->rect().center()).manhattanLength() <= 2);
        const auto cardSize = dialog->size();
        auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
        auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("PreferencesApply"));
        auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("PreferencesCancel"));
        CHECK(preset && apply && cancel);
        if (!preset || !apply || !cancel) { dialog->reject(); return; }

        // Send to the actual underlying event recipients, not just the shield.
        // Neither tool keys, a stroke, panning, nor menus can leak through it.
        QTest::keyClick(canvas, Qt::Key_V);
        CHECK(session.activeTool() == core::ToolId::Brush);
        const QPoint start(canvas->width() / 2, canvas->height() / 2);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas, start + QPoint(40, 15));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 15));
        QTest::mousePress(canvas, Qt::MiddleButton, Qt::NoModifier, start);
        QTest::mouseMove(canvas, start + QPoint(40, 15));
        QTest::mouseRelease(canvas, Qt::MiddleButton, Qt::NoModifier, start + QPoint(40, 15));
        QTest::keyClick(canvas, Qt::Key_Z, Qt::ControlModifier);
        auto* rail = window.findChild<QToolBar*>(QStringLiteral("ToolRail"));
        CHECK(rail);
        bool toolChecked = false;
        if (rail) for (auto* button : rail->findChildren<QToolButton*>()) {
            const auto* action = button->defaultAction();
            if (!action || !action->shortcuts().contains(QKeySequence(Qt::Key_M))) continue;
            QTest::mouseClick(button, Qt::LeftButton);
            toolChecked = true;
            break;
        }
        CHECK(toolChecked && session.activeTool() == core::ToolId::Brush);
        auto* menu = window.menuBar();
        CHECK(menu && !menu->actions().empty());
        if (menu && !menu->actions().empty())
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(menu->actions().front()).center());
        CHECK(!QApplication::activePopupWidget());
        unchanged();

        choosePreset(*preset, ui::ThemePreset::Midnight);
        CHECK(ui::currentThemeSettings().preset == ui::ThemePreset::Midnight);
        CHECK(ui::themeColor(ui::ThemeColor::Accent)==QColor("#79A8FF"));
        sceneUsesTheme();
        renderedWithoutUploads();
        choosePreset(*preset, ui::ThemePreset::Light);
        CHECK(ui::currentThemeSettings().preset == ui::ThemePreset::Light);
        CHECK(QApplication::palette().color(QPalette::Window) == ui::themeColor(ui::ThemeColor::Background));
        CHECK(dialog->size() == cardSize);
        sceneUsesTheme();
        renderedWithoutUploads();
        optionalScreenshot(window, QStringLiteral("-light"));
        QSettings store;
        CHECK(ui::loadThemeSettings(store).preset == ui::ThemePreset::Dark);

        choosePreset(*preset, ui::ThemePreset::Custom);
        pickColor(*dialog, ui::ThemeColor::Accent, QColor("#EC783A"));
        pickColor(*dialog, ui::ThemeColor::Selection, QColor("#6B4131"));
        pickColor(*dialog, ui::ThemeColor::Canvas, QColor("#342A25"));
        pickColor(*dialog, ui::ThemeColor::CheckerLight, QColor("#AD998A"));
        pickColor(*dialog, ui::ThemeColor::CheckerDark, QColor("#8A7465"));
        CHECK(ui::editorAccent() == core::Rgba8({236, 120, 58, 255}));
        const auto* contextLabel = window.findChild<QLabel*>(QStringLiteral("ToolOptionsContext"));
        CHECK(contextLabel);
        if (contextLabel) {
            CHECK(contextLabel->palette().color(QPalette::WindowText) == ui::themeTone("#8D9AF8"));
            std::cout << "Context accent: " << contextLabel->palette().color(QPalette::WindowText).name().toStdString()
                      << ", expected " << ui::themeTone("#8D9AF8").name().toStdString() << '\n';
        }
        sceneUsesTheme();
        renderedWithoutUploads();
        optionalScreenshot(window, QStringLiteral("-custom"));
        applied = dialog->draft().theme;
        apply->click();
        CHECK(dialog->isVisible());
        store.sync();
        CHECK(ui::loadThemeSettings(store) == applied);
        unchanged();

        choosePreset(*preset, ui::ThemePreset::Light);
        renderedWithoutUploads();
        cancel->click();
    });
    preferencesAction->trigger();
    CHECK(opened);
    settle();
    CHECK(ui::currentThemeSettings() == applied);
    sceneUsesTheme();
    renderedWithoutUploads();

    // Reopening consumes the saved/current state; cancel without applying
    // restores it and never overwrites the already persisted custom colors.
    bool reopened = false;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = visiblePreferences(window);
        CHECK(dialog && dialog->isVisible());
        if (!dialog) return;
        reopened = true;
        CHECK(dialog->draft().theme == applied);
        auto* preset = dialog->findChild<QComboBox*>(QStringLiteral("ThemePresetCombo"));
        CHECK(preset);
        if (preset) choosePreset(*preset, ui::ThemePreset::Dark);
        QTest::keyClick(dialog, Qt::Key_Escape);
    });
    preferencesAction->trigger();
    CHECK(reopened && ui::currentThemeSettings() == applied);
    QSettings reopenedSettings;
    CHECK(ui::loadThemeSettings(reopenedSettings) == applied);
    sceneUsesTheme();
    renderedWithoutUploads();
    // The barrier is removed on close: tool input resumes normally.
    QTest::keyClick(canvas, Qt::Key_V);
    settle();
    CHECK(session.activeTool() == core::ToolId::Move);
    unchanged();
    window.logRendererDiagnostics();
    std::cout << "Theme previews: frames=" << canvas->rendererStats().framesSubmitted - stats.framesSubmitted
              << ", raster upload bytes=" << canvas->rendererStats().uploadedBytes - stats.uploadedBytes
              << ", swapchain recreations=" << canvas->rendererStats().swapchainGeneration - stats.swapchainGeneration << '\n';
    window.close();
    settle();
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ThemePreferencesInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(app);
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        std::cerr << "Theme preferences validation requires native Wayland\n";
        return EXIT_FAILURE;
    }
    int warnings = 0, errors = 0;
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) {
        std::cerr << "Theme preferences validation requires VK_LAYER_KHRONOS_validation\n";
        return EXIT_FAILURE;
    }
    instance.setLayers({QByteArrayLiteral("VK_LAYER_KHRONOS_validation")});
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags types, const void* message) {
        if (!types.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* details = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << (details ? details->pMessage : "Vulkan validation message") << '\n';
        return false;
    });
    if (!instance.create()) {
        std::cerr << "Could not create Vulkan instance\n";
        return EXIT_FAILURE;
    }
    preferencesUseTheWorkspaceModalPlaneAndOnlyInvalidatePresentation(instance);
    instance.destroy();
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Theme preferences Vulkan: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
