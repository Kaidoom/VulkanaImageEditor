#pragma once
#include "SpatialAliases.h"
#include "SpatialGuidance.h"

namespace vulkana::diffusion {
struct FluxSpatialCaption {
    std::string prompt;
    nlohmann::ordered_json rows = nlohmann::ordered_json::array();
    nlohmann::json bindings = nlohmann::json::array();
};
// FLUX consumes this table as part of its trained caption format, not through
// a separate mask or an additional image. Coordinate order matches ComfyUI/BFL.
inline FluxSpatialCaption fluxSpatialCaption(const std::string& prompt,
    const nlohmann::json& output, const nlohmann::json& inputs, std::size_t imageCount,
    const nlohmann::json& crops = nlohmann::json::array(), int outputScene = 0, bool extended = false)
{
    using J = nlohmann::json;
    using O = nlohmann::ordered_json;
    FluxSpatialCaption result;
    SpatialAliases aliases;
    if (outputScene < 0 || std::size_t(outputScene) > imageCount)
        throw std::invalid_argument("Output scene must name an existing input image.");
    if (outputScene > 0)
        addSpatialAlias(aliases, "Scene", "<ref_image_" + std::to_string(outputScene - 1) + ">");
    std::map<std::string, const J*> targets;
    std::set<std::string> regionIds, usedTargets;
    const auto checkId = [&](const J& r) {
        const auto id = r.value("id", "");
        if (!id.empty() && !regionIds.insert(id).second)
            throw std::invalid_argument("Region IDs must be unique across inputs and output locations.");
    };
    for (const auto& r : output) {
        checkId(r);
        if (!r.value("targetId", "").empty()) throw std::invalid_argument("An output location cannot have a destination.");
        if (r.value("targetOnly", false)) {
            const auto id = r.value("id", "");
            if (id.empty()) throw std::invalid_argument("Output target locations require a stable region ID.");
            targets.emplace(id, &r);
        }
    }
    for (const auto& group : inputs) for (const auto& r : group["regions"]) {
        checkId(r);
        if (r.value("targetOnly", false)) throw std::invalid_argument("Target-only locations belong to the output frame.");
        const auto id = r.value("targetId", "");
        if (!id.empty()) {
            if (imageCount < 2 || outputScene == 0)
                throw std::invalid_argument("Linked transfers require multiple inputs and a selected output scene.");
            if (!targets.contains(id))
                throw std::invalid_argument("A source region's destination is missing. Choose another target or unlink that region.");
            usedTargets.insert(id);
        }
    }
    for (std::size_t i = 0; i < imageCount; ++i)
        addSpatialAlias(aliases, "Image" + std::to_string(i + 1), "<ref_image_" + std::to_string(i) + ">", true);
    // Fully native captions (including a supplied JSON table) pass through when
    // there are no editor regions. Never append a second box table.
    for (auto pos = prompt.rfind('['); pos != std::string::npos; pos = pos ? prompt.rfind('[', pos - 1) : std::string::npos) {
        const auto parsed = J::parse(prompt.substr(pos), nullptr, false);
        if (parsed.is_array() && !parsed.empty() && std::all_of(parsed.begin(), parsed.end(), [](const J& row) {
                return row.is_object() && row.contains("id") && (row.contains("bbox") || row.contains("src_bbox") || row.contains("tgt_bbox")); })) {
            if (!output.empty() || !inputs.empty())
                throw std::invalid_argument("The prompt already contains a bounding-box table. Remove that table or clear the drawn regions before generating.");
            result.prompt = prompt;
            return result;
        }
    }
    const auto box = [](const J& r) {
        // Python's round() in the reference implementation uses ties-to-even.
        const auto grid = [](double v) {
            const double scaled = std::clamp(v, 0.0, 1.0) * 1000;
            int value = int(std::floor(scaled));
            const double fraction = scaled - value;
            if (fraction > .5 || (fraction == .5 && (value & 1))) ++value;
            return value;
        };
        const int top = grid(r["y"]), left = grid(r["x"]);
        const int bottom = grid(r["y"].get<double>() + r["height"].get<double>());
        const int right = grid(r["x"].get<double>() + r["width"].get<double>());
        if (bottom <= top || right <= left) throw std::invalid_argument("Region is too small for FLUX.3's 0–1000 coordinate grid.");
        return O::array({top, left, bottom, right});
    };
    int object = 0;
    const bool hasOutputObjects = std::any_of(output.begin(), output.end(), [](const J& r) { return !r.value("targetOnly", false); });
    // Input ordering, rather than the order of groups in the request, determines
    // object IDs. The qualified UI names remain unambiguous across images.
    for (std::size_t i = 1; i <= imageCount; ++i) for (const auto& group : inputs) {
        if (group["image"] != i) continue;
        for (const auto& r : group["regions"]) {
            const auto id = "obj_" + std::to_string(++object);
            const auto name = "Image" + std::to_string(i) + "/" + r["label"].get<std::string>();
            addSpatialAlias(aliases, name, "<" + id + ">", true);
            if (imageCount == 1) addSpatialAlias(aliases, r["label"], "<" + id + ">");
            else addSpatialAlias(aliases, r["label"], ""); // require a qualified name
            const auto source = "ref_image_" + std::to_string(i - 1);
            const auto targetId = r.value("targetId", "");
            const J* target = targetId.empty() ? nullptr : targets.at(targetId);
            const auto description = extended ? r.value("description", "") : std::string{};
            if (imageCount == 1 && !hasOutputObjects && !target)
                result.rows.push_back(O{{"id", id}, {"bbox", box(r)}, {"desc", description}});
            else
                result.rows.push_back(O{{"id", id}, {"from", source}, {"src_bbox", box(r)}, {"tgt_bbox", target ? box(*target) : O(nullptr)}, {"desc", description}});
            result.bindings.push_back({{"name", name}, {"id", id}, {"sourceImage", source}});
            if (target) {
                const auto targetName = "Output/" + (*target)["label"].get<std::string>();
                addSpatialAlias(aliases, targetName, "the target position of <" + id + ">");
                result.bindings.back()["targetName"] = targetName;
            }
        }
    }
    for (const auto& r : output) {
        if (r.value("targetOnly", false)) {
            if (!usedTargets.contains(r.value("id", "")) && prompt.find("<Output/" + r["label"].get<std::string>() + ">") != std::string::npos)
                throw std::invalid_argument("That output target is not linked to a source region. Choose it as a source region's Destination first.");
            continue;
        }
        const auto id = "obj_" + std::to_string(++object);
        const auto label = r["label"].get<std::string>();
        addSpatialAlias(aliases, "Output/" + label, "<" + id + ">");
        if (imageCount == 0) addSpatialAlias(aliases, label, "<" + id + ">");
        const bool defaultLabel = label.starts_with("Region") && label.size() > 6 &&
            std::all_of(label.begin() + 6, label.end(), [](unsigned char c) { return std::isdigit(c); });
        result.rows.push_back(O{{"id", id}, {"bbox", box(r)}, {"desc", extended ? r.value("description", "") : (defaultLabel ? "" : label)}});
        result.bindings.push_back({{"name", "Output/" + label}, {"id", id}, {"sourceImage", "target"}});
    }
    result.prompt = resolveSpatialAliases(prompt, aliases);
    for (const auto& crop : crops) {
        const auto binding = std::find_if(result.bindings.begin(), result.bindings.end(), [&](const J& b) { return b["name"] == crop["name"]; });
        if (binding == result.bindings.end()) continue;
        const auto image = crop["detailImage"].get<std::string>();
        const int index = std::stoi(image.substr(5)) - 1;
        result.prompt += " A detail view of <" + (*binding)["id"].get<std::string>() + "> is provided in <ref_image_" + std::to_string(index) + ">.";
    }
    if (!result.rows.empty()) result.prompt += " " + result.rows.dump(-1, ' ', false, J::error_handler_t::strict);
    return result;
}
}
