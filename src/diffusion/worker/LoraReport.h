#pragma once

#include <algorithm>
#include <map>
#include <regex>
#include <string>

namespace vulkana::diffusion {
// The pinned engine reports actual tensor matches after runtime application.
// A successfully generated image alone does not establish LoRA compatibility.
struct LoraReport {
    struct Counts { unsigned long applied = 0, incompatible = 0; };
    std::map<std::string, Counts> files;

    void observe(const std::string& message)
    {
        if (message.find("lora_file_path = ") == std::string::npos) return;
        static const std::regex report(R"(\(([0-9]+) / [0-9]+\) (LoRA tensors have been applied|incompatible LoRA tensors have been skipped), lora_file_path = (.+))");
        std::smatch match;
        if (!std::regex_search(message, match, report)) return;
        auto path = match[3].str();
        const auto location = path.rfind(" --- ");
        if (location != std::string::npos) path.resize(location);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        auto& counts = files[path];
        auto& count = match[2] == "LoRA tensors have been applied" ? counts.applied : counts.incompatible;
        count = std::max(count, std::stoul(match[1].str()));
    }
    bool compatible(const std::string& path) const
    {
        const auto entry = files.find(path);
        return entry != files.end() && entry->second.applied > 0 && entry->second.incompatible == 0;
    }
};
}
