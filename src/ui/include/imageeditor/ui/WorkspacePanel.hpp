#pragma once

#include <QWidget>

#include <functional>

class QResizeEvent;

namespace imageeditor::ui {

// A product panel that always remains inside the editor's overlay surface.
// "Floating" is presentation state, not QWidget top-level window state.
class WorkspacePanel final : public QWidget {
public:
    explicit WorkspacePanel(QString title, QWidget* content,
        QWidget* parent = nullptr);

    [[nodiscard]] QString title() const { return title_; }
    void setTitle(const QString& title);
    void setTabbedPresentation(bool tabbed);
    [[nodiscard]] QWidget* contentWidget() const noexcept { return content_; }
    [[nodiscard]] QWidget* resizeGrip() const noexcept { return resizeGrip_; }
    // Top inset plus the draggable header, kept reachable when parked low.
    [[nodiscard]] int headerExtent() const noexcept;
    [[nodiscard]] bool hasFloatingPresentation() const noexcept
    {
        return floatingPresentation_;
    }
    void setFloatingPresentation(bool floating);
    void setHeightRange(int minimum, int maximum);
    [[nodiscard]] int configuredMinimumHeight() const noexcept
    {
        return configuredMinimumHeight_;
    }
    [[nodiscard]] int configuredMaximumHeight() const noexcept
    {
        return configuredMaximumHeight_;
    }

    std::function<void(WorkspacePanel*, QPoint, QPoint)> onDragStarted;
    std::function<void(WorkspacePanel*, QPoint)> onDragMoved;
    std::function<void(WorkspacePanel*, QPoint, bool)> onDragFinished;
    std::function<void(WorkspacePanel*, QPoint)> onResizeStarted;
    std::function<void(WorkspacePanel*, QPoint)> onResizeMoved;
    std::function<void(WorkspacePanel*, bool)> onResizeFinished;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void applyHeightRange();

    QString title_;
    QWidget* content_ {nullptr};
    QWidget* titleBar_ {nullptr};
    QWidget* resizeGrip_ {nullptr};
    int configuredMinimumHeight_ {160};
    int configuredMaximumHeight_ {QWIDGETSIZE_MAX};
    bool floatingPresentation_ {false};
    bool tabbedPresentation_ {false};
};

} // namespace imageeditor::ui
