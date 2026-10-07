#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/ColorPicker.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include <QApplication>
#include <QCursor>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>
#ifdef Q_OS_LINUX
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#include <cmath>

namespace imageeditor::ui {
namespace {
class ColorPreview final : public QWidget {
public:
    ColorPreview(ColorDialog& dialog, QColor initial) : QWidget(&dialog), dialog_(dialog), initial_(initial) {
        setMinimumHeight(26); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(tr("Original color / New color"));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        for (int y = 0; y < height(); y += 6) for (int x = 0; x < width(); x += 6)
            p.fillRect(x, y, 6, 6, (x / 6 + y / 6) % 2 ? QColor(94, 98, 111) : QColor(160, 164, 175));
        p.fillRect(0, 0, width() / 2, height(), initial_);
        p.fillRect(width() / 2, 0, width() - width() / 2, height(), dialog_.currentColor());
        p.setPen(palette().color(QPalette::Mid)); p.setBrush(Qt::NoBrush); p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
private:
    ColorDialog& dialog_;
    QColor initial_;
};
}

// Wayland uses the desktop's color-pick portal (no screen capture permissions or
// private Qt APIs). Other desktop backends use Qt's screen-coordinate capture.
class ScreenColorPick final : public QObject {
    Q_OBJECT
public:
    explicit ScreenColorPick(QWidget* owner) : QObject(owner), owner_(owner) {}
    ~ScreenColorPick() override { stop(); }
    void start() {
        stop();
#ifdef Q_OS_LINUX
        const auto platform = QGuiApplication::platformName();
        if (platform.startsWith("wayland") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland") {
            const auto bus = QDBusConnection::sessionBus();
            auto sender = bus.baseService(); sender.remove(0, 1); sender.replace('.', '_');
            const auto token = "vulkana_color_" + QUuid::createUuid().toString(QUuid::Id128);
            path_ = "/org/freedesktop/portal/desktop/request/" + sender + '/' + token;
            if (!bus.isConnected() || !subscribe()) { stop(); emit finished({}, tr("Desktop color picking is unavailable.")); return; }
            const auto expected = path_;
            auto request = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                        "org.freedesktop.portal.Screenshot", "PickColor");
            // An empty parent identifier is permitted; never pass a Wayland
            // subsurface's winId as though it were an exported desktop handle.
            request << QString{} << QVariantMap{{"handle_token", token}};
            auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(request, 3000), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, expected](QDBusPendingCallWatcher* watcher) {
                const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                watcher->deleteLater();
                if (path_ != expected) return;
                if (reply.isError()) { stop(); emit finished({}, tr("Desktop color picking is unavailable.")); return; }
                if (reply.value().path() != path_) {
                    unsubscribe(); path_ = reply.value().path();
                    if (!subscribe()) { stop(); emit finished({}, tr("Could not start desktop color picking.")); }
                }
            });
            return;
        }
#endif
        picking_ = true;
        qApp->installEventFilter(this);
        owner_->grabMouse(Qt::CrossCursor); owner_->grabKeyboard();
    }
    void stop() {
#ifdef Q_OS_LINUX
        if (!path_.isEmpty()) {
            unsubscribe();
            auto close = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", path_, "org.freedesktop.portal.Request", "Close");
            QDBusConnection::sessionBus().asyncCall(close, 1000);
            path_.clear();
        }
#endif
        if (picking_) {
            picking_ = false;
            qApp->removeEventFilter(this);
            if (QWidget::mouseGrabber() == owner_) owner_->releaseMouse();
            if (QWidget::keyboardGrabber() == owner_) owner_->releaseKeyboard();
        }
    }
signals:
    void finished(QColor color, QString error);
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (!picking_) return false;
        if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        if (event->type() == QEvent::ApplicationDeactivate
            || (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)) {
            stop(); emit finished({}, {}); return true;
        }
        QPoint position;
        bool choose = false;
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) { stop(); emit finished({}, {}); return true; }
            position = mouse->globalPosition().toPoint(); choose = true;
        } else if (event->type() == QEvent::KeyPress) {
            const auto key = static_cast<QKeyEvent*>(event)->key();
            choose = key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space;
            position = QCursor::pos();
        }
        if (choose) {
            QColor color;
            if (auto* screen = QGuiApplication::screenAt(position)) {
                const auto local = position - screen->geometry().topLeft();
                const auto image = screen->grabWindow(0, local.x(), local.y(), 1, 1).toImage();
                if (!image.isNull()) color = image.pixelColor(0, 0);
            }
            stop(); emit finished(color, color.isValid() ? QString{} : tr("Could not sample this screen."));
            return true;
        }
        return event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove
            || event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease;
    }
