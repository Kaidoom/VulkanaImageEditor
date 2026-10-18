#pragma once
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include <QWidget>
class QGridLayout;
namespace imageeditor::ui {
class ToolShortcutHints final : public QWidget {
public:
    explicit ToolShortcutHints(QWidget* parent = nullptr);
    void setTool(core::ToolId);
    void setBindings(const ShortcutBindings&);

private:
    void rebuild();
    QGridLayout* grid_;
    core::ToolId tool_ { core::ToolId::Move };
    ShortcutBindings bindings_ = defaultShortcutBindings();
};
}
