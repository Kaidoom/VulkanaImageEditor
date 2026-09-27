#pragma once
#include "imageeditor/core/RasterSurface.hpp"
#include <map>
#include <memory>

namespace imageeditor::core
{
// Ordinary committed pixels, with COW regional storage. There is no transform,
// selection, or floating-fragment evaluator in this surface. Untouched bytes
// share a pinned base; changed tiles are immutable between history states.
class RegionalRasterSurface final : public RasterSurface
{
  public:
    static constexpr int tileSide = 64;
    using Tile = std::array<std::byte, tileSide * tileSide * 4>;
    using Key = std::pair<int, int>;
    struct State {
        RectI bounds;
        std::map<Key, std::shared_ptr<const Tile>> tiles;
    };
    RegionalRasterSurface(std::shared_ptr<const RasterSurface> base, Vec2d origin);
    [[nodiscard]] SurfaceId id() const noexcept override { return id_; }
    [[nodiscard]] Extent2u extent() const noexcept override
    {
        return {std::uint32_t(state_.bounds.width), std::uint32_t(state_.bounds.height)};
    }
    [[nodiscard]] Revision revision() const noexcept override { return revision_; }
    [[nodiscard]] DirtySet dirtySince(Revision revision) const override;
    void copyRgba8(RectI, std::span<std::byte>, std::size_t) const override;
    [[nodiscard]] DirtySet replaceRgba8Batch(std::span<const RasterPatch>) override;
    [[nodiscard]] DirtySet swapRgba8Batch(std::span<MutableRasterPatch>) override;
    [[nodiscard]] const State &state() const noexcept { return state_; }
    [[nodiscard]] const std::shared_ptr<const RasterSurface> &baseSurface() const noexcept { return base_; }
    [[nodiscard]] RectI baseBounds() const noexcept { return baseBounds_; }
    [[nodiscard]] Vec2d origin() const noexcept { return {double(state_.bounds.x), double(state_.bounds.y)}; }
    void restore(State state);
    static void validateBounds(RectI);
    [[nodiscard]] std::size_t retainedBytes() const noexcept
    {
        return state_.tiles.size() * (sizeof(Tile) + sizeof(Key) + 64);
    }

  private:
    void copyLocal(RectI, std::span<std::byte>, std::size_t) const;
    void publish(State, DirtySet);
    std::shared_ptr<const RasterSurface> base_;
    RectI baseBounds_;
    State state_;
    SurfaceId id_{makeSurfaceId()};
    Revision revision_{1};
    std::deque<DirtySet> dirty_;
};
} // namespace imageeditor::core