private slots:
#ifdef Q_OS_LINUX
    void response(uint result, const QVariantMap& values, const QDBusMessage& message) {
        if (path_.isEmpty() || message.path() != path_) return;
        unsubscribe(); path_.clear();
        if (result == 1) { emit finished({}, {}); return; }
        const auto color = values.value("color");
        if (result != 0 || !color.canConvert<QDBusArgument>()) { emit finished({}, tr("Could not sample this screen.")); return; }
        const auto argument = color.value<QDBusArgument>();
        if (argument.currentSignature() != "(ddd)") { emit finished({}, tr("Invalid desktop color response.")); return; }
        double r = 0, g = 0, b = 0;
        argument.beginStructure(); argument >> r >> g >> b; argument.endStructure();
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || r < 0 || g < 0 || b < 0 || r > 1 || g > 1 || b > 1) {
            emit finished({}, tr("Invalid desktop color response.")); return;
        }
        emit finished(QColor::fromRgbF(float(r), float(g), float(b)), {});
    }
#endif
private:
#ifdef Q_OS_LINUX
    bool subscribe() {
        return QDBusConnection::sessionBus().connect("org.freedesktop.portal.Desktop", path_, "org.freedesktop.portal.Request",
            "Response", this, SLOT(response(uint,QVariantMap,QDBusMessage)));
    }
    void unsubscribe() {
        QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", path_, "org.freedesktop.portal.Request",
            "Response", this, SLOT(response(uint,QVariantMap,QDBusMessage)));
    }
    QString path_;
#endif
    QPointer<QWidget> owner_;
    bool picking_ {};
};

