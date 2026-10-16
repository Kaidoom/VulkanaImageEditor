#include "LayerDrag.hpp"
#include "imageeditor/ui/LayerListView.hpp"

#include <QApplication>
#include <QDragEnterEvent>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QWheelEvent>
#include <QWindow>

namespace imageeditor::ui {
namespace {
class LayerDrag final : public QObject {
public:
    LayerDrag(QWidget* source, const QMimeData* mime, const QPixmap& pixmap)
        : source_(source), window_(source->window()), mime_(mime)
    {
        preview_ = new QLabel(source->window(), Qt::ToolTip | Qt::FramelessWindowHint
            | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        preview_->setAttribute(Qt::WA_TransparentForMouseEvents);
        preview_->setAttribute(Qt::WA_ShowWithoutActivating);
        preview_->setPixmap(pixmap);
        preview_->adjustSize();
        connect(source, &QObject::destroyed, this, [this] { finish(); });
    }

    ~LayerDrag() override { delete preview_; }

    void exec(const QPoint& global)
    {
        QApplication::setOverrideCursor(Qt::DragMoveCursor);
        qApp->installEventFilter(this);
        move(global, Qt::LeftButton, QGuiApplication::keyboardModifiers());
        if (!finished_ && source_) {
            // The first DragEnter hands ownership away from the cross-surface
            // pointer router. Keep the remaining press/release in this session.
            source_->grabMouse();
            loop_.exec();
        }
        leave();
        if (source_ && QWidget::mouseGrabber() == source_) source_->releaseMouse();
        qApp->removeEventFilter(this);
        QApplication::restoreOverrideCursor();
        if (preview_) preview_->hide();
    }

private:
    static constexpr auto actions = Qt::MoveAction | Qt::CopyAction;
    QPointer<QWidget> source_, window_, target_;
    QPointer<QLabel> preview_;
    const QMimeData* mime_;
    QEventLoop loop_;
    bool forwardingWheel_ {false};
    bool entered_ {false};
    bool accepted_ {false};
    bool finished_ {false};

    void finish() { finished_ = true; loop_.quit(); }

    void leave()
    {
        if (target_) {
            QDragLeaveEvent event;
            QApplication::sendEvent(target_, &event);
        }
        target_.clear();
        entered_ = accepted_ = false;
    }

    void move(const QPoint& global, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers)
    {
        if (preview_) {
            preview_->move(global - QPoint(12, 18));
            preview_->show();
        }
        auto* hit = QApplication::widgetAt(global);
        auto* root = window_.data();
        while (root && root->parentWidget()) root = root->parentWidget()->window();
        // The panels are masked native child surfaces above the canvas. Global
        // widgetAt can report the underlying canvas on XCB, or nothing during
        // a Wayland grab. Resolve our own surfaces in their stacking order,
        // using the same input mask as the compositor; retain other windows'
        // normal hit-testing results.
        if (!hit || (root && (hit == root || root->isAncestorOf(hit)))) {
            for (auto* surface = window_.data(); surface;
                 surface = surface->parentWidget() ? surface->parentWidget()->window() : nullptr) {
                const auto local = surface->mapFromGlobal(global);
                const auto* native = surface->windowHandle();
                const auto mask = native ? native->mask() : QRegion{};
                if (surface->isVisible() && surface->rect().contains(local)
                    && (!native || !native->flags().testFlag(Qt::WindowTransparentForInput))
                    && (mask.isEmpty() || mask.contains(local))) {
                    hit = surface->childAt(local);
                    if (!hit) hit = surface;
                    break;
                }
            }
        }
        while (hit && !hit->acceptDrops() && !hit->isWindow()) hit = hit->parentWidget();
        if (hit && (!hit->acceptDrops() || !hit->isEnabled())) hit = nullptr;
        if (hit != target_) { leave(); target_ = hit; }
        if (target_ && !entered_) {
            QDragEnterEvent enter(target_->mapFromGlobal(global), actions, mime_, buttons, modifiers);
            QApplication::sendEvent(target_, &enter);
            entered_ = enter.isAccepted();
        }
        accepted_ = false;
        auto cursor = Qt::ForbiddenCursor;
        if (target_ && entered_) {
            QDragMoveEvent event(target_->mapFromGlobal(global), actions, mime_, buttons, modifiers);
            QApplication::sendEvent(target_, &event);
            accepted_ = event.isAccepted();
            if (accepted_) cursor = event.dropAction() == Qt::CopyAction ? Qt::DragCopyCursor : Qt::DragMoveCursor;
        }
        QApplication::changeOverrideCursor(cursor);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (finished_ || (forwardingWheel_ && event->type() == QEvent::Wheel)) return false;
        switch (event->type()) {
        case QEvent::MouseMove: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (!(mouse->buttons() & Qt::LeftButton)) finish();
            else move(mouse->globalPosition().toPoint(), mouse->buttons(), mouse->modifiers());
            return true;
        }
        case QEvent::MouseButtonRelease: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) return true;
            const auto global = mouse->globalPosition().toPoint();
            move(global, mouse->buttons(), mouse->modifiers());
            if (target_ && accepted_) {
                QDropEvent drop(target_->mapFromGlobal(global), actions, mime_, mouse->buttons(), mouse->modifiers());
                QApplication::sendEvent(target_, &drop);
            }
            finish();
            // Let the source clear its ordinary pressed-row state after the
            // drag loop unwinds, without a second model move/removal.
            if (source_) QCoreApplication::postEvent(source_, new QMouseEvent(QEvent::MouseButtonRelease,
                source_->mapFromGlobal(global), global, Qt::LeftButton, mouse->buttons(), mouse->modifiers()));
            return true;
        }
        case QEvent::Wheel: {
            const auto* wheel = static_cast<QWheelEvent*>(event);
            const auto global = wheel->globalPosition().toPoint();
            move(global, wheel->buttons(), wheel->modifiers());
            auto* widget = target_.data();
            while (widget && !dynamic_cast<LayerListView*>(widget)) widget = widget->parentWidget();
            if (auto* view = dynamic_cast<LayerListView*>(widget)) {
                QWheelEvent forwarded(view->viewport()->mapFromGlobal(global), wheel->globalPosition(),
                    wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(), wheel->modifiers(),
                    wheel->phase(), wheel->inverted(), wheel->source(), wheel->pointingDevice());
                forwardingWheel_ = true;
                QApplication::sendEvent(view->viewport(), &forwarded);
                forwardingWheel_ = false;
                move(global, wheel->buttons(), wheel->modifiers());
            }
            // A drag must not zoom the canvas or operate another panel.
            event->accept();
            return true;
        }
        case QEvent::ShortcutOverride:
            event->accept();
            return true;
        case QEvent::KeyPress:
            if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) finish();
            return true;
        case QEvent::KeyRelease:
            return true;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            finish();
            return true;
        case QEvent::ApplicationDeactivate:
        case QEvent::TouchCancel:
            finish();
            break;
        case QEvent::Close:
        case QEvent::Hide:
            if (watched == source_ || watched == window_) finish();
            break;
        default: break;
        }
        return false;
    }
};
}

void dragLayers(QWidget* source, const QMimeData* mime, const QPixmap& preview, const QPoint& globalPosition)
{
    LayerDrag(source, mime, preview).exec(globalPosition);
}
}
