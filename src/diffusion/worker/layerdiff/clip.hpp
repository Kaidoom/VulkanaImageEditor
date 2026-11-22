#pragma once

#include "backend.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace layerdiff {

struct Conditioning {
    Tensor context; // [parts, 77, 2048] penultimate CLIP-L and CLIP-G states
    Tensor pooled;  // [parts, 1280] projected CLIP-G EOS states
};

Conditioning encodeParts(Backend& backend, const std::filesystem::path& directory,
                         const std::vector<std::string>& tags);

} // namespace layerdiff
