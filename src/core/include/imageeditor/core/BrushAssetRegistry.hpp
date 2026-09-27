#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace imageeditor::core {

// Raster brush assets use one convention after import: 0 means no coverage
// and 255 means full coverage. Source-channel and polarity metadata belong to
// the registry so filenames never acquire implicit rendering semantics.
enum class BrushAssetType : std::uint8_t {
    Tip,
    Grain,
};

enum class BrushCoverageChannel : std::uint8_t {
    Luminance,
    Alpha,
    LuminanceTimesAlpha,
};

struct BrushAssetRecord {
    std::string id;
    std::string displayName;
    BrushAssetType type {BrushAssetType::Tip};
    std::string relativePackagedPath;
    BrushCoverageChannel coverageChannel {BrushCoverageChannel::Luminance};
    bool invert {false};
    std::optional<bool> seamless;
    std::optional<double> defaultScalePixels;
    std::optional<double> defaultRotationDegrees;
    std::uint64_t revision {1};
    std::string sha256;

    friend bool operator==(
        const BrushAssetRecord&, const BrushAssetRecord&) = default;
};

class BrushAssetRegistry final {
public:
    static constexpr std::uint32_t SupportedVersion = 1;

    BrushAssetRegistry(std::uint32_t version,
        std::vector<BrushAssetRecord> assets,
        std::span<const std::string_view> reservedAssetIds = {});

    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    [[nodiscard]] std::span<const BrushAssetRecord> assets() const noexcept
    {
        return assets_;
    }
    [[nodiscard]] const BrushAssetRecord* find(
        std::string_view id) const noexcept;
    [[nodiscard]] bool contains(
        std::string_view id, BrushAssetType type) const noexcept;

private:
    std::uint32_t version_ {SupportedVersion};
    std::vector<BrushAssetRecord> assets_;
};

} // namespace imageeditor::core
