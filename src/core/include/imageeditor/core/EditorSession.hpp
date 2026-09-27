#pragma once

#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/EditorColors.hpp"
#include "imageeditor/core/History.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace imageeditor::core {

enum class ToolId {
    Move,
    Marquee,
    Lasso,
    Brush,
    Eraser,
    Fill,
    Eyedropper,
    Text,
    Transform, // Temporary, explicitly applied/cancelled layer-geometry mode.
    Shape,
    Measure,
    SelectByColor,
    Crop,
    SmartSelect,
    Cloning,
    LocalBlur,
};

[[nodiscard]] constexpr bool isSelectionTool(ToolId tool) noexcept
{
    return tool == ToolId::Marquee || tool == ToolId::Lasso || tool == ToolId::SelectByColor || tool == ToolId::SmartSelect;
}

class EditorSession {
public:
    void replaceDocument(std::unique_ptr<Document> document);

    [[nodiscard]] Document* document() noexcept { return document_.get(); }
    [[nodiscard]] const Document* document() const noexcept { return document_.get(); }
    [[nodiscard]] History& history() noexcept { return history_; }
    [[nodiscard]] const History& history() const noexcept { return history_; }

    [[nodiscard]] std::optional<LayerId> activeLayer() const noexcept { return activeLayer_; }
    // Single-target tools deliberately replace the layer selection. Layer
    // selection is session state: it never changes content or history.
    void setActiveLayer(std::optional<LayerId> layerId);
    [[nodiscard]] const std::vector<LayerId>& selectedLayers() const noexcept { return selectedLayers_; }
    [[nodiscard]] bool isLayerSelected(LayerId layerId) const noexcept;
    [[nodiscard]] std::optional<LayerId> selectionAnchor() const noexcept { return selectionAnchor_; }
    [[nodiscard]] LayerSelectionState layerSelectionState() const { return {selectedLayers_,activeLayer_,selectionAnchor_}; }
    // Unknown IDs and duplicates are discarded. The primary must belong to
    // the selection; a valid range anchor need not (e.g. Ctrl-toggled off).
    void setLayerSelection(std::span<const LayerId> layerIds,
        std::optional<LayerId> primary = { }, std::optional<LayerId> anchor = { });
    void toggleSelectedLayer(LayerId layerId);
    // The panel supplies its displayed row order, never storage indices.
    void selectLayerRange(LayerId layerId, std::span<const LayerId> displayedOrder,
        bool additive = false);

    [[nodiscard]] ToolId activeTool() const noexcept { return activeTool_; }
    void setActiveTool(ToolId tool) noexcept { activeTool_ = tool; }

    [[nodiscard]] Rgba8 foregroundColor() const noexcept
    {
        return colors_.foreground();
    }
    void setForegroundColor(Rgba8 color) noexcept { colors_.setColor(colors_.active, color); }
    [[nodiscard]] const EditorColors& colors() const noexcept { return colors_; }
    void setColors(EditorColors colors) noexcept { colors_ = colors; }
    [[nodiscard]] ColorSampleSource colorSampleSource() const noexcept { return colorSampleSource_; }
    void setColorSampleSource(ColorSampleSource source) noexcept { colorSampleSource_ = source; }

    bool execute(std::unique_ptr<Command> command);
    bool adoptApplied(std::unique_ptr<Command> command);
    bool undo();
    bool redo();

private:
    void normalizeActiveLayer();
    void applyActiveLayerHint();
    [[nodiscard]] std::optional<LayerId> topmostSelectedLayer() const noexcept;

    std::unique_ptr<Document> document_;
    History history_;
    std::optional<LayerId> activeLayer_;
    std::vector<LayerId> selectedLayers_;
    std::optional<LayerId> selectionAnchor_;
    ToolId activeTool_ { ToolId::Move };
    EditorColors colors_;
    ColorSampleSource colorSampleSource_ { ColorSampleSource::MergedVisible };
};

} // namespace imageeditor::core
