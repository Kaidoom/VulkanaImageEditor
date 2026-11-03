#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <json.hpp>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>
#include "SpatialAliases.h"

namespace vulkana::diffusion {
inline void validateRegionArray(const nlohmann::json& regions)
{
    const auto invalid = [](const std::string& message) { throw std::invalid_argument(message); };
    if (!regions.is_array() || regions.size() > 128)
        invalid("spatial.regions must be an array of at most 128 regions");
    for (const auto& region : regions) {
        if (!region.is_object())
            invalid("Each region must be an object");
        for (const auto& item : region.items())
            if (item.key() != "x" && item.key() != "y" && item.key() != "width" && item.key() != "height"
                && item.key() != "label")
                invalid("Unknown region field: " + item.key());
        for (const char* key : { "x", "y", "width", "height" }) {
            if (!region.contains(key) || !region[key].is_number())
                invalid(std::string("Region ") + key + " must be a number");
            const double v = region[key].get<double>();
            if (!std::isfinite(v) || v < 0 || v > 1)
                invalid("Region coordinates must be finite normalized values from 0 to 1");
        }
        const double x = region["x"], y = region["y"], w = region["width"], h = region["height"];
        if (w <= 0 || h <= 0 || x + w > 1.000000001 || y + h > 1.000000001)
            invalid("Regions must have positive size and lie within the image");
        if (!region.contains("label") || !region["label"].is_string())
            invalid("Every region needs a text label");
        const auto label = region["label"].get<std::string>();
        if (label.empty() || label.size() > 2048 || label.find('\0') != std::string::npos)
            invalid("Region label must contain 1–2048 UTF-8 bytes without NUL");
    }
}
// Version 1 retains output placement. Version 2 also identifies regions in
// individual input images; neither kind replaces the pixel-preservation mask.
inline nlohmann::json spatialRegions(const nlohmann::json& spatial)
{
    if (!spatial.is_object())
        throw std::invalid_argument("spatial must be an object");
    if (!spatial.contains("version") || !spatial["version"].is_number_integer()
        || (spatial["version"] != 1 && spatial["version"] != 2))
        throw std::invalid_argument("spatial.version must be 1 or 2");
    for (const auto& item : spatial.items())
        if (item.key() != "version" && item.key() != "regions" && item.key() != "annotationImage"
            && !(spatial["version"] == 2 && (item.key() == "inputRegions" || item.key() == "cropGuidance")))
            throw std::invalid_argument("Unknown spatial field: " + item.key());
    if (spatial.contains("cropGuidance") && !spatial["cropGuidance"].is_boolean())
        throw std::invalid_argument("spatial.cropGuidance must be boolean");
    if (!spatial.contains("regions"))
        throw std::invalid_argument("spatial.regions is required (use [] for no output regions)");
    validateRegionArray(spatial["regions"]);
    return spatial["regions"];
}
inline nlohmann::json inputRegionGroups(const nlohmann::json& spatial, std::size_t imageCount)
{
    using J = nlohmann::json;
    if (!spatial.contains("inputRegions"))
        return J::array();
    const auto& groups = spatial["inputRegions"];
    if (spatial.value("version", 0) != 2 || !groups.is_array() || groups.size() > imageCount)
        throw std::invalid_argument("spatial.inputRegions requires version 2 and at most one group per input image");
    std::set<int> seen;
    std::size_t total = spatial["regions"].size();
    for (const auto& group : groups) {
        if (!group.is_object())
            throw std::invalid_argument("Each input region group must be an object");
        for (const auto& item : group.items())
            if (item.key() != "image" && item.key() != "regions")
                throw std::invalid_argument("Unknown input region field: " + item.key());
        if (!group.contains("image") || !group["image"].is_number_integer()
            || group["image"] < 1 || group["image"] > imageCount)
            throw std::invalid_argument("Input region image must name an existing referenceImages entry (starting at 1)");
        if (!seen.insert(group["image"].get<int>()).second)
            throw std::invalid_argument("Input region groups must not repeat an image");
        if (!group.contains("regions"))
            throw std::invalid_argument("Input region group needs regions");
        validateRegionArray(group["regions"]);
        std::set<std::string> labels;
        for (const auto& r : group["regions"])
            if (!labels.insert(r["label"].get<std::string>()).second)
                throw std::invalid_argument("Region names must be unique within each input image");
        total += group["regions"].size();
        if (total > 128)
            throw std::invalid_argument("Combined input and output regions must not exceed 128");
    }
    return groups;
}
struct RegionCrop { int x, y, width, height; };
inline RegionCrop inputRegionCrop(const nlohmann::json& r, int width, int height)
{
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("Cannot crop an empty reference image");
    const int x = std::clamp(int(std::floor(r["x"].get<double>() * width)), 0, width - 1);
    const int y = std::clamp(int(std::floor(r["y"].get<double>() * height)), 0, height - 1);
    const int right = std::clamp(int(std::ceil((r["x"].get<double>() + r["width"].get<double>()) * width)), x + 1, width);
    const int bottom = std::clamp(int(std::ceil((r["y"].get<double>() + r["height"].get<double>()) * height)), y + 1, height);
    return {x, y, right - x, bottom - y};
}
// Resolve editor aliases before inference. Models receive ordinary reference
// image numbers, rather than having to interpret an application-specific table.
inline std::string inputRegionPrompt(const std::string& prompt, const nlohmann::json& references, std::size_t imageCount = 0)
{
    SpatialAliases aliases;
    for (std::size_t i = 1; i <= imageCount; ++i)
        addSpatialAlias(aliases, "Image" + std::to_string(i), "Image" + std::to_string(i));
    for (const auto& reference : references) {
        const auto name = reference["name"].get<std::string>();
        const auto detail = reference["detailImage"].get<std::string>();
        addSpatialAlias(aliases, name, detail, true);
        const auto label = name.substr(name.find('/') + 1);
        if (imageCount == 1) addSpatialAlias(aliases, label, detail);
        else if (imageCount > 1) addSpatialAlias(aliases, label, "");
    }
    auto result = resolveSpatialAliases(prompt, aliases);
    const auto caption = result;
    for (const auto& reference : references) {
        const auto detail = reference["detailImage"].get<std::string>();
        if (caption.find(detail) == std::string::npos) continue;
        result += " " + detail + " is the selected crop of " + reference["sourceImage"].get<std::string>()
            + ". Use only the content shown in " + detail + " for that region.";
    }
    return result;
}
}
