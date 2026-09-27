#pragma once

#include <QToolButton>

namespace imageeditor::ui {

// Shared top-bar button preset. Identity/behavior stay with the owning page;
// metrics and presentation live here, interaction colors live in Theme.
class ToolOptionsButton final : public QToolButton {
public:
    enum class Kind { Action, Toggle };
    enum class Presentation { Text, Icon, Status };

    ToolOptionsButton(const QString& name, const QString& label, const QString& hint,
        Kind kind, QWidget* parent = nullptr, Presentation presentation = Presentation::Text);
};

} // namespace imageeditor::ui
