#pragma once
#include "imageeditor/ui/ScreenCapture.hpp"
#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
class QComboBox;
class QLabel;
class QPushButton;
namespace imageeditor::ui {
class CompactValueControl;
class ScreenshotPreview;
class ScreenshotDialog final : public QDialog {
    Q_OBJECT
public:
    enum class Output { NewDocument, Layer };
    ScreenshotDialog(QWidget& host, QWidget* parent, ScreenCapture* capture=nullptr);
    ~ScreenshotDialog() override;
    QImage selectedImage() const;
    Output output() const { return output_; }
    void setCanAddLayer(bool);
    void showError(const QString&);
    void done(int result) override;
private:
    void beginCapture();
    void cancelCapture(bool showStatus);
    void restoreHost(bool activate);
    void refresh();
    QPointer<QWidget> host_;
    QPointer<ScreenCapture> capture_;
    ScreenshotPreview* preview_{};
    QComboBox* target_{};
    CompactValueControl* delay_{};
    QPushButton *captureButton_{}, *stopButton_{}, *resetButton_{}, *openButton_{}, *layerButton_{}, *resetViewButton_{};
    QLabel *status_{}, *dimensions_{}, *zoom_{};
    QTimer timer_;
    QElapsedTimer clock_;
    quint64 generation_{};
    qint64 delayMs_{};
    bool capturing_{}, minimizedHost_{};
    bool canAddLayer_{};
    Output output_{Output::NewDocument};
    Qt::WindowStates previousState_;
};
}
