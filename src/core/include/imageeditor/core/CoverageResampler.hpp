#pragma once
#include "imageeditor/core/SelectionMask.hpp"
namespace imageeditor::core
{
// Immutable R8 coverage reconstructed as float. Minification averages coverage,
// never thresholds it, and interpolates between levels without R8 requantizing.
class CoverageResampler final
{
  public:
    explicit CoverageResampler(const SelectionMask &mask) : mask_(mask), bounds_(mask.bounds()) {}
    float sample(Vec2d point, double footprint = 1);
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

  private:
    struct Level {
        int width, height;
        std::vector<float> values;
    };
    float texel(int x, int y, int level) const;
    void prepare();
    const SelectionMask &mask_;
    RectI bounds_;
    std::vector<Level> levels_;
};
} // namespace imageeditor::core
