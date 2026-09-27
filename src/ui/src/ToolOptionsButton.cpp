#include "imageeditor/ui/ToolOptionsButton.hpp"

namespace imageeditor::ui {

ToolOptionsButton::ToolOptionsButton(const QString& name, const QString& label,
    const QString& hint, Kind kind, QWidget* parent, Presentation presentation)
    : QToolButton(parent)
{
    setObjectName(name);
    setProperty("toolOptionsButton", true);
    setText(label);
    setAccessibleName(label);
    setToolTip(hint);
    setAutoRaise(false);
    setCheckable(kind == Kind::Toggle);
    // Buttons must not steal focus from an active canvas gesture or numeric edit.
    // A page may explicitly opt into keyboard focus (e.g. Brush Direction).
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(30);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    if (presentation == Presentation::Icon) {
        setProperty("toolOptionsIcon", true);
        setFixedSize(28, 28);
        setIconSize({18, 18});
        setToolButtonStyle(Qt::ToolButtonIconOnly);
    } else if (presentation == Presentation::Status) {
        setProperty("toolOptionsStatus", true);
        setFixedHeight(fontMetrics().height());
    }
}

} // namespace imageeditor::ui
