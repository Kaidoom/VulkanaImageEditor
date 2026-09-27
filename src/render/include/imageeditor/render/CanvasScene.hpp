#pragma once

#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/BrushEngine.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/Geometry.hpp"
#include "imageeditor/core/ViewportState.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/Measurement.hpp"
#include <optional>

namespace imageeditor::render {

struct TransformOverlay {
    core::AffineTransform localToDocument;
    core::Extent2u extent;
    double rotationDegrees {0.0};
    // Shape geometry may be fractional or have a zero line axis. Other tools
    // retain their existing integer document/image frame unchanged.
    std::optional<core::Extent2d> geometryExtent {};
    bool rotationEnabled {true};
    std::optional<core::LayerCrop> cropGeometry {}; // Frame-local border, separate from drag handles.
    bool chamferHandles {false};
    [[nodiscard]] std::array<core::Vec2d,8> frameHandles() const
    {
        return geometryExtent ? core::geometryTransformHandles(localToDocument,*geometryExtent)
            : core::transformHandles(localToDocument,extent);
    }
    [[nodiscard]] std::array<core::Vec2d,8> handles() const
    {
        auto result=frameHandles();
        if(cropGeometry && chamferHandles){
            const auto c=cropGeometry->resolvedCorners();
            const auto w=cropGeometry->width,h=cropGeometry->height;
            const std::array<core::Vec2d,4> p{{{c[0]*.5,c[0]*.5},{w-c[1]*.5,c[1]*.5},
                {w-c[2]*.5,h-c[2]*.5},{c[3]*.5,h-c[3]*.5}}};
            for(std::size_t i=0;i<4;++i)result[i*2]=localToDocument.map(p[i]);
        }
        return result;
    }
};

struct CanvasScene {
    struct TextQuad { std::array<core::Vec2d,4> corners; core::Rgba8 color; };
    std::vector<TextQuad> textQuads;
    core::DocumentSnapshot document;
    std::uint64_t documentInstance {0};
    // Weak retention hints only; document contexts own the actual data. The
    // renderer keeps hot inactive textures within its shared cache budget.
    std::vector<std::weak_ptr<const core::RasterSurface>> retainedSources;
    // Separate display source: original document remains authoritative for all
    // targeting, coordinates and overlays. Never serialized/exported.
    bool pixelPreviewEnabled {false};
    std::shared_ptr<const core::RasterSurface> pixelPreview;
    // Momentary panel comparison is presentation state only: never part of a
    // document snapshot, sampling/export, persistence or document history.
    std::optional<core::LayerId> adjustmentBypassLayer;
    std::optional<core::LayerId> filterBypassLayer;
    std::optional<core::LayerId> effectBypassLayer;
    std::optional<core::LayerId> cropPreviewLayer; // Editor-only retained-source ghost.
    core::ViewportState viewport;
    core::Extent2d logicalViewport;
    double devicePixelRatio {1.0};
    core::Vec2d cursorLogical;
    std::optional<core::Vec2d> constrainedBrushPosition {}; // Document-space, resolved by the engine.
    std::optional<core::Vec2d> cloneSourceAnchor, cloneSourceOffset;
    bool spotHealActive {false};
    bool repairProcessing {false};
    // Stroke marks are view state, independent of raster selection and pixels.
    std::shared_ptr<const std::vector<core::SelectionEdge>> repairRegionEdges;
    core::Revision repairRegionRevision {0};
    std::string pointerTooltip;
    core::RectI pointerTooltipArea; // Unobscured workspace area in logical pixels; empty uses viewport.
    std::optional<core::MeasurementLine> measurement;
    core::SnapGuides snapGuides;
    bool measureActive {false};
    bool cursorInside {false};
    bool eyedropperActive {false};
    bool eyedropperSampleValid {false};
    core::Rgba8 eyedropperReference;
    core::Rgba8 eyedropperCandidate;
    core::ToolId activeTool {core::ToolId::Move};
    double brushSizeDocument {32.0};
    bool quickSelectionCursor {true};
    double quickSelectionSize {24};
    double brushHardness {0.8};
    double brushTipAspectRatio {1.0};
    double brushTipAngleDegrees {0.0};
    double brushTipBaseAngleDegrees {0.0};
    std::optional<TransformOverlay> transformOverlay;
    core::TransformHandle transformHighlight {core::TransformHandle::None};
    core::Rgba8 overlayAccent {101, 119, 243, 255};
    core::Rgba8 layerOutlineColor;
    std::shared_ptr<const std::vector<core::SelectionEdge>> layerOutlineEdges;
    core::Revision layerOutlineRevision {0};
    // Static view-only reminder of the current adjustment's owned mask.
    // Separate from the document selection and never drives ants animation.
    std::shared_ptr<const std::vector<core::SelectionEdge>> capturedRegionEdges;
    core::Revision capturedRegionRevision {0};
    core::Rgba8 canvasBackground {66, 70, 77, 255};
    core::Rgba8 checkerLight {136, 138, 144, 255}, checkerDark {119, 122, 128, 255};
    std::shared_ptr<const std::vector<core::SelectionEdge>> selectionEdges;
    core::Revision selectionEdgesRevision {0};
    bool selectionPathPreview {false};
    core::Revision selectionPathEpoch {0};
    std::size_t selectionStablePrefix {0};
    std::size_t selectionClosingEdges {0}, selectionAnchorCount {0};
    std::shared_ptr<const std::vector<core::SelectionEdge>> selectionRetainedEdges;
    core::Revision selectionRetainedRevision {0};
    double selectionPhase {0.0};
};

} // namespace imageeditor::render
