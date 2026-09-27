#include "imageeditor/core/BrushAssetRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace imageeditor::core {
namespace {

bool validRelativePath(std::string_view path) noexcept
{
    if (path.empty() || path.front() == '/' || path.front() == '\\') {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find_first_of("/\\", start);
        const auto component = path.substr(start,
            end == std::string_view::npos ? path.size() - start : end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return true;
}

bool validSha256(std::string_view digest) noexcept
{
    return digest.size() == 64U
        && std::all_of(digest.begin(), digest.end(), [](char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

bool validAssetType(BrushAssetType type) noexcept
{
    return type == BrushAssetType::Tip || type == BrushAssetType::Grain;
}

bool validCoverageChannel(BrushCoverageChannel channel) noexcept
{
    return channel == BrushCoverageChannel::Luminance
        || channel == BrushCoverageChannel::Alpha
        || channel == BrushCoverageChannel::LuminanceTimesAlpha;
}

bool validAssetId(std::string_view id) noexcept
{
    return !id.empty() && id.size() <= 192U
        && std::all_of(id.begin(), id.end(), [](unsigned char character) {
               return std::isalnum(character) || character == '.'
                   || character == '_' || character == '-';
           });
}

bool validDisplayName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 240U) {
        return false;
    }
    bool hasVisibleCharacter = false;
    for (const auto raw : name) {
        const auto character = static_cast<unsigned char>(raw);
        if (character < 0x20U || character == 0x7fU) {
            return false;
        }
        hasVisibleCharacter = hasVisibleCharacter
            || std::isspace(character) == 0;
    }
    return hasVisibleCharacter;
}

} // namespace

BrushAssetRegistry::BrushAssetRegistry(std::uint32_t version,
    std::vector<BrushAssetRecord> assets,
    std::span<const std::string_view> reservedAssetIds)
    : version_(version)
    , assets_(std::move(assets))
{
    if (version_ != SupportedVersion) {
        throw std::invalid_argument("Unsupported brush asset registry version");
    }
    std::unordered_set<std::string> ids;
    ids.reserve(assets_.size() + reservedAssetIds.size());
    for (const auto id : reservedAssetIds) {
        if (!validAssetId(id) || !ids.emplace(id).second) {
            throw std::invalid_argument("Invalid or duplicate reserved brush asset ID");
        }
    }
    for (const auto& asset : assets_) {
        const auto finiteOptional = [](const std::optional<double>& value) {
            return !value || std::isfinite(*value);
        };
        if (!validAssetId(asset.id) || !validDisplayName(asset.displayName)
            || !validAssetType(asset.type)
            || !validCoverageChannel(asset.coverageChannel)
            || !validRelativePath(asset.relativePackagedPath)
            || asset.relativePackagedPath.size() > 512U
            || asset.revision == 0 || !validSha256(asset.sha256)
            || !finiteOptional(asset.defaultScalePixels)
            || !finiteOptional(asset.defaultRotationDegrees)
            || (asset.defaultScalePixels && *asset.defaultScalePixels <= 0.0)
            || (asset.type == BrushAssetType::Tip
                && asset.seamless.value_or(false))
            || (asset.type == BrushAssetType::Grain
                && !asset.seamless.value_or(false))) {
            throw std::invalid_argument("Invalid brush asset registry entry");
        }
        if (!ids.insert(asset.id).second) {
            throw std::invalid_argument("Duplicate brush asset ID");
        }
    }
}

const BrushAssetRecord* BrushAssetRegistry::find(
    std::string_view id) const noexcept
{
    const auto found = std::find_if(assets_.begin(), assets_.end(),
        [id](const BrushAssetRecord& asset) { return asset.id == id; });
    return found == assets_.end() ? nullptr : &*found;
}

bool BrushAssetRegistry::contains(
    std::string_view id, BrushAssetType type) const noexcept
{
    const auto* asset = find(id);
    return asset && asset->type == type;
}

} // namespace imageeditor::core
