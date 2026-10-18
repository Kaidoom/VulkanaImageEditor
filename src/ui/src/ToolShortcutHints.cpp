#include "imageeditor/ui/ToolShortcutHints.hpp"
#include "imageeditor/ui/Localization.hpp"
#include <QGridLayout>
#include <QLabel>
namespace imageeditor::ui {
ToolShortcutHints::ToolShortcutHints(QWidget* parent)
    : QWidget(parent)
    , grid_(new QGridLayout(this))
{
    setObjectName("ToolShortcutHints");
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(10);
    grid_->setVerticalSpacing(7);
    grid_->setColumnStretch(1, 1);
    connect(
        &i18n::Localization::instance(), &i18n::Localization::languageChanged, this, [this] { rebuild(); });
    rebuild();
}
void ToolShortcutHints::setTool(core::ToolId tool)
{
    if (tool_ != tool) {
        tool_ = tool;
        rebuild();
    }
}
void ToolShortcutHints::setBindings(const ShortcutBindings& bindings)
{
    bindings_ = bindings;
    rebuild();
}
void ToolShortcutHints::rebuild()
{
    while (auto* item = grid_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    int row = 0;
    const auto add = [&](const QString& keys, const i18n::Message& label, const QString& id = QString { }) {
        auto* cap = new QLabel(keys, this);
        cap->setObjectName("ShortcutKeycap");
        cap->setProperty("shortcutAction", id);
        cap->setTextFormat(Qt::PlainText);
        cap->setLayoutDirection(Qt::LeftToRight);
        cap->setWordWrap(true);
        cap->setAlignment(Qt::AlignCenter);
        cap->setMaximumWidth(120);
        cap->setMinimumWidth(std::min(120, cap->fontMetrics().horizontalAdvance(keys) + 14));
        cap->setStyleSheet("QLabel {background:palette(button);border:1px solid "
                           "palette(mid);border-radius:4px;padding:3px 6px;font-weight:600;}");
        auto* description = i18n::make<QLabel>(label);
        description->setWordWrap(true);
        description->setMinimumWidth(50);
        grid_->addWidget(cap, row, 0, Qt::AlignTop);
        grid_->addWidget(description, row++, 1);
    };
    const auto command = [&](const char* id) {
        if (const auto* d = shortcutDefinition(id))
            add(shortcutLabel(bindings_, id), d->label, id);
    };
    using T = core::ToolId;
    if (tool_ == T::Brush || tool_ == T::Eraser || tool_ == T::Cloning || tool_ == T::LocalBlur) {
        command("DecreaseBrushSizeAction");
        command("IncreaseBrushSizeAction");
        add("Shift", i18n::text("properties.shortcuts.constrain"));
        if (tool_ == T::Brush || tool_ == T::Eraser) {
            command("ToolAction_eraser");
            command("SwapColorsAction");
            command("ResetColorsAction");
            add("Alt", i18n::text("properties.shortcuts.sample"));
        }
        if (tool_ == T::Cloning)
            add("Alt + click", i18n::text("properties.shortcuts.source"));
    } else if (core::isSelectionTool(tool_)) {
        add("Shift", i18n::text("properties.shortcuts.add"));
        add("Alt", i18n::text("properties.shortcuts.subtract"));
        add("Shift + Alt", i18n::text("properties.shortcuts.intersect"));
        command("DeselectAction");
        command("LayerTransformAction");
        if (tool_ == T::Lasso) {
            command("FinishOperationAction");
            command("RemovePointAction");
        }
        if (tool_ == T::SmartSelect) {
            command("DecreaseBrushSizeAction");
            command("IncreaseBrushSizeAction");
        }
    } else if (tool_ == T::Text) {
        command("FinishTextAction");
        command("LayerTransformAction");
    } else if (tool_ == T::Shape) {
        command("FinishOperationAction");
        command("RemovePointAction");
        command("LayerTransformAction");
        add("Shift", i18n::text("properties.shortcuts.constrain"));
    } else if (tool_ == T::Crop || tool_ == T::CanvasResize || tool_ == T::Transform) {
        command("FinishOperationAction");
        add("Shift", i18n::text("properties.shortcuts.ratio"));
        add("Alt", i18n::text("properties.shortcuts.center"));
    } else if (tool_ == T::Move) {
        command("LayerTransformAction");
        command("DuplicateLayersAction");
        command("NudgeLeftAction");
        command("NudgeRightAction");
        command("NudgeUpAction");
        command("NudgeDownAction");
    } else if (tool_ == T::Fill) {
        command("FillForegroundAction");
        command("FillBackgroundAction");
        command("SwapColorsAction");
    } else if (tool_ == T::Measure) {
        command("TemporaryMeasureAction");
        add("Shift", i18n::text("properties.shortcuts.constrain"));
    }
    add("Esc", i18n::text("properties.shortcuts.cancel"));
    command("PanCanvasAction");
    add(i18n::text("properties.shortcuts.middle"), i18n::text("properties.shortcuts.pan"));
}
}
