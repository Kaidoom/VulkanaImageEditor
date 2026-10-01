#pragma once

#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/LayerCrop.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace imageeditor::core {

struct RasterEditTransactionOptions {
    std::uint32_t journalTileSize {64};
    const SelectionMaskInput* selectionMask {nullptr};
    bool ignoreCrop {false}; // Explicit whole-source clear, not ordinary painting.
    bool coverageValues {false}; // Numeric R8 mask edits, not sRGB colors.
};

enum class RasterEditCommitResult {
    Committed,
    NoChanges,
    TargetUnavailable,
    HistoryRejected,
};

// One retained tile is always a clipped fixed-grid tile. pixels contains the
// alternate state; changedRegions is a disjoint exact pixel-region cover used
// by both swap directions and Vulkan invalidation.
class RasterEditCommand final : public Command {
public:
    struct TileSnapshot {
        RectI tileRegion;
        std::vector<std::byte> pixels;
        std::vector<RectI> changedRegions;
    };

    bool apply(Document& document) override;
    bool undo(Document& document) override;
    [[nodiscard]] std::string_view label() const noexcept override;
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] bool canAdoptApplied(const Document& document) const noexcept override;

    [[nodiscard]] LayerId layerId() const noexcept { return layerId_; }
    [[nodiscard]] SurfaceId surfaceId() const noexcept { return surfaceId_; }
    [[nodiscard]] std::size_t tileCount() const noexcept { return tiles_.size(); }
    [[nodiscard]] std::size_t journalPixelBytes() const noexcept;

private:
    friend class RasterEditTransaction;

    RasterEditCommand(LayerId layerId, SurfaceId surfaceId, Extent2u surfaceExtent,
        std::string_view label, std::vector<TileSnapshot> tiles);
    [[nodiscard]] RasterSurface* resolve(Document& document) const noexcept;
    [[nodiscard]] const RasterSurface* resolve(const Document& document) const noexcept;
    bool swapWithSurface(Document& document);
    bool swapWithSurface(RasterSurface& surface);

    LayerId layerId_ {0};
    SurfaceId surfaceId_ {0};
    Extent2u surfaceExtent_;
    std::array<char, 48> label_ {};
    std::uint8_t labelLength_ {0};
    std::vector<TileSnapshot> tiles_;
};

class RasterEditTransaction final {
public:
    RasterEditTransaction(Document& document, LayerId layerId,
        std::string_view historyLabel,
        RasterEditTransactionOptions options = {});
    ~RasterEditTransaction();

    RasterEditTransaction(const RasterEditTransaction&) = delete;
    RasterEditTransaction& operator=(const RasterEditTransaction&) = delete;
    RasterEditTransaction(RasterEditTransaction&&) = delete;
    RasterEditTransaction& operator=(RasterEditTransaction&&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool targetAvailable() const noexcept;
    [[nodiscard]] LayerId layerId() const noexcept { return layerId_; }
    [[nodiscard]] SurfaceId surfaceId() const noexcept { return surfaceId_; }
    [[nodiscard]] std::uint32_t journalTileSize() const noexcept { return tileSize_; }
    [[nodiscard]] std::size_t capturedTileCount() const noexcept { return journal_.size(); }
    [[nodiscard]] std::size_t capturedPixelBytes() const noexcept;
    [[nodiscard]] bool hasSelection() const noexcept { return selectionMask_ != nullptr; }
    // Our pinned mask is immutable. A borrowed/custom mask has no concurrent
    // sampling contract and must remain on the caller thread.
    [[nodiscard]] bool supportsConcurrentAdmission() const noexcept
    { return !selectionMask_ || selectionMask_ == pinnedSelection_.get(); }
    [[nodiscard]] bool cropAllows(int x,int y) const noexcept { return ignoreCrop_ || cropAllowsTexel(crop_,x,y); }
    [[nodiscard]] RectI clipToCrop(RectI bounds) const noexcept;
    [[nodiscard]] bool selectionIsEmpty() const noexcept
    { return pinnedSelection_ && selectionMask_ == pinnedSelection_.get() && pinnedSelection_->bounds().empty(); }
    [[nodiscard]] std::uint8_t selectionCoverageAtDocumentPoint(Vec2d point) const noexcept
    {
        if (!selectionMask_) return 255;
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
            || point.x >= double(std::numeric_limits<std::int32_t>::max())
            || point.y >= double(std::numeric_limits<std::int32_t>::max())) return 0;
        return selectionMask_->coverageAtDocumentPixel(std::int32_t(std::floor(point.x)), std::int32_t(std::floor(point.y)));
    }

    // Capture is lazy and idempotent. It is public so a rasterizer can read the
    // stroke-start pixels before constructing a composited patch.
    bool capture(RectI localRegion);
    void copyOriginalRgba8(RectI localRegion, std::span<std::byte> destination,
        std::size_t destinationStride) const;

    [[nodiscard]] DirtySet writeRgba8(RectI localRegion,
        std::span<const std::byte> source, std::size_t sourceStride);
    [[nodiscard]] DirtySet writeRgba8Batch(std::span<const RasterPatch> patches);
    // Producer already combined the pinned selection with its floating-point
    // edit, then encoded once. Retain all admission checks and journaling;
    // never apply fractional selection coverage to these candidates again.
    [[nodiscard]] DirtySet writeSelectionResolvedRgba8Batch(std::span<const RasterPatch> patches);

    [[nodiscard]] RasterEditCommitResult commit(History& history);
    void cancel() noexcept;

private:
    struct JournalTile {
        RectI region;
        std::vector<std::byte> before;
    };

    using TileKey = std::pair<std::int32_t, std::int32_t>;

    [[nodiscard]] RectI surfaceBounds() const noexcept;
    [[nodiscard]] RasterSurface* resolveCurrentTarget() const noexcept;
    [[nodiscard]] std::vector<RasterEditCommand::TileSnapshot> finalizeTiles();
    [[nodiscard]] std::vector<RectI> changedRegions(
        const JournalTile& tile, std::span<const std::byte> current) const;
    [[nodiscard]] DirtySet writeRgba8BatchImpl(std::span<const RasterPatch> patches, bool selectionResolved);
    [[nodiscard]] DirtySet writeMasked(std::span<const RasterPatch> patches, bool selectionResolved);
    void restoreOriginalPixels();

    Document* document_ {nullptr};
    LayerId layerId_ {0};
    SurfaceId surfaceId_ {0};
    Extent2u surfaceExtent_;
    Extent2u canvasExtent_;
    AffineTransform localToDocument_;
    std::optional<LayerCrop> crop_;
    std::optional<LayerCrop> layerCrop_;
    std::shared_ptr<RasterSurface> liveSurface_;
    std::uint32_t tileSize_ {64};
    SelectionState pinnedSelection_;
    const SelectionMaskInput* selectionMask_ {nullptr};
    Revision selectionRevision_ {0};
    std::array<char, 48> historyLabel_ {};
    std::uint8_t historyLabelLength_ {0};
    std::map<TileKey, JournalTile> journal_;
    bool active_ {false};
    bool ignoreCrop_ {false};
    bool coverageValues_ {false};
};

} // namespace imageeditor::core
