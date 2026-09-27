#pragma once

#include "imageeditor/core/Geometry.hpp"
#include "imageeditor/core/ViewportState.hpp"

#include <array>

namespace imageeditor::render {

struct FramebufferScissor {
    std::int32_t x {0};
    std::int32_t y {0};
    std::uint32_t width {0};
    std::uint32_t height {0};

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width == 0 || height == 0;
    }

    friend constexpr bool operator==(const FramebufferScissor&, const FramebufferScissor&) = default;
};

// The canonical bridge from the editor's top-left, Y-down coordinate system
// to a Vulkan framebuffer using a positive-height viewport. Keeping this math
// outside the renderer makes raster quads, overlays, and pointer mapping use
// the same convention and makes orientation testable without a GPU.
class CanvasCoordinateMapping final {
public:
    CanvasCoordinateMapping(core::Extent2d documentExtent,
        core::Extent2d logicalViewportExtent, core::Extent2u framebufferExtent,
        core::ViewportState viewport);

    [[nodiscard]] core::Vec2d documentToLogicalViewport(
        core::Vec2d documentPoint) const noexcept;
    [[nodiscard]] core::Vec2d logicalViewportToDocument(
        core::Vec2d logicalPoint) const noexcept;
    [[nodiscard]] core::Vec2d logicalViewportToFramebuffer(
        core::Vec2d logicalPoint) const noexcept;
    [[nodiscard]] core::Vec2d documentToFramebuffer(
        core::Vec2d documentPoint) const noexcept;
    [[nodiscard]] core::Vec2d framebufferToVulkanClip(
        core::Vec2d framebufferPoint) const noexcept;
    [[nodiscard]] core::Vec2d documentToVulkanClip(
        core::Vec2d documentPoint) const noexcept;

    // Floating-point framebuffer rectangle used by fragment shaders for an
    // exact half-open canvas clip. The integer scissor is a conservative,
    // framebuffer-clamped optimization around that same rectangle.
    [[nodiscard]] std::array<double, 4> documentCanvasFramebufferRect() const noexcept;
    [[nodiscard]] FramebufferScissor documentCanvasFramebufferScissor() const noexcept;

    // Column-major matrix consumed directly by GLSL. Layer unit coordinates
    // and texture UVs are both (0,0) at the top-left and (1,1) bottom-right.
    [[nodiscard]] std::array<float, 16> layerUnitToVulkanClip(
        const core::AffineTransform& localToDocument,
        core::Extent2u layerExtent) const noexcept;

private:
    core::Extent2d documentExtent_;
    core::Extent2d logicalViewportExtent_;
    core::Extent2d framebufferExtent_;
    core::ViewportState viewport_;
};

} // namespace imageeditor::render
