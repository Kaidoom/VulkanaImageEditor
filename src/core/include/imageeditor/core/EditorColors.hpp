#pragma once

#include "imageeditor/core/Geometry.hpp"

namespace imageeditor::core {

enum class ColorSlot { Primary, Secondary };

// Session-only, straight RGBA8: RGB is sRGB-encoded, alpha is linear coverage.
// The highlighted slot supplies the foreground for every color-consuming tool.
struct EditorColors {
    Rgba8 primary {79, 115, 255, 255};
    Rgba8 secondary {255, 255, 255, 255};
    ColorSlot active {ColorSlot::Primary};

    [[nodiscard]] Rgba8 color(ColorSlot slot) const noexcept
    {
        return slot == ColorSlot::Primary ? primary : secondary;
    }
    [[nodiscard]] Rgba8 foreground() const noexcept { return color(active); }
    [[nodiscard]] Rgba8 background() const noexcept
    { return color(active == ColorSlot::Primary ? ColorSlot::Secondary : ColorSlot::Primary); }
    void setColor(ColorSlot slot, Rgba8 value) noexcept
    {
        (slot == ColorSlot::Primary ? primary : secondary) = value;
    }
    void switchActive() noexcept
    {
        active = active == ColorSlot::Primary ? ColorSlot::Secondary : ColorSlot::Primary;
    }
    friend bool operator==(const EditorColors&, const EditorColors&) = default;
};

} // namespace imageeditor::core
