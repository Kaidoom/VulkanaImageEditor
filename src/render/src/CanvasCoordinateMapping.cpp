#include "imageeditor/render/CanvasCoordinateMapping.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace imageeditor::render {

CanvasCoordinateMapping::CanvasCoordinateMapping(core::Extent2d documentExtent,
    core::Extent2d logicalViewportExtent, core::Extent2u framebufferExtent,
    core::ViewportState viewport)
    : documentExtent_(documentExtent)
    , logicalViewportExtent_ {
        std::max(1.0, logicalViewportExtent.width),
        std::max(1.0, logicalViewportExtent.height),
    }
    , framebufferExtent_ {
        std::max(1.0, static_cast<double>(framebufferExtent.width)),
        std::max(1.0, static_cast<double>(framebufferExtent.height)),
    }
    , viewport_(viewport)
{
}

core::Vec2d CanvasCoordinateMapping::documentToLogicalViewport(
    core::Vec2d documentPoint) const noexcept
{
    return viewport_.documentToViewport(
        documentPoint, documentExtent_, logicalViewportExtent_);
}

core::Vec2d CanvasCoordinateMapping::logicalViewportToDocument(
    core::Vec2d logicalPoint) const noexcept
{
    return viewport_.viewportToDocument(
        logicalPoint, documentExtent_, logicalViewportExtent_);
}

core::Vec2d CanvasCoordinateMapping::logicalViewportToFramebuffer(
    core::Vec2d logicalPoint) const noexcept
{
    return {
        logicalPoint.x * framebufferExtent_.width / logicalViewportExtent_.width,
        logicalPoint.y * framebufferExtent_.height / logicalViewportExtent_.height,
    };
}

core::Vec2d CanvasCoordinateMapping::documentToFramebuffer(
    core::Vec2d documentPoint) const noexcept
{
    return logicalViewportToFramebuffer(documentToLogicalViewport(documentPoint));
}

core::Vec2d CanvasCoordinateMapping::framebufferToVulkanClip(
    core::Vec2d framebufferPoint) const noexcept
{
    // Vulkan's positive-height viewport maps NDC -1 to framebuffer coordinate
    // zero and NDC +1 to the positive framebuffer extent on both axes.
    return {
        2.0 * framebufferPoint.x / framebufferExtent_.width - 1.0,
        2.0 * framebufferPoint.y / framebufferExtent_.height - 1.0,
    };
}

core::Vec2d CanvasCoordinateMapping::documentToVulkanClip(
    core::Vec2d documentPoint) const noexcept
{
    return framebufferToVulkanClip(documentToFramebuffer(documentPoint));
}

std::array<double, 4> CanvasCoordinateMapping::documentCanvasFramebufferRect() const noexcept
{
    const auto first = documentToFramebuffer({0.0, 0.0});
    const auto second = documentToFramebuffer(
        {documentExtent_.width, documentExtent_.height});
    const auto left = std::min(first.x, second.x);
    const auto top = std::min(first.y, second.y);
    return {
        left,
        top,
        std::max(0.0, std::max(first.x, second.x) - left),
        std::max(0.0, std::max(first.y, second.y) - top),
    };
}

FramebufferScissor CanvasCoordinateMapping::documentCanvasFramebufferScissor() const noexcept
{
    const auto rect = documentCanvasFramebufferRect();
    const auto left = std::clamp(std::floor(rect[0]), 0.0, framebufferExtent_.width);
    const auto top = std::clamp(std::floor(rect[1]), 0.0, framebufferExtent_.height);
    const auto right = std::clamp(
        std::ceil(rect[0] + rect[2]), 0.0, framebufferExtent_.width);
    const auto bottom = std::clamp(
        std::ceil(rect[1] + rect[3]), 0.0, framebufferExtent_.height);
    constexpr auto maxOffset = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    constexpr auto maxExtent = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    return {
        .x = static_cast<std::int32_t>(std::min(left, maxOffset)),
        .y = static_cast<std::int32_t>(std::min(top, maxOffset)),
        .width = static_cast<std::uint32_t>(std::min(std::max(0.0, right - left), maxExtent)),
        .height = static_cast<std::uint32_t>(std::min(std::max(0.0, bottom - top), maxExtent)),
    };
}

std::array<float, 16> CanvasCoordinateMapping::layerUnitToVulkanClip(
    const core::AffineTransform& localToDocument, core::Extent2u layerExtent) const noexcept
{
    const auto width = static_cast<double>(layerExtent.width);
    const auto height = static_cast<double>(layerExtent.height);
    const auto origin = documentToVulkanClip(localToDocument.map({0.0, 0.0}));
    const auto unitX = documentToVulkanClip(localToDocument.map({width, 0.0}));
    const auto unitY = documentToVulkanClip(localToDocument.map({0.0, height}));

    return {
        static_cast<float>(unitX.x - origin.x),
        static_cast<float>(unitX.y - origin.y),
        0.0F,
        0.0F,
        static_cast<float>(unitY.x - origin.x),
        static_cast<float>(unitY.y - origin.y),
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        1.0F,
        0.0F,
        static_cast<float>(origin.x),
        static_cast<float>(origin.y),
        0.0F,
        1.0F,
    };
}

} // namespace imageeditor::render
