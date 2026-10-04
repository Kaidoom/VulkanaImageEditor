#pragma once
#include "imageeditor/core/BlendMode.hpp"
#include <QComboBox>

namespace imageeditor::ui {
// Presentation order is independent of the stable document/shader IDs.
inline constexpr std::array blendModeDisplayOrder {
    core::BlendMode::Normal, core::BlendMode::Dissolve,
    core::BlendMode::Darken, core::BlendMode::Multiply,
    core::BlendMode::ColorBurn, core::BlendMode::LinearBurn, core::BlendMode::DarkerColor,
    core::BlendMode::Lighten, core::BlendMode::Screen, core::BlendMode::ColorDodge,
    core::BlendMode::LinearDodge, core::BlendMode::LighterColor,
    core::BlendMode::Overlay, core::BlendMode::SoftLight, core::BlendMode::HardLight,
    core::BlendMode::VividLight, core::BlendMode::LinearLight, core::BlendMode::PinLight,
    core::BlendMode::HardMix, core::BlendMode::Difference, core::BlendMode::Exclusion,
    core::BlendMode::Subtract, core::BlendMode::Divide,
    core::BlendMode::Hue, core::BlendMode::Saturation, core::BlendMode::Color,
    core::BlendMode::Luminosity,
};
static_assert([] {
    if (blendModeDisplayOrder.size() != core::allBlendModes.size()) return false;
    for (auto mode : core::allBlendModes) {
        int count = 0;
        for (auto displayed : blendModeDisplayOrder) count += displayed == mode;
        if (count != 1) return false;
    }
    return true;
}());

inline void populateBlendModeCombo(QComboBox& combo)
{
    for (auto mode : blendModeDisplayOrder)
        combo.addItem(QString::fromUtf8(core::blendModeName(mode)), int(mode));
    combo.setMaxVisibleItems(combo.count());
}

[[nodiscard]] inline std::optional<core::BlendMode> blendModeAt(const QComboBox& combo, int index)
{
    bool valid = false;
    const auto value = combo.itemData(index).toUInt(&valid);
    const auto mode = core::BlendMode(value);
    return valid && core::isValidBlendMode(mode) ? std::optional(mode) : std::nullopt;
}
} // namespace imageeditor::ui
