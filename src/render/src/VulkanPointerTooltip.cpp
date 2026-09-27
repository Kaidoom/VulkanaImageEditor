#include "imageeditor/render/VulkanCanvasRenderer.hpp"
#include <QGuiApplication>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace imageeditor::render {

void VulkanCanvasRenderer::preparePointerTooltip(const CanvasScene& scene, const VulkanFrameContext& frame)
{
    if (scene.pointerTooltip.empty() || (scene.measureActive && !scene.cursorInside)) return;
    auto font = QGuiApplication::font();
    font.setPixelSize(13);
    font.setWeight(QFont::Medium);
    if (pointerTooltipCache_.update(scene.pointerTooltip, scene.devicePixelRatio, font))
        ++stats_.pointerTooltipRasterizations;
    const auto& image = pointerTooltipCache_.image();
    if (image.isNull()) return;
    // The backend has waited this slot's fence. Its texture/staging resources
    // can be updated or replaced without touching any other in-flight frame.
    auto& texture = pointerTooltipTextures_[frame.frameSlot];
    const core::Extent2u needed {std::uint32_t(image.width()), std::uint32_t(image.height())};
    if (texture.image == VK_NULL_HANDLE || texture.extent.width < needed.width
        || texture.extent.height < needed.height) {
        destroyTexture(texture);
        texture = createTexture(core::Extent2u {std::bit_ceil(needed.width), std::bit_ceil(needed.height)});
        ++stats_.pointerTooltipTextureAllocations;
    }
    if (texture.uploadedRevision == pointerTooltipCache_.revision()) return;
    // Clear unused capacity too, so linear filtering at the quad boundary
    // cannot sample stale text from an earlier, longer label.
    const auto bytes = VkDeviceSize(texture.extent.width) * texture.extent.height * 4;
    auto staging = reserveStaging(frame.frameSlot, bytes);
    std::memset(staging.bytes.data(), 0, std::size_t(bytes));
    for (int row = 0; row < image.height(); ++row)
        std::memcpy(staging.bytes.data() + std::size_t(row) * texture.extent.width * 4,
            image.constScanLine(row), std::size_t(image.width()) * 4);

    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = texture.uploadedRevision ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = texture.uploadedRevision ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(frame.commandBuffer,
        texture.uploadedRevision ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy {};
    copy.bufferOffset = staging.offset;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {texture.extent.width, texture.extent.height, 1};
    vkCmdCopyBufferToImage(frame.commandBuffer, staging.buffer, texture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(frame.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    texture.uploadedRevision = pointerTooltipCache_.revision();
    ++stats_.pointerTooltipUploads;
    stats_.pointerTooltipUploadedBytes += bytes;
}

void VulkanCanvasRenderer::recordPointerTooltip(const CanvasScene& scene, const VulkanFrameContext& frame)
{
    if (scene.pointerTooltip.empty() || (scene.measureActive && !scene.cursorInside) || pointerTooltipCache_.image().isNull()
        || scene.logicalViewport.width <= 0 || scene.logicalViewport.height <= 0) return;
    const auto& texture = pointerTooltipTextures_[frame.frameSlot];
    if (texture.image == VK_NULL_HANDLE) return;
    const double sx = frame.extent.width / scene.logicalViewport.width;
    const double sy = frame.extent.height / scene.logicalViewport.height;
    QRectF area(0, 0, scene.logicalViewport.width, scene.logicalViewport.height);
    if (!scene.pointerTooltipArea.empty()) {
        const auto& safe = scene.pointerTooltipArea;
        const auto clipped = area.intersected(QRectF(safe.x, safe.y, safe.width, safe.height));
        if (clipped.width() >= pointerTooltipCache_.logicalSize().width() + 12
            && clipped.height() >= pointerTooltipCache_.logicalSize().height() + 12) area = clipped;
    }
    auto rect = pointerTooltipRect({scene.cursorLogical.x, scene.cursorLogical.y},
        pointerTooltipCache_.logicalSize(), area);
    if (rect.isEmpty()) return;
    // Raster-aligned placement prevents text shimmer while the pointer moves
    // at subpixel positions. Tooltip pixels stay independent of canvas zoom.
    const auto& raster = pointerTooltipCache_.image();
    const auto rasterScale = raster.devicePixelRatio();
    struct alignas(16) Push {
        std::array<float, 4> rect;
        std::array<float, 4> viewportAndUv;
    } push {
        {float(std::round(rect.x() * sx)), float(std::round(rect.y() * sy)),
            float(raster.width() / rasterScale * sx), float(raster.height() / rasterScale * sy)},
        {float(frame.extent.width), float(frame.extent.height),
            float(raster.width()) / float(texture.extent.width), float(raster.height()) / float(texture.extent.height)}
    };
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pointerTooltipPipeline_);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
        0, 1, &texture.descriptorSet, 0, nullptr);
    vkCmdPushConstants(frame.commandBuffer, pipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(frame.commandBuffer, 6, 1, 0, 0);
}
}
