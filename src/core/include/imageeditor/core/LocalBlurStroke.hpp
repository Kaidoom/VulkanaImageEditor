#pragma once

#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/BlurSettings.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/SpatialFilters.hpp"

namespace imageeditor::core {

struct LocalBlurStats {
    std::size_t snapshotBytes {0};
    std::size_t cachedBytes {0};
    std::size_t peakWorkingBytes {0};
    std::size_t filteredTiles {0};
    std::size_t cacheHits {0};
};

// Each stroke pins intrinsic source bytes. The shared brush path accumulates
// influence over that reference; it never convolves its previously written
// previews. Filter samples are cached on a document-pixel grid so rotated,
// stretched and flipped target layers retain the same document-space radius.
class LocalBlurStroke final {
public:
    static constexpr std::size_t snapshotLimit = 256U * 1024U * 1024U;
    static constexpr std::size_t cacheLimit = 32U * 1024U * 1024U;
    LocalBlurStroke(Document&, LayerId, BrushSettings, BlurSettings,
        const IBrushAssetResolver* = nullptr);
    bool begin(const NormalizedPointerSample&, const std::function<bool()>& cancelled = {});
    bool append(const NormalizedPointerSample&, const std::function<bool()>& cancelled = {});
    RasterEditCommitResult end(const NormalizedPointerSample&, History&,
        const std::function<bool()>& cancelled = {});
    void cancel() noexcept;
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] const BrushStrokeStats& stats() const noexcept { return brush_.stats(); }
    [[nodiscard]] const LocalBlurStats& filterStats() const noexcept { return stats_; }
    [[nodiscard]] const BasicPixelBrushStroke& brush() const noexcept { return brush_; }
private:
    struct CachedTile { std::vector<float> pixels; std::uint64_t used {0}; };
    bool targetMatches() const noexcept;
    bool interrupted() const;
    void checkpoint() noexcept;
    bool capture();
    PremultipliedColor filteredSample(Vec2d);
    PremultipliedColor filteredPixel(std::int32_t, std::int32_t);
    Document& document_;
    LayerId target_;
    BlurSettings settings_;
    AffineTransform localToDocument_;
    SurfaceId surface_ {0};
    Revision revision_ {0}, surfaceRevision_ {0};
    std::optional<PreparedLayerSampler> source_;
    SpatialKernel kernel_;
    std::map<std::pair<std::int32_t, std::int32_t>, CachedTile> tiles_;
    std::uint64_t clock_ {0};
    std::function<bool()> cancelled_;
    BasicPixelBrushStroke brush_;
    LocalBlurStats stats_;
    std::string diagnostic_;
};

}
