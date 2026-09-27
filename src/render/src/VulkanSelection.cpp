#include "imageeditor/render/CanvasCoordinateMapping.hpp"
#include "imageeditor/render/VulkanCanvasRenderer.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include <algorithm>
#include <bit>

namespace imageeditor::render {
void VulkanCanvasRenderer::recordSelection(const CanvasScene& scene, const VulkanFrameContext& frame)
{
    const CanvasCoordinateMapping mapping(
        { double(scene.document.canvas.extent.width), double(scene.document.canvas.extent.height) },
        scene.logicalViewport, { frame.extent.width, frame.extent.height }, scene.viewport);
    const auto origin = mapping.documentToFramebuffer({ 0, 0 });
    const auto pixel = mapping.documentToFramebuffer({ 1, 1 }) - origin;
    const auto logical = mapping.logicalViewportToFramebuffer({ 1, 1 });
    struct alignas(16) Push {
        std::array<float, 4> originScale, viewportStyle, colorStyle, canvasRect, outsideColor;
    } push { { float(origin.x), float(origin.y), float(pixel.x), float(pixel.y) },
        { float(frame.extent.width), float(frame.extent.height), float(logical.x),
            float(scene.selectionPhase) },
        { scene.overlayAccent.red / 255.0F, scene.overlayAccent.green / 255.0F,
            scene.overlayAccent.blue / 255.0F, 0 },
        {float(origin.x), float(origin.y), float(origin.x + pixel.x * scene.document.canvas.extent.width),
            float(origin.y + pixel.y * scene.document.canvas.extent.height)}, {} };
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, selectionPipeline_);
    const auto record = [&](const auto& edges, SelectionBuffer& buffer, core::Revision revision,
                            core::Revision epoch, std::size_t stable, bool pending, float solidStyle = 0) {
        if (!edges || edges->empty())
            return;
        const auto count = edges->size();
        const auto bytes = VkDeviceSize(count * 4 * sizeof(float));
        // The frame slot fence is already complete. Each stream owns its own
        // persistently mapped buffer so retained ants never follow path edits.
        if (buffer.resource.size < bytes) {
            if (buffer.mapped)
                vkUnmapMemory(device_, buffer.resource.memory);
            destroyBuffer(buffer.resource);
            buffer.resource = createBuffer(std::max<VkDeviceSize>(4096, std::bit_ceil(bytes)),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            checkVk(vkMapMemory(device_, buffer.resource.memory, 0, buffer.resource.size, 0, &buffer.mapped),
                "map selection vertices");
            buffer.revision = 0;
            buffer.pathEpoch = 0;
            buffer.edgeCount = 0;
            buffer.stablePrefix = 0;
        }
        if (buffer.revision != revision) {
            const auto first = pending && buffer.pathEpoch == epoch
                ? std::min({ count, buffer.edgeCount, stable, buffer.stablePrefix })
                : 0;
            auto* vertices = static_cast<float*>(buffer.mapped) + first * 4;
            for (std::size_t i = first; i < count; ++i) {
                const auto& edge = (*edges)[i];
                *vertices++ = float(edge.from.x);
                *vertices++ = float(edge.from.y);
                *vertices++ = float(edge.to.x);
                *vertices++ = float(edge.to.y);
            }
            buffer.revision = revision;
            buffer.pathEpoch = epoch;
            buffer.edgeCount = count;
            buffer.stablePrefix = stable;
            if (solidStyle == 4) {
                ++stats_.layerOutlineGeometryUploads;
                stats_.layerOutlineUploadedBytes += (count - first) * 4 * sizeof(float);
            } else {
                ++stats_.selectionGeometryUploads;
                stats_.selectionUploadedBytes += (count - first) * 4 * sizeof(float);
            }
        }
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &buffer.resource.buffer, &offset);
        const auto draw = [&](std::size_t first, std::size_t amount, float style) {
            if (!amount)
                return;
            push.colorStyle[3] = style;
            vkCmdPushConstants(frame.commandBuffer, pipelineLayout_,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(frame.commandBuffer, 6, std::uint32_t(amount), 0, std::uint32_t(first));
        };
        if (!pending) {
            draw(0, count, solidStyle);
            return;
        }
        const auto anchors = std::min(count, scene.selectionAnchorCount);
        const auto closing = std::min(count - anchors, scene.selectionClosingEdges);
        draw(0, count - anchors - closing, 1);
        draw(count - anchors - closing, closing, 2);
        draw(count - anchors, anchors, 3);
    };
    // A third cached edge stream shares the instanced line pipeline but has no
    // ants/animation. Fragment classification keeps canvas crossings exact at
    // every pan/zoom, without rebuilding/splitting document-space geometry.
    const auto selectionColor = push.colorStyle;
    const auto c = scene.layerOutlineColor;
    const auto linear = [](std::uint8_t value) { return float(core::srgbToLinear(value)); };
    const auto darker = [&](std::uint8_t value) { return linear(std::uint8_t(std::lround(value * .62))); };
    push.colorStyle = {linear(c.red), linear(c.green), linear(c.blue), 4};
    push.outsideColor = {darker(c.red), darker(c.green), darker(c.blue), 1};
    record(scene.layerOutlineEdges, layerOutlineBuffers_.at(frame.frameSlot),
        scene.layerOutlineRevision, 0, 0, false, 4);
    const auto accent = scene.overlayAccent;
    push.colorStyle = {linear(accent.red), linear(accent.green), linear(accent.blue), 5};
    record(scene.capturedRegionEdges, capturedRegionBuffers_.at(frame.frameSlot),
        scene.capturedRegionRevision, 0, 0, false, 5);
    record(scene.repairRegionEdges, repairRegionBuffers_.at(frame.frameSlot),
        scene.repairRegionRevision, 0, 0, false, 6);
    push.colorStyle = selectionColor;
    record(scene.selectionRetainedEdges, selectionRetainedBuffers_.at(frame.frameSlot),
        scene.selectionRetainedRevision, 0, 0, false);
    record(scene.selectionEdges, selectionBuffers_.at(frame.frameSlot), scene.selectionEdgesRevision,
        scene.selectionPathEpoch, scene.selectionStablePrefix, scene.selectionPathPreview);
}
}
