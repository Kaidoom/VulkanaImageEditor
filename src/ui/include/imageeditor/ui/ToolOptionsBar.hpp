#pragma once

#include "imageeditor/core/EditorSession.hpp"

#include <QToolBar>

#include <map>

class QLabel;
class QScrollArea;
class QStackedWidget;
class QToolButton;

namespace imageeditor::ui {

// Stable, generic host for contextual tool controls. The toolbar itself never
// changes height or visibility; tools contribute centered pages independently.
class ToolOptionsBar final : public QToolBar {
public:
    explicit ToolOptionsBar(QWidget* parent = nullptr);
    ~ToolOptionsBar() override;

    void registerToolPage(core::ToolId tool, const QString& label, QWidget* page);
    // Optional cached mode controls beside the tool name, outside the centered
    // options page. Mirrored side widths keep the options truly centered.
    void registerToolLeadingWidget(core::ToolId tool, QWidget* widget);
    void setActiveTool(core::ToolId tool);
    void setContextLabel(const QString& label);
    [[nodiscard]] QWidget* pageForTool(core::ToolId tool) const noexcept;
    [[nodiscard]] QWidget* leadingWidgetForTool(core::ToolId tool) const noexcept;
    [[nodiscard]] core::ToolId activeTool() const noexcept { return activeTool_; }

protected:
    bool event(QEvent* event) override;

private:
    void updateOverflowControls();
    void updateSideSlots();

    struct PageRecord {
        QString label;
        QWidget* page {nullptr};
    };

    QLabel* contextLabel_ {nullptr};
    QWidget* leftSlot_ {nullptr};
    QWidget* rightBalance_ {nullptr};
    QWidget* leadingGap_ {nullptr};
    QStackedWidget* leadingStack_ {nullptr};
    QWidget* emptyLeading_ {nullptr};
    QScrollArea* pageViewport_ {nullptr};
    QToolButton* previousOptions_ {nullptr};
    QToolButton* nextOptions_ {nullptr};
    QStackedWidget* pageStack_ {nullptr};
    QWidget* emptyPage_ {nullptr};
    std::map<core::ToolId, PageRecord> pages_;
    std::map<core::ToolId, QWidget*> leadingWidgets_;
    core::ToolId activeTool_ {core::ToolId::Move};
};

} // namespace imageeditor::ui
