#pragma once
#include <QImage>
#include <QObject>
#include <QTimer>
#include <QUrl>
#ifdef Q_OS_LINUX
class QDBusMessage;
Q_MOC_INCLUDE(<QDBusMessage>)
#endif

namespace imageeditor::ui {
struct ScreenCaptureResult {
    QImage image;
    QString error;
    bool cancelled{};
};
// Only reads the local file returned by the screenshot portal. No remote URLs,
// clipboard transfers or deletion of paths owned by another process.
ScreenCaptureResult readScreenCapture(const QUrl&);
class ScreenCapture : public QObject {
    Q_OBJECT
public:
    enum class Target { DesktopChooser, EntireDesktop, ActiveWindow };
    explicit ScreenCapture(QObject* parent=nullptr);
    ~ScreenCapture() override;
    virtual bool usesDesktopChooser() const;
    virtual void start(quint64 request, Target);
    virtual void cancel();
signals:
    void finished(quint64 request, QImage image, QString error, bool cancelled);
private:
    void complete(ScreenCaptureResult);
    quint64 generation_{}, request_{};
    bool active_{};
    QTimer deadline_;
#ifdef Q_OS_LINUX
    QString path_;
    void unsubscribe();
    bool subscribe();
private slots:
    void response(uint code, const QVariantMap&, const QDBusMessage&);
#endif
};
}
