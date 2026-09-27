#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/platform/BuildInfo.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QIcon>
#include <QKeyEvent>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <QtGui/qguiapplication_platform.h>
#include <iostream>

namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool ok, const char* what, int line)
{
    if (!ok) { ++failures; std::cerr << "FAIL " << line << ": " << what << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
template<class F> bool waitFor(F condition)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < 2000) QTest::qWait(10);
    return condition();
}
u::NewDocumentDialog* visibleCard(u::MainWindow& window)
{
    for (auto* widget : window.findChildren<QDialog*>())
        if (auto* card = dynamic_cast<u::NewDocumentDialog*>(widget); card && !card->isHidden()) return card;
    return nullptr;
}
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    app.setOrganizationName("ImageEditorTests"); app.setApplicationName("WorkspaceDialog");
    app.setApplicationVersion(QStringLiteral("0.2.1-test"));
    app.setWindowIcon(QIcon(QStringLiteral(":/Vulkana512.png")));
    CHECK(!app.windowIcon().isNull());
    QTemporaryDir files;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, files.path());
    u::applyEditorTheme(app);
    const bool native = app.arguments().contains("--native");
    int validationMessages = 0;
    QVulkanInstance instance;
    if (native) {
        instance.setApiVersion(QVersionNumber(1, 2));
        instance.setLayers({"VK_LAYER_KHRONOS_validation"});
        instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,
            QVulkanInstance::DebugMessageTypeFlags types, const void* message) {
            if (!types.testFlag(QVulkanInstance::ValidationMessage)
                || !(flags & (QVulkanInstance::ErrorSeverity | QVulkanInstance::WarningSeverity))) return false;
            ++validationMessages;
            std::cerr << static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message)->pMessage << '\n';
            return false;
        });
        if (!instance.create()) return 1;
    }
    {
        u::MainWindow window(native ? &instance : nullptr, false);
        window.setUnsavedPromptEnabled(false);
        window.resize(1280, 800); window.show(); window.activateWindow();
        u::OverlayDockWorkspace* workspace = nullptr;
        for (auto* child : window.findChildren<QWidget*>())
            if (auto* w = dynamic_cast<u::OverlayDockWorkspace*>(child)) workspace = w;
        CHECK(workspace);
        if (!workspace) return 1;
        QTest::qWait(150);
        if (native) CHECK(waitFor([&] { return window.windowHandle()->isExposed()
            && QGuiApplication::focusWindow() == window.windowHandle(); }));
#if QT_CONFIG(wayland)
        if (native) {
            if (auto* wayland = app.nativeInterface<QNativeInterface::QWaylandApplication>();
                wayland && !wayland->lastInputSeat()) {
                // QTest's synthetic keys cannot supply a Wayland input serial.
                // Cover the host screen briefly so the real compositor sends
                // pointer-enter, then restore the intended test geometry. This
                // is fixture synchronization, not a production popup fallback.
                window.showMaximized();
                if (app.arguments().contains("--await-native-input")) {
                    window.setWindowTitle(QStringLiteral("Vulkana popup checks — move the pointer into this window"));
                    std::cout << "Waiting up to 60 seconds for a real Wayland pointer/key event.\n" << std::flush;
                    QElapsedTimer inputWait;
                    inputWait.start();
                    while (!wayland->lastInputSeat() && inputWait.elapsed() < 60000)
                        QTest::qWait(20);
                }
                CHECK(waitFor([&] { return wayland->lastInputSeat() != nullptr; }));
                window.showNormal();
                CHECK(waitFor([&] { return !window.isMaximized() && window.size() == QSize(1280, 800); }));
            }
        }
#endif
        imageeditor::render::CanvasWindow* canvas = nullptr;
        u::TextController* editor = nullptr;
        for (auto* w : QGuiApplication::allWindows())
            if (auto* c = dynamic_cast<imageeditor::render::CanvasWindow*>(w)) canvas = c;
        for (auto* child : window.children())
            if (auto* text = dynamic_cast<u::TextController*>(child)) editor = text;
        CHECK(canvas && editor);
        if (!canvas || !editor) return 1;
        auto* newAction = window.findChild<QAction*>("NewDocumentAction");
        auto* resizeAction = window.findChild<QAction*>("ChangeCanvasSizeAction");
        CHECK(newAction && resizeAction);
        if (!newAction || !resizeAction) return 1;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, [&] {
            CHECK(false);
            if (auto* card = visibleCard(window)) card->reject();
        });
        auto exercise = [&](QAction* action, bool closeHost) {
            const auto originalTool = window.editorSession().activeTool();
            QPointer<QWidget> previousProxy = workspace->canvasContainer()->focusProxy();
            bool visited = false;
            watchdog.start(15000);
            QTimer::singleShot(0, &window, [&] {
                auto* card = visibleCard(window);
                CHECK(card);
                if (!card) return;
                visited = true;
                CHECK(!card->isWindow());
                CHECK(!card->windowHandle());
                CHECK(!QApplication::topLevelWidgets().contains(card));
                CHECK(!QApplication::activeModalWidget());
                CHECK(card->minimumSize() == card->maximumSize());
                auto* shield = card->parentWidget();
                CHECK(shield->parentWidget() == workspace->panelOverlay());
                auto centered = [&] {
                    return (card->geometry().center() - shield->rect().center()).manhattanLength() <= 2;
                };
                CHECK(centered());
                CHECK(shield->rect().contains(card->geometry()));
                auto* width = card->findChild<QDoubleSpinBox*>("CanvasWidthSpinBox");
                CHECK(width);
                if (!width) { card->reject(); return; }
                // Cross-native-surface input takes the same QWidget bridge as
                // real Wayland input; the shield must not swallow its carrier.
                auto* plane = workspace->panelOverlay()->windowHandle();
                if (native) {
                    QTest::mouseClick(plane, Qt::LeftButton, {}, width->mapTo(workspace->panelOverlay(), width->rect().center()));
                    CHECK(waitFor([&] { return QGuiApplication::focusWindow() == window.windowHandle()
                        && (QGuiApplication::focusObject() == width || QGuiApplication::focusObject() == width->findChild<QLineEdit*>()); }));
                    QTest::qWait(20);
                    QTest::keyClick(window.windowHandle(), Qt::Key_A, Qt::ControlModifier);
                    QTest::keyClick(window.windowHandle(), Qt::Key_6);
                    QTest::keyClick(window.windowHandle(), Qt::Key_5);
                    QTest::keyClick(window.windowHandle(), Qt::Key_Return);
                } else {
                    width->setFocus();
                    QTest::keyClick(width, Qt::Key_A, Qt::ControlModifier);
                    QTest::keyClick(width, Qt::Key_6);
                    QTest::keyClick(width, Qt::Key_5);
                    QTest::keyClick(width, Qt::Key_Return);
                }
                if (width->value() != 65) std::cerr << "width=" << width->value() << " focus="
                    << (QApplication::focusWidget() ? QApplication::focusWidget()->objectName().toStdString() : "none") << '\n';
                CHECK(width->value() == 65);
                // Actual native input may arrive on the Vulkan child rather
                // than the host QWidgetWindow (not covered by widget tests).
                width->setFocus();
                QTest::keyClick(canvas, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClick(canvas, Qt::Key_8);
                QTest::keyClick(canvas, Qt::Key_1);
                QTest::keyClick(canvas, Qt::Key_Return);
                CHECK(width->value() == 81);
                if (auto* background = card->findChild<QComboBox*>("CanvasBackgroundCombo")) {
                    background->setFocus();
                    QTest::keyClick(canvas, Qt::Key_Return);
                    CHECK(QApplication::activePopupWidget());
                    QTest::keyClick(canvas, Qt::Key_Down);
                    QTest::keyClick(canvas, Qt::Key_Return);
                    CHECK(!QApplication::activePopupWidget());
                    CHECK(background->currentIndex() == 1);
                    CHECK(visibleCard(window) == card);
                }
                // Input aimed at editor surfaces remains blocked, not rerouted
                // into brush, pan or QAction shortcuts. Outside clicks stay put.
                QTest::keyClick(&window, Qt::Key_B);
                QTest::keyClick(&window, Qt::Key_N, Qt::ControlModifier);
                QTest::mouseClick(shield, Qt::LeftButton, {}, QPoint(4, 4));
                CHECK(window.editorSession().activeTool() == originalTool);
                CHECK(visibleCard(window) == card);
                QTest::keyClick(canvas, Qt::Key_Space);
                CHECK(!canvas->spacePanHeld());
                QTest::mousePress(canvas, Qt::MiddleButton, {}, QPoint(20, 20));
                CHECK(!canvas->panDragging());
                QTest::mouseRelease(canvas, Qt::MiddleButton, {}, QPoint(20, 20));
                width->setFocus();
                for (int i = 0; i < 30; ++i) {
                    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
                    auto* focus = QApplication::focusWidget();
                    CHECK(focus && (focus == card || card->isAncestorOf(focus)));
                }
                const auto fixedSize = card->size();
                window.resize(980, 640);
                CHECK(waitFor(centered));
                CHECK(card->size() == fixedSize);
                CHECK(shield->rect().contains(card->geometry()));
                window.showMaximized(); QTest::qWait(120);
                CHECK(waitFor(centered));
                CHECK(card->size() == fixedSize);
                if (editor->active()) {
                    editor->viewportChanged();
                    auto* textOverlay = window.findChild<QWidget*>("TextContextOverlay");
                    CHECK(textOverlay);
                    if (textOverlay) {
                        CHECK(waitFor([&] {
                            auto* hit = workspace->panelOverlay()->childAt(textOverlay->geometry().center());
                            return hit && (hit == shield || shield->isAncestorOf(hit));
                        }));
                    }
                }
                window.showMinimized(); QTest::qWait(120);
                if (!native) CHECK(window.isMinimized());
                if (native) CHECK(waitFor([&] { return !window.windowHandle()->isExposed(); }));
                if (native) CHECK(waitFor([&] { return !plane->isExposed(); }));
                window.showNormal(); window.activateWindow(); QTest::qWait(120);
                CHECK(waitFor(centered));
                CHECK(visibleCard(window) == card);
                if (native) CHECK(waitFor([&] { return plane->isExposed(); }));
                // Focus restoration after minimize/restore is equally important
                // when an unfinished text edit exists underneath the shield.
                width->setFocus();
                QTest::keyClick(width, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClick(width, Qt::Key_7);
                QTest::keyClick(width, Qt::Key_Return);
                CHECK(width->value() == 7);
                const auto preview = qEnvironmentVariable("IMAGEEDITOR_TEST_WORKSPACE_DIALOG_PREVIEW");
                if (!preview.isEmpty() && !closeHost) CHECK(shield->grab().save(preview));
                if (closeHost) window.close();
                else {
                    if (native) {
                        QTest::mouseClick(plane, Qt::LeftButton, {}, QPoint(4, 4));
                        QTest::qWait(20);
                        QTest::keyClick(window.windowHandle(), Qt::Key_Escape);
                    } else QTest::keyClick(width, Qt::Key_Escape);
                }
            });
            action->trigger();
            watchdog.stop();
            CHECK(visited);
            CHECK(!visibleCard(window));
            CHECK(!window.findChild<QWidget*>("WorkspaceDialogShield"));
            if (!closeHost) CHECK(workspace->canvasContainer()->focusProxy() == previousProxy);
        };
        exercise(newAction, false);
        editor->press({200, 200}, {}, false);
        QKeyEvent type(QEvent::KeyPress, Qt::Key_H, {}, QStringLiteral("Hello"));
        CHECK(editor->keyEvent(&type, false));
        CHECK(editor->active());
        const auto id = editor->layerId();
        const auto before = std::get<imageeditor::core::TextLayer>(window.editorSession().document()->layer(id)->payload);
        auto* help = window.findChild<QMenu*>("HelpMenu");
        auto* documentation = window.findChild<QAction*>("DocumentationAction");
        auto* about = window.findChild<QAction*>("AboutVulkanaAction");
        CHECK(help && documentation && about);
        const auto historyDepth = window.editorSession().history().undoDepth();
        const auto toolBeforeAbout = window.editorSession().activeTool();
        if (help && documentation && about) {
            auto* feedback = window.findChild<QAction*>("FeedbackAction");
            CHECK(feedback);
            CHECK(help->actions() == QList<QAction*>({documentation, feedback, about}));
            documentation->trigger(); // Placeholder: no popup, navigation or edit.
            CHECK(!window.findChild<QDialog*>("AboutVulkanaDialog"));
            for (bool escape : {false, true}) {
                bool visited = false;
                QTimer::singleShot(0, &window, [&] {
                    auto* card = window.findChild<QDialog*>("AboutVulkanaDialog");
                    CHECK(card && card->isVisible());
                    if (!card) { window.close(); return; }
                    visited = true;
                    CHECK(!card->isWindow() && card->minimumSize() == card->maximumSize());
                    auto* version = card->findChild<QLabel*>("AboutVulkanaVersion");
                    auto* icon = card->findChild<QLabel*>("AboutVulkanaIcon");
                    auto* status = card->findChild<QLabel*>("AboutUpdateStatus");
                    auto* checkUpdate = card->findChild<QPushButton*>("AboutCheckUpdate");
                    auto* close = card->findChild<QPushButton*>("AboutClose");
                    CHECK(version && icon && status && checkUpdate && close);
                    if (version && icon && status && checkUpdate && close) {
                        CHECK(version->text() == QStringLiteral("Version 0.2.1-test · %1")
                            .arg(QString::fromLatin1(imageeditor::platform::releaseChannel)));
                        CHECK(!icon->pixmap().isNull());
                        CHECK(status->text() == "Check for a new release.");
                        const auto size = card->size();
                        // Network/update state coverage lives in UpdateServiceTests;
                        // workspace focus checks must never contact a live server.
                        auto* page = card->findChild<QPushButton*>("AboutDownloadPage");
                        auto* download = card->findChild<QPushButton*>("AboutDownload");
                        CHECK(page && page->isEnabled());
                        CHECK(!download && checkUpdate->text() == "Check for Updates");
                        QTest::qWait(10);
                        CHECK(card->size() == size);
                        QTest::keyClick(canvas, Qt::Key_B);
                        CHECK(window.editorSession().activeTool() == toolBeforeAbout);
                        if (escape) QTest::keyClick(canvas, Qt::Key_Escape);
                        else close->click();
                    } else card->reject();
                });
                about->trigger();
                CHECK(visited);
                CHECK(!window.findChild<QDialog*>("AboutVulkanaDialog"));
                CHECK(editor->active() && editor->layerId() == id);
                CHECK(std::get<imageeditor::core::TextLayer>(window.editorSession().document()->layer(id)->payload) == before);
                CHECK(window.editorSession().history().undoDepth() == historyDepth);
            }
        }
        exercise(newAction, false);
        CHECK(editor->active());
        CHECK(std::get<imageeditor::core::TextLayer>(window.editorSession().document()->layer(id)->payload) == before);
        exercise(resizeAction, true);
        CHECK(!window.isVisible());
    }
    // Match startup: open the card on the first event-loop turn, before any
    // canvas click or pre-established native/QWidget focus handoff.
    {
        u::MainWindow window(native ? &instance : nullptr, false);
        window.setUnsavedPromptEnabled(false);
        window.resize(1280,800);
        bool checked = false;
        QTimer probe;
        QObject::connect(&probe, &QTimer::timeout, &window, [&] {
            auto* card = visibleCard(window);
            if (!card) return;
            probe.stop();
            auto* width = card->findChild<QDoubleSpinBox*>("CanvasWidthSpinBox");
            imageeditor::render::CanvasWindow* canvas = nullptr;
            for (auto* candidate : QGuiApplication::allWindows())
                if (auto* c = dynamic_cast<imageeditor::render::CanvasWindow*>(candidate)) canvas = c;
            CHECK(width && canvas);
            if (width && canvas) {
                width->setFocus();
                QTest::keyClick(canvas, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClick(canvas, Qt::Key_9);
                QTest::keyClick(canvas, Qt::Key_6);
                QTest::keyClick(canvas, Qt::Key_Return);
                CHECK(width->value() == 96);
            }
            checked = true;
            card->reject();
        });
        window.show();
        probe.start(50);
        QTimer::singleShot(0, &window, &u::MainWindow::showStartupDocument);
        QTimer::singleShot(10000, &window, [&] { if (!checked) { CHECK(false); window.close(); } });
        CHECK(waitFor([&] { return checked; }));
        window.close();
    }
    CHECK(validationMessages == 0);
    std::cout << "Workspace dialog checks: " << failures << " failures; " << validationMessages << " validation messages\n";
    return failures ? 1 : 0;
}
