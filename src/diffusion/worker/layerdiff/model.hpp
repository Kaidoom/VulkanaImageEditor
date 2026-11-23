#pragma once

#include "backend.hpp"
#include "image.hpp"
#include <filesystem>
#include <functional>
#include <memory>

namespace layerdiff {

inline constexpr auto profileId = "see-through-layerdiff-v3";
using Progress = std::function<void(const char *stage, const char *detail, int completed, int total, int pass)>;
void validateBundle(const std::filesystem::path &root);

// The GPU device, tag encodings and weights are kept for repeated runs.
class Model {
  public:
    Model(const std::filesystem::path &root, ggml_backend_dev_t device, std::function<void()> checkpoint);
    ~Model();
    bool stagesDenoiserWeights() const;
    void generate(const Image &source, int resolution, int steps, uint64_t seed, bool splitParts,
                  const Progress &report, const std::function<void(Part &&)> &writePart);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace layerdiff
