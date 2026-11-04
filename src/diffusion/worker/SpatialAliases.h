#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace vulkana::diffusion {
struct SpatialAlias { std::string value; bool legacy{}; };
using SpatialAliases = std::map<std::string, SpatialAlias>;
inline void addSpatialAlias(SpatialAliases& aliases, const std::string& name, const std::string& value, bool legacy = false)
{
    auto [it, added] = aliases.emplace(name, SpatialAlias{value, legacy});
    if (!added && it->second.value != value) it->second.value.clear(); // ambiguous
}
inline std::string resolveSpatialAliases(const std::string& prompt, const SpatialAliases& aliases)
{
    const auto word = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '/' || c >= 128; };
    const auto imageName = [](const std::string& name) {
        if (!name.starts_with("Image") || name.size() < 6 || !std::isdigit(static_cast<unsigned char>(name[5]))) return false;
        return true;
    };
    const auto resolve = [&](const std::string& name) {
        const auto it = aliases.find(name);
        if (it == aliases.end())
            throw std::invalid_argument("Unknown image or region <" + name + ">. Use <ImageN/RegionName> with the exact input and region name.");
        if (it->second.value.empty())
            throw std::invalid_argument("Ambiguous region <" + name + ">. Use <ImageN/RegionName> or <Output/RegionName>.");
        return it->second.value;
    };
    std::vector<std::string> legacy;
    for (const auto& [name, value] : aliases) if (value.legacy) legacy.push_back(name);
    std::sort(legacy.begin(), legacy.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    std::string result;
    for (std::size_t pos = 0; pos < prompt.size();) {
        if (prompt[pos] == '<') {
            const auto end = prompt.find('>', pos + 1);
            if (end != std::string::npos) {
                const auto name = prompt.substr(pos + 1, end - pos - 1);
                if (aliases.contains(name) || name == "Scene" || imageName(name) || name.starts_with("Region") || name.starts_with("Output/"))
                    result += resolve(name);
                else result += prompt.substr(pos, end - pos + 1); // native tags and literal markup
                pos = end + 1;
                continue;
            }
        }
        bool matched = false;
        if (pos == 0 || !word(prompt[pos - 1])) {
            for (const auto& name : legacy) {
                if (prompt.compare(pos, name.size(), name) == 0 &&
                    (pos + name.size() == prompt.size() || !word(prompt[pos + name.size()]))) {
                    result += resolve(name); pos += name.size(); matched = true; break;
                }
            }
            if (!matched && prompt.compare(pos, 5, "Image") == 0) {
                auto end = pos + 5;
                while (end < prompt.size() && std::isdigit(static_cast<unsigned char>(prompt[end]))) ++end;
                if (end > pos + 5 && end < prompt.size() && prompt[end] == '/')
                    throw std::invalid_argument("Unknown input region in prompt. Use <ImageN/RegionName> with the exact region name.");
            }
        }
        if (!matched) result += prompt[pos++];
    }
    return result;
}
}
