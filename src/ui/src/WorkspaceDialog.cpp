#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"

#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDropEvent>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLayout>
#include <QLoggingCategory>
#include <QMimeData>
#include <QPainter>
#include <QTimer>
#include <QUrl>
#include <QWindow>

namespace imageeditor::ui {
Q_LOGGING_CATEGORY(workspaceDialogLog, "imageeditor.workspaceDialog", QtWarningMsg)
namespace {
bool belongsTo(const QObject* target, const QObject* owner)
{
    for (; target; target = popupLogicalParent(target))
        if (target == owner) return true;
    return false;
}
bool isEditorInput(QEvent::Type type)
{
    switch (type) {
    case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::Shortcut:
    case QEvent::ShortcutOverride: case QEvent::InputMethod:
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: case QEvent::MouseMove: case QEvent::Wheel:
    case QEvent::TabletPress: case QEvent::TabletMove: case QEvent::TabletRelease:
    case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::TouchEnd:
    case QEvent::ContextMenu: case QEvent::DragEnter: case QEvent::DragMove:
    case QEvent::Drop: case QEvent::NativeGesture: return true;
    default: return false;
    }
}
}

WorkspaceDialog::WorkspaceDialog(OverlayDockWorkspace& workspace, QWidget& host)
    : QWidget(workspace.panelOverlay()), workspace_(workspace), host_(host)
{
    setObjectName(QStringLiteral("WorkspaceDialogShield"));
    setFocusPolicy(Qt::NoFocus);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (running_ && now && belongsTo(now, dialog_)) {
            lastFocus_ = now;
            workspace_.canvasContainer()->setFocusProxy(now);
        } else if (running_) {
            // A recently dismissed combo popup can restore its previous focus
            // after the card has opened. Reassert modal focus after that Qt
            // handoff, while still allowing nested popups and other apps.
            QTimer::singleShot(0, this, [this] { restoreCardFocus(); });
        }
    });
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
        QTimer::singleShot(0, this, [this] { restoreCardFocus(); });
    });
}

WorkspaceDialog::~WorkspaceDialog()
{
    qApp->removeEventFilter(this);
}

int WorkspaceDialog::exec(QDialog& dialog)
{
    Q_ASSERT(dialog.parentWidget() == this && !dialog.isWindow());
    dialog_ = &dialog;
    previousFocus_ = QApplication::focusWidget();
    previousProxy_ = workspace_.canvasContainer()->focusProxy();
    lastFocus_ = nullptr;
    dialog.setResult(QDialog::Rejected);
    QEventLoop loop;
    bool finished = false;
    const auto finish = [&] { finished = true; loop.quit(); };
    connect(&dialog, &QDialog::finished, &loop, finish);
    connect(qApp, &QCoreApplication::aboutToQuit, &loop, finish);
    running_ = true;
    qApp->installEventFilter(this);
    workspace_.setModalOverlay(this);
    dialog.show();
    centerCard();
    // Establish QWidget focus on the same child-surface bridge used by text
    // controls. No keyboard grab or synthetic pointer/key stream is required.
    QWidget* first = dialog.nextInFocusChain();
    while (first != &dialog && (!belongsTo(first, &dialog)
        || !(first->focusPolicy() & Qt::TabFocus) || !first->isEnabled() || first->isHidden()))
        first = first->nextInFocusChain();
    lastFocus_ = first;
    focusControl(first);
    if (!finished) loop.exec();
    running_ = false;
    qApp->removeEventFilter(this);
    if (auto* focus = QApplication::focusWidget(); focus && belongsTo(focus, &dialog)) focus->clearFocus();
    workspace_.canvasContainer()->setFocusProxy(nullptr);
    dialog.hide();
    if (host_.isVisible() && !host_.isMinimized())
        focusControl(previousFocus_ ? previousFocus_.data() : workspace_.canvasContainer());
    workspace_.canvasContainer()->setFocusProxy(previousProxy_);
    workspace_.setModalOverlay(nullptr);
    return dialog.result();
}

void WorkspaceDialog::reject() { if (dialog_) dialog_->reject(); }

void WorkspaceDialog::setLocalFileDropHandler(std::function<void(const QStringList&)> handler)
{
    localFileDropHandler_ = std::move(handler);
    setAcceptDrops(bool(localFileDropHandler_));
}

void WorkspaceDialog::focusControl(QWidget* control)
{
    auto* nativeFocus = QGuiApplication::focusWindow();
    auto* before = QGuiApplication::focusObject();
    workspace_.canvasContainer()->setFocusProxy(control == workspace_.canvasContainer() ? nullptr : control);
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(control->window());
    QT_WARNING_POP
    control->setFocus(Qt::OtherFocusReason);
    // The native host and transparent panel plane have different QWidget
    // ancestry. Notify the platform input context when their shared bridge
    // changes editor, just as the canvas text overlay does.
    auto* after = QGuiApplication::focusObject();
    if (nativeFocus && nativeFocus == QGuiApplication::focusWindow() && before != after)
        Q_EMIT nativeFocus->focusObjectChanged(after);
}

