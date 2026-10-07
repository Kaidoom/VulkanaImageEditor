#pragma once

#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <functional>

class QDialog;

namespace imageeditor::ui {
class OverlayDockWorkspace;

// Application-local modality: a QWidget card on the existing ARGB panel plane,
// not another Wayland toplevel. The host's window-manager controls stay usable.
class WorkspaceDialog final : public QWidget {
public:
    WorkspaceDialog(OverlayDockWorkspace& workspace, QWidget& host);
    ~WorkspaceDialog() override;
    int exec(QDialog& dialog);
    void open(QDialog& dialog);
    void finish();
    [[nodiscard]] bool active() const noexcept { return running_; }
    void reject();
    // Opt-in for a starter card: local files dropped anywhere in its owning
    // application are open requests, not edits to the document behind it.
    void setLocalFileDropHandler(std::function<void(const QStringList&)> handler);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void centerCard();
    void focusControl(QWidget* control);
    void restoreCardFocus();
    OverlayDockWorkspace& workspace_;
    QPointer<OverlayDockWorkspace> workspaceGuard_;
    QWidget& host_;
    QPointer<QWidget> previousOverlay_;
    QPointer<QDialog> dialog_;
    QPointer<QWidget> previousFocus_, previousProxy_, lastFocus_;
    std::function<void(const QStringList&)> localFileDropHandler_;
    bool running_ {false};
};
} // namespace imageeditor::ui
