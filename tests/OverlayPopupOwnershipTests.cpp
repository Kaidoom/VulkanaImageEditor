#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include "imageeditor/ui/FontFamilyPicker.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/BrushComponentPicker.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include <QAbstractItemView>
#include "imageeditor/ui/ColorDialog.hpp"
#include <QFileDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QPushButton>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QToolButton>
#include <QTest>
#include <QVBoxLayout>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
#include <csignal>
#include <iostream>

namespace {
int failures = 0;
QtMessageHandler previousHandler = nullptr;
void messages(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(QStringLiteral("must be a top level window"))) {
        ++failures;
        if (qEnvironmentVariableIsSet("VULKANA_TRACE_POPUP_WARNING")) std::raise(SIGTRAP);
    }
    if (previousHandler) previousHandler(type, context, message);
    else std::cerr << message.toStdString() << '\n';
}
void check(bool value, const char* description)
{
    if (!value) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    imageeditor::ui::applyEditorTheme(app);
    previousHandler = qInstallMessageHandler(messages);
    {
        QWidget host;
        host.setObjectName("PopupOwnerHost");
        auto* layout = new QVBoxLayout(&host);
        auto* canvas = new imageeditor::render::CanvasWindow;
        auto* workspace = new imageeditor::ui::OverlayDockWorkspace(canvas, &host);
        layout->addWidget(workspace);
        auto* content = new QWidget;
        auto* controls = new QVBoxLayout(content);
        auto* combo = new QComboBox(content);
        combo->addItems({"First", "Second", "Third"});
        // As in Blend Mode startup: Qt creates a popup, then the control
        // becomes unavailable before joining the native overlay hierarchy.
        combo->view();
        combo->setEnabled(false);
        controls->addWidget(combo);
        auto* font = new imageeditor::ui::FontFamilyPicker({"Noto Sans", "Noto Serif"}, content);
        controls->addWidget(font);
        auto* panel = new imageeditor::ui::WorkspacePanel("Popup test", content);
        workspace->addPanel(panel);
        host.resize(1000, 700); host.show();
        QTest::qWait(100);
#if QT_CONFIG(wayland)
        if (!app.arguments().contains("--wait-for-click")) {
            if (auto* wayland = app.nativeInterface<QNativeInterface::QWaylandApplication>();
                wayland && !wayland->lastInputSeat()) {
                host.showMaximized();
                check(QTest::qWaitForWindowExposed(host.windowHandle()), "Wayland test host is exposed");
                QElapsedTimer inputDeadline;
                inputDeadline.start();
                while (!wayland->lastInputSeat() && inputDeadline.elapsed() < 3000)
                    QTest::qWait(20);
                host.showNormal();
                QTest::qWait(100);
                if (!wayland->lastInputSeat()) {
                    std::cerr << "PENDING: compositor input device unavailable; run with --wait-for-click\n";
                    return 77;
                }
            }
        }
#endif
        if (app.arguments().contains("--wait-for-click")) {
            // Synthetic QTest events do not create the compositor input serial
            // required for Wayland popup grabs. This optional real-input gate
            // is separate from deterministic ownership/routing assertions.
            auto* begin = new QPushButton("Click to run the native popup ownership checks", content);
            controls->addWidget(begin);
            begin->show();
            QEventLoop input;
            bool clicked = false;
            QObject::connect(begin, &QPushButton::clicked, &input, [&] { clicked = true; input.quit(); });
            QTimer::singleShot(120000, &input, &QEventLoop::quit);
            input.exec();
            if (!clicked) { std::cerr << "PENDING: real Wayland input was not supplied\n"; return 77; }
            // Keep the control in the layout: deleting it here changes the
            // combo's position while the first popup configure is in flight.
            begin->setEnabled(false);
        }
        auto* overlay = workspace->panelOverlay();
        qInfo() << "Ownership: overlay widget" << overlay << "widget window" << overlay->window()
            << "QWindow" << overlay->windowHandle() << "native parent" << overlay->windowHandle()->parent()
            << "host" << host.windowHandle();
        const auto geometry = workspace->canvasContainer()->geometry();
        // Like Blend Mode before a document is opened: the popup can be
        // created/polished while its logical combo is disabled. Reparenting
        // the native popup must not strand its inherited disabled state.
        combo->setEnabled(false);
        auto* earlyPopup = combo->view()->window();
        earlyPopup->ensurePolished();
        combo->setEnabled(true);
        QTest::mousePress(combo, Qt::LeftButton);
        QTest::qWait(50);
        auto* popup = QApplication::activePopupWidget();
        check(popup && popup->windowHandle(), "combo popup is native");
        if (popup && popup->windowHandle()) {
            check(popup->isEnabled() && combo->view()->isEnabled(), "reparented combo popup follows the enabled control");
            // Send the release through QWindow, not directly to the widget:
            // Qt's native popup routing closes disabled popups here.
            QTest::mouseRelease(host.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                combo->mapTo(&host, combo->rect().center()));
            check(QApplication::activePopupWidget() == popup && popup->isVisible(),
                "ordinary click leaves the dropdown open after mouse release");
            qInfo() << "Combo: widget parent" << popup->parentWidget() << "QWindow parent" << popup->windowHandle()->parent()
                << "transient owner" << popup->windowHandle()->transientParent();
            check(popup->windowHandle()->transientParent() == host.windowHandle(), "combo has real top-level owner");
            check(imageeditor::ui::popupLogicalParent(popup) == combo, "combo retains logical owner for Enter routing");
            if (app.platformName() == "wayland") {
                QTest::qWait(100);
                const int bottom = combo->mapToGlobal(QPoint(0, combo->height())).y();
                const int top = popup->windowHandle()->mapToGlobal(QPoint()).y();
                qInfo() << "Combo native anchor: top" << top << "control bottom" << bottom;
                check(top >= bottom - 1 && top <= bottom + 8, "Wayland combo anchor is relative to the real owner");
            }
            const auto secondItem = combo->model()->index(1, 0);
            // Qt guards against selecting on the opening click/double click.
            QTest::qWait(QApplication::doubleClickInterval() + 10);
            QTest::mouseClick(combo->view()->viewport(), Qt::LeftButton, Qt::NoModifier,
                combo->view()->visualRect(secondItem).center());
            check(combo->currentIndex() == 1 && !popup->isVisible(), "mouse selection commits and closes the dropdown");
            combo->showPopup();
            combo->view()->setCurrentIndex(combo->model()->index(2, 0));
            QTest::keyClick(popup, Qt::Key_Return);
            check(combo->currentIndex() == 2, "Enter selects the dropdown item");
        }
        combo->hidePopup();
        QMenu menu(combo); menu.addAction("Test action");
        menu.popup(combo->mapToGlobal(QPoint(0, combo->height()))); QTest::qWait(50);
        check(menu.windowHandle() && menu.windowHandle()->transientParent() == host.windowHandle(), "menu has real top-level owner");
        menu.close();
        font->showPopup(); QTest::qWait(50);
        check(font->completionPopup()->windowHandle()->transientParent() == host.windowHandle(), "font popup has real top-level owner");
        font->hidePopup();
        QString chosen;
        font->onFamilyChosen = [&](const QString& name) { chosen = name; };
        font->lineEdit()->setFocus();
        QTest::keyClick(font->lineEdit(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(font->lineEdit(), "Serif");
        check(font->candidate() == "Noto Serif", "font query retains its model");
        font->showPopup(); // Explicitly highlight the candidate, as the existing picker does.
        QTest::keyClick(font->completionPopup(), Qt::Key_Return);
        check(chosen == "Noto Serif", "font typing and Enter retain the control/model association");
        // Exercise grabbing menus before separate dialog windows. A synthetic
        // button click cannot refresh Wayland's input serial after the pointer
        // has entered a dialog and that dialog has been destroyed.
        imageeditor::ui::BrushComponentPicker picker(imageeditor::core::BrushAssetType::Tip, panel);
        picker.show();
        picker.findChild<QToolButton*>()->click();
        QTest::qWait(50);
        popup = QApplication::activePopupWidget();
        check(popup && popup->windowHandle()->transientParent() == host.windowHandle(), "brush button opens an owned menu");
        if (popup) popup->close();
        picker.hide();
        imageeditor::ui::NewDocumentDialog card(panel);
        card.show();
        card.findChild<QPushButton*>("OpenDocumentButton")->click();
        QTest::qWait(50);
        popup = QApplication::activePopupWidget();
        check(popup && popup->windowHandle()->transientParent() == host.windowHandle(), "document button opens an owned menu");
        if (popup) popup->close();
        card.hide();
        imageeditor::ui::ColorDialog color(imageeditor::ui::popupTopLevelOwner(combo));
        color.setOption(imageeditor::ui::ColorDialog::DontUseNativeDialog);
        color.show(); QTest::qWait(30);
        check(!color.isWindow() && !color.windowHandle(), "color card has no native window");
        check(color.parentWidget() == workspace->modalOverlay(), "color card uses the workspace modal overlay");
        check(color.minimumSize() == color.maximumSize(), "color card is fixed-size");
        color.close();
        if (app.platformName() != "offscreen")
            check(QTest::qWaitForWindowActive(host.windowHandle()), "host focus returns after color dialog");
        QFileDialog file(imageeditor::ui::popupTopLevelOwner(combo));
        file.setOption(QFileDialog::DontUseNativeDialog);
        file.show(); QTest::qWait(30);
        check(file.windowHandle()->transientParent() == host.windowHandle(), "file dialog has real top-level owner");
        file.close();
        if (app.platformName() != "offscreen")
            check(QTest::qWaitForWindowActive(host.windowHandle()), "host focus returns after file dialog");
        QMessageBox embedded(panel);
        embedded.setWindowFlags(Qt::Widget);
        check(embedded.parentWidget() == panel && !embedded.isWindow(), "embedded modal construction is untouched");
        check(overlay->windowHandle()->parent() != nullptr, "panel plane remains a native child");
        check(overlay->backingStore() != host.backingStore(), "panel plane retains independent alpha backing store");
        check(!panel->isWindow(), "ordinary panel remains inside the workspace");
        check(workspace->canvasContainer()->geometry() == geometry, "popup does not move the canvas");
    }
    qInstallMessageHandler(previousHandler);
    std::cout << "Overlay popup ownership: " << failures << " failures\n";
    return failures ? 1 : 0;
}