void WorkspaceDialog::restoreCardFocus()
{
    auto* nativeFocus = QGuiApplication::focusWindow();
    if (running_ && lastFocus_ && host_.isVisible() && !host_.isMinimized()
        && QGuiApplication::applicationState() == Qt::ApplicationActive
        && (nativeFocus == host_.windowHandle() || nativeFocus == workspace_.panelOverlay()->windowHandle())
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget())
        focusControl(lastFocus_);
}

bool WorkspaceDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (!running_ || !isEditorInput(event->type())) return false;
    if (event->type() == QEvent::KeyPress)
        qCDebug(workspaceDialogLog) << "Key recipient" << watched << "focus" << QApplication::focusWidget()
            << "native focus" << QGuiApplication::focusWindow() << "focus object" << QGuiApplication::focusObject()
            << "proxy" << workspace_.canvasContainer()->focusProxy();
    // Native file/error/save-confirmation dialogs still use Qt's own modality.
    if (QApplication::activeModalWidget()) return false;
    auto* native = qobject_cast<QWindow*>(watched);
    bool nativeChild = false;
    for (auto* ancestor = native; ancestor; ancestor = ancestor->parent())
        if (ancestor == host_.windowHandle()) { nativeChild = true; break; }
    const bool ours = nativeChild || belongsTo(watched, &host_) || belongsTo(watched, workspace_.panelOverlay());
    if (!ours) return false;
    if (localFileDropHandler_ && (event->type() == QEvent::DragEnter
            || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        // Handle both QWidget recipients and native carriers before the input
        // barrier. Opening never moves/deletes the dragged source file, and
        // neither remote URLs nor internal layer drags may bypass the card.
        auto* drop = static_cast<QDropEvent*>(event);
        QStringList paths;
        for (const auto& url : drop->mimeData()->urls())
            if (url.isLocalFile()) paths.push_back(url.toLocalFile());
        if (!paths.isEmpty() && drop->possibleActions().testFlag(Qt::CopyAction)) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            if (event->type() == QEvent::Drop) localFileDropHandler_(paths);
        } else {
            drop->ignore();
        }
        return true;
    }
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept(); // Don't activate editor QActions while the card owns input.
        return true;
    }
    // Wayland can retain keyboard focus on the embedded Vulkan surface even
    // after QWidget focus has moved to the card. A focus proxy alone does not
    // dispatch that QWindow's keys. Deliver native keyboard/IME input to the
    // modal editor (or its popup), never to the canvas underneath it.
    auto* popup = QApplication::activePopupWidget();
    if (native && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
            || event->type() == QEvent::InputMethod)) {
        QWidget* target = QApplication::focusWidget();
        if (popup && belongsTo(popup, dialog_)) {
            if (auto* combo = qobject_cast<QComboBox*>(popupLogicalParent(popup)))
                target = combo->view(); // Container is NoFocus; the list commits Enter.
            else if (!target || !belongsTo(target, popup))
                target = popup->focusWidget() ? popup->focusWidget() : popup;
        } else if (!target || !belongsTo(target, dialog_)) {
            target = lastFocus_;
        }
        if (target && target->isEnabled() && target->isVisible())
            QCoreApplication::sendEvent(target, event);
        event->accept();
        return true;
    }
    // QWidgetWindow must first dispatch native input to its child widget. The
    // barrier then checks that real recipient; swallowing the carrier would
    // also swallow clicks/typing intended for the card itself.
    if ((popup && watched == popup->windowHandle())
        || watched == host_.windowHandle() || watched == workspace_.panelOverlay()->windowHandle()
        || watched == workspace_.canvasContainer()->windowHandle()) return false;
    if (belongsTo(watched, dialog_)) {
        if (!QApplication::activePopupWidget() && event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) { reject(); return true; }
            // Tab goes through shortcut capture / the shared widget policy,
            // never through a modal-only focus traversal path.
        }
        return false;
    }
    // Includes Vulkan QWindow, menus, toolbars and dock panels. Window state,
    // expose, focus and Close events deliberately pass through untouched.
    event->accept();
    return true;
}

void WorkspaceDialog::centerCard()
{
    if (!dialog_) return;
    const auto preferred = dialog_->property("workspacePreferredSize").toSize();
    if (preferred.isValid()) dialog_->setFixedSize(preferred.boundedTo(size() - QSize(24, 24)));
    dialog_->adjustSize();
    dialog_->move((width() - dialog_->width()) / 2, (height() - dialog_->height()) / 2);
}
void WorkspaceDialog::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    centerCard();
}
void WorkspaceDialog::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0, 112));
}
} // namespace imageeditor::ui
