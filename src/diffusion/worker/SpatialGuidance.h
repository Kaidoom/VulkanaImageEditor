#pragma once
#include <algorithm>
#include <cmath>
#include <json.hpp>
#include <stdexcept>
#include <string>

namespace vulkana::diffusion {
// Spatial schema 1 is model-independent. It describes semantic placement, never
// a pixel-preservation mask. FLUX.3 receives its documented caption box table.
inline nlohmann::json spatialRegions(const nlohmann::json& spatial)
{
    using J = nlohmann::json;
    const auto invalid = [](const std::string& message) { throw std::invalid_argument(message); };
    if (!spatial.is_object())
        invalid("spatial must be an object");
    for (const auto& item : spatial.items())
        if (item.key() != "version" && item.key() != "regions" && item.key() != "annotationImage")
            invalid("Unknown spatial field: " + item.key());
    if (!spatial.contains("version") || !spatial["version"].is_number_integer() || spatial["version"] != 1)
        invalid("spatial.version must be 1");
    if (!spatial.contains("regions") || !spatial["regions"].is_array() || spatial["regions"].size() > 128)
        invalid("spatial.regions must be an array of at most 128 regions");
    for (const auto& region : spatial["regions"]) {
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
    return spatial["regions"];
}
inline std::string flux3RegionPrompt(const std::string& prompt, const nlohmann::json& regions)
{
    auto rows = nlohmann::json::array();
    int i = 0;
    const auto grid = [](double v) { return std::clamp(static_cast<int>(std::lround(v * 1000.0)), 0, 1000); };
    for (const auto& r : regions) {
        const double x = r["x"], y = r["y"], w = r["width"], h = r["height"];
        const int top = grid(y), left = grid(x), bottom = grid(y + h), right = grid(x + w);
        if (bottom <= top || right <= left)
            throw std::invalid_argument("Region is too small for FLUX.3's 0–1000 coordinate grid");
        rows.push_back({ { "id", "region_" + std::to_string(++i) }, { "bbox", { top, left, bottom, right } },
            { "desc", r["label"] } });
    }
    return rows.empty() ? prompt
                        : prompt + " " + rows.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}
}