ColorDialog::ColorDialog(QWidget* parent) : ColorDialog(Qt::white, parent) {}
ColorDialog::ColorDialog(const QColor& color, QWidget* parent) : QDialog(parent, Qt::SubWindow), initial_(color) {
    // Resolve the shared native-canvas workspace, not the nearest overlay's
    // QWidgetWindow. Color cards never create another desktop toplevel.
    presenter_ = dynamic_cast<WorkspaceDialog*>(parent);
    if (!presenter_ && parent) {
        auto* host = parent;
        while (host->parentWidget()) host = host->parentWidget();
        auto* workspace = dynamic_cast<OverlayDockWorkspace*>(host);
        if (!workspace) for (auto* widget : host->findChildren<QWidget*>()) {
            workspace = dynamic_cast<OverlayDockWorkspace*>(widget);
            if (workspace) break;
        }
        if (workspace) {
            presenter_ = new WorkspaceDialog(*workspace, *host);
            ownsPresenter_ = true;
            setParent(presenter_, Qt::SubWindow);
        }
    }
    setWindowTitle(tr("Select color"));
    setSizeGripEnabled(false);
    setFixedSize(360, 470);
    setProperty("workspacePreferredSize", size());
    setAttribute(Qt::WA_StyledBackground);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(12, 12, 12, 12); layout->setSpacing(8);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel(windowTitle(), this);
    title->setObjectName("ColorDialogTitle");
    connect(this, &QWidget::windowTitleChanged, title, &QLabel::setText);
    header->addWidget(title, 1);
    auto* close = new QPushButton(toolGlyph(ToolGlyph::Close), {}, this);
    close->setObjectName("ColorDialogClose"); close->setToolTip(tr("Close"));
    close->setAccessibleName(tr("Close color picker")); close->setAutoDefault(false);
    close->setStyleSheet("QPushButton { padding: 0; min-width: 26px; max-width: 26px; min-height: 26px; max-height: 26px; background: transparent; }");
    close->setFixedSize(28, 28); close->setIconSize({18, 18});
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    header->addWidget(close); layout->addLayout(header);
    picker_ = new ColorPicker(this); picker_->setAlphaEnabled(false); picker_->setColor(color);
    layout->addWidget(picker_, 1);
    auto* row = new QHBoxLayout;
    preview_ = new ColorPreview(*this, color); row->addWidget(preview_, 1);
    screenButton_ = new QPushButton(themedIcon(QStringLiteral(":/theme/eyedropper.svg")), QString(), this);
    screenButton_->setObjectName("PickScreenColor"); screenButton_->setToolTip(tr("Pick screen color"));
    screenButton_->setAccessibleName(screenButton_->toolTip());
    screenButton_->setStyleSheet("QPushButton { padding: 0; min-width: 28px; max-width: 28px; min-height: 26px; max-height: 26px; }");
    screenButton_->setFixedSize(30, 28); screenButton_->setIconSize({18, 18});
    screenButton_->setEnabled(QGuiApplication::platformName() != "offscreen" && QGuiApplication::platformName() != "minimal");
    row->addWidget(screenButton_); layout->addLayout(row);
    status_ = new QLabel(this); status_->setWordWrap(true); status_->hide(); layout->addWidget(status_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this); layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    picker_->onColorChanged = [this](QColor color) { preview_->update(); emit currentColorChanged(color); };
    auto* screen = new ScreenColorPick(this); screenPicker_ = screen;
    connect(screenButton_, &QPushButton::clicked, this, [this, screen] {
        screenButton_->setEnabled(false); status_->setText(tr("Select a screen color; Escape cancels.")); status_->show(); screen->start();
    });
    connect(screen, &ScreenColorPick::finished, this, [this](QColor color, const QString& error) {
        screenButton_->setEnabled(true); status_->setText(error); status_->setVisible(!error.isEmpty());
        if (color.isValid()) { color.setAlpha(currentColor().alpha()); setCurrentColor(color); }
    });
}
ColorDialog::~ColorDialog() {
    static_cast<ScreenColorPick*>(screenPicker_)->stop();
    if (ownsPresenter_ && presenter_) { presenter_->finish(); presenter_->deleteLater(); }
}
void ColorDialog::setVisible(bool visible) {
    if (visible && ownsPresenter_ && presenter_ && !presenter_->active()) {
        presenter_->open(*this);
        return;
    }
    QDialog::setVisible(visible);
    if (!visible && ownsPresenter_ && presenter_) presenter_->finish();
    if (visible && !presenter_ && parentWidget())
        move((parentWidget()->width() - width()) / 2, (parentWidget()->height() - height()) / 2);
}
int ColorDialog::exec() {
    if (presenter_) return presenter_->exec(*this);
    // Standalone widget hosts (including tests) still use an embedded card.
    // The editor always takes the WorkspaceDialog input/focus path above.
    QEventLoop loop;
    connect(this, &QDialog::finished, &loop, &QEventLoop::quit);
    connect(qApp, &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    setResult(Rejected); show(); loop.exec();
    return result();
}
QColor ColorDialog::currentColor() const { return picker_->color(); }
void ColorDialog::setCurrentColor(const QColor& color) {
    const auto before = currentColor(); picker_->setColor(color); preview_->update();
    if (currentColor() != before) emit currentColorChanged(currentColor());
}
void ColorDialog::setOptions(ColorDialogOptions options) {
    const bool alphaWasEnabled = picker_->alphaEnabled();
    options_ = options | DontUseNativeDialog;
    picker_->setAlphaEnabled(options_.testFlag(ShowAlphaChannel));
    // Callers conventionally enable alpha after constructing with an RGBA color.
    if (!alphaWasEnabled && picker_->alphaEnabled()) { auto color = currentColor(); color.setAlpha(initial_.alpha()); picker_->setColor(color); }
    buttons_->setVisible(!options_.testFlag(NoButtons)); preview_->update();
}
void ColorDialog::setOption(ColorDialogOption option, bool on) { auto value = options_; value.setFlag(option, on); setOptions(value); }
void ColorDialog::done(int result) {
    static_cast<ScreenColorPick*>(screenPicker_)->stop();
    screenButton_->setEnabled(QGuiApplication::platformName() != "offscreen" && QGuiApplication::platformName() != "minimal");
    status_->hide();
    selected_ = result == Accepted ? currentColor() : QColor{};
    QPointer<ColorDialog> guarded(this);
    if (selected_.isValid()) emit colorSelected(selected_);
    if (guarded) QDialog::done(result);
}
QColor ColorDialog::getColor(const QColor& initial, QWidget* parent, const QString& title, ColorDialogOptions options) {
    ColorDialog dialog(initial, parent); dialog.setOptions(options);
    if (!title.isEmpty()) dialog.setWindowTitle(title);
    return dialog.exec() == Accepted ? dialog.selectedColor() : QColor{};
}
} // namespace imageeditor::ui
#include "ColorDialog.moc"
