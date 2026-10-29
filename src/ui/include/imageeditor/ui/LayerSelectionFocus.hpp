#pragma once

namespace imageeditor::ui {
// UI-only policy; serialized values are stable across application versions.
enum class LayerSelectionFocus { Disabled = 0, Focus = 1, Expand = 2 };
constexpr LayerSelectionFocus layerSelectionFocusFromInt(int value)
{
    return value == 0 ? LayerSelectionFocus::Disabled
        : value == 2 ? LayerSelectionFocus::Expand : LayerSelectionFocus::Focus;
}
}
