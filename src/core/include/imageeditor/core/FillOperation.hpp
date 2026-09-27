#pragma once
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/RegionFinder.hpp"
#include <array>

namespace imageeditor::core {
enum class FillMode { SelectionOrLayer, Contiguous };
struct FillOptions {
    FillMode mode { FillMode::SelectionOrLayer };
    Rgba8 color { };
    double opacity { 1.0 };
    std::uint8_t tolerance { 16 };
    Vec2d seed { };
    // Destination-out; without a mask clear the entire raster source, including
    // cropped/off-canvas pixels. An active empty mask remains a no-op. Color and
    // bucket opacity are irrelevant. Coverage is applied once by the transaction.
    bool eraseSelection { false };
};
enum class FillState { Discovering, Applying, Ready, Finished, Cancelled, Failed };
struct FillStats {
    std::uint64_t evaluatedPixels { 0 }, candidatePixels { 0 }, writeBatches { 0 };
    std::size_t discoveryBytes { 0 }, journalBytes { 0 };
};
// Cooperative job, exclusively owning the edit until commit/cancel. Each step
// runs on the document owner thread; UI yields between steps (no pixel races).
class FillOperation final {
public:
    FillOperation(Document& document, LayerId layer, FillOptions options);
    ~FillOperation();
    FillState step(std::size_t pixelBudget = 65536);
    RasterEditCommitResult commit(History& history);
    void cancel() noexcept;
    [[nodiscard]] FillState state() const noexcept { return state_; }
    [[nodiscard]] const FillStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }
    [[nodiscard]] std::string_view label() const noexcept
    { return options_.eraseSelection ? (selection_ ? "Erase selection" : "Clear layer") : "Fill"; }

private:
    bool eligible(int x, int y) const;
    bool targetValid() const;
    Rgba8 source(int x, int y);
    Rgba8 composite(Rgba8 before);
    Document& document_;
    LayerId layer_;
    FillOptions options_;
    Extent2u canvas_, extent_;
    RectI workBounds_;
    AffineTransform mapping_;
    SelectionState selection_;
    std::shared_ptr<RasterSurface> surface_;
    Revision expectedRevision_ { 0 };
    std::unique_ptr<RegionFinder> region_;
    std::unique_ptr<RasterEditTransaction> transaction_;
    struct Row {
        int y { -1 };
        std::vector<std::byte> bytes;
    };
    std::array<Row, 8> rows_;
    struct CompositeTable {
        std::array<std::array<std::uint8_t, 256>, 3> rgb;
        std::uint8_t alpha;
    };
    std::array<std::unique_ptr<CompositeTable>, 256> tables_;
    std::uint64_t nextTile_ { 0 };
    FillState state_ { FillState::Failed };
    FillStats stats_;
    std::string error_;
};
}
