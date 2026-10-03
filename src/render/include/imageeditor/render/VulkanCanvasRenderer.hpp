#pragma once

#include <vulkan/vulkan.h>

#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/core/Adjustments.hpp"
#include "imageeditor/render/CanvasScene.hpp"
#include "imageeditor/render/RendererStats.hpp"
#include "imageeditor/render/PointerTooltip.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace imageeditor::render {

struct VulkanDeviceContext {
    VkPhysicalDevice physicalDevice {VK_NULL_HANDLE};
    VkDevice device {VK_NULL_HANDLE};
    VkPhysicalDeviceProperties properties {};
    std::uint32_t frameSlotCount {0};
};

struct VulkanSwapchainContext {
    VkRenderPass renderPass {VK_NULL_HANDLE};
    VkFormat colorFormat {VK_FORMAT_UNDEFINED};
    VkExtent2D extent {};
};

struct VulkanFrameContext {
    VkCommandBuffer commandBuffer {VK_NULL_HANDLE};
    VkFramebuffer framebuffer {VK_NULL_HANDLE};
    VkExtent2D extent {};
    std::uint32_t frameSlot {0};
};

// Records the product's canvas pass. Window-system integration, frame
// acquisition, submission, and presentation are deliberately owned by the
// host backend instead.
class VulkanCanvasRenderer final {
public:
    VulkanCanvasRenderer() = default;
    ~VulkanCanvasRenderer() = default;

    void initializeDevice(const VulkanDeviceContext& context);
    void initializeSwapchain(const VulkanSwapchainContext& context);
    void releaseSwapchain();
    void releaseDevice();
    void recordFrame(const CanvasScene& scene, const VulkanFrameContext& frame);

    [[nodiscard]] const RendererStats& stats() const noexcept { return stats_; }

private:
    struct BufferResource {
        VkBuffer buffer {VK_NULL_HANDLE};
        VkDeviceMemory memory {VK_NULL_HANDLE};
        VkDeviceSize size {0};
    };

    struct StagingBlock {
        BufferResource resource;
        std::byte* mapped {nullptr};
        VkDeviceSize used {0};
    };

    struct StagingSlice {
        VkBuffer buffer {VK_NULL_HANDLE};
        VkDeviceSize offset {0};
        std::span<std::byte> bytes;
    };

    struct TextureResource {
        VkImage image {VK_NULL_HANDLE};
        VkDeviceMemory memory {VK_NULL_HANDLE};
        VkImageView view {VK_NULL_HANDLE};
        VkDescriptorSet descriptorSet {VK_NULL_HANDLE};
        core::Extent2u extent;
        core::Revision uploadedRevision {0};
        std::uint64_t lastUsedFrame {0};
    };
    struct SelectionBuffer {
        BufferResource resource;
        void* mapped {nullptr};
        core::Revision revision {0};
        core::Revision pathEpoch {0};
        std::size_t edgeCount {0};
        std::size_t stablePrefix {0};
    };
    struct AdjustmentBuffers {
        BufferResource parameters, masks, clipping;
        float* mappedParameters {nullptr};
        VkDescriptorSet descriptor {VK_NULL_HANDLE};
        std::uint64_t maskGeneration {0};
        std::vector<float> parameterKey;
        std::unordered_map<core::LayerId,std::vector<std::uint64_t>> clippingKeys;
    };
    struct AdjustmentProgram {
        core::AdjustmentState state;
        core::CompiledAdjustmentStack compiled;
    };
    // Parameter blocks extend the core ABI with source-cache→layer-local rows
    // and ten (local→mask rows, mask-atlas offset) records, twelve floats each.
    static constexpr std::size_t adjustmentGpuStride = core::adjustmentCount * core::adjustmentParameterStride
        + 12 + core::adjustmentCount * 12;
    void prepareAdjustmentMasks(const CanvasScene&);
    [[nodiscard]] std::int32_t appendEffectParameters(const core::LayerSnapshot&, std::vector<float>&);
    [[nodiscard]] std::int32_t appendAdjustmentParameters(const core::LayerSnapshot&, bool bypass,
        std::vector<float>&);
    [[nodiscard]] VkDescriptorSet uploadAdjustmentData(std::span<const float>, const VulkanFrameContext&);
    void releaseAdjustments();

    struct alignas(16) LayerPushConstants {
        std::array<float, 16> clipFromUnit {};
        float opacity {1.0F};
        std::array<float, 3> padding {};
        std::array<float, 4> canvasRect {};
    };
    static_assert(offsetof(LayerPushConstants, canvasRect) == 80);
    static_assert(sizeof(LayerPushConstants) == 96);

    struct alignas(16) CanvasPushConstants {
        std::array<float, 4> canvasRect {};
        std::array<float, 4> cursorAndFlags {};
        std::array<float, 4> cursorStyle {};
        std::array<float, 4> pickerStyle {};
        std::array<float, 4> pickerReference {};
        std::array<float, 4> pickerCandidate {};
    };
    static_assert(offsetof(CanvasPushConstants, pickerStyle) == 48);
    static_assert(sizeof(CanvasPushConstants) == 96);

    struct alignas(16) TransformPushConstants {
        std::array<float, 4> topCorners {};
        std::array<float, 4> bottomCorners {};
        std::array<float, 4> style {}; // framebuffer/logical scale XY, hovered handle, reserved
        std::array<float, 4> accent {}; // linear-light shared editor accent
        std::array<float, 4> cutX {}; // TL,TR,BR,BL normalized to frame width.
        std::array<float, 4> cutY {}; // Same cuts normalized to frame height.
    };
    static_assert(sizeof(TransformPushConstants) == 96);

    static void checkVk(VkResult result, const char* operation);
    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t typeBits,
        VkMemoryPropertyFlags required) const;
    [[nodiscard]] BufferResource createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties) const;
    [[nodiscard]] StagingSlice reserveStaging(
        std::uint32_t frameSlot, VkDeviceSize size);
    void resetStagingForFrame(std::uint32_t frameSlot) noexcept;
    void releaseStaging();
    void destroyBuffer(BufferResource& resource) const;
    void destroyTexture(TextureResource& resource) const;
    [[nodiscard]] VkShaderModule loadShader(const char* fileName) const;
    [[nodiscard]] VkPipeline createPipeline(VkShaderModule vertexShader,
        VkShaderModule fragmentShader, bool blending, bool selection = false) const;
    void recordSelection(const CanvasScene&, const VulkanFrameContext&);
    void preparePointerTooltip(const CanvasScene&, const VulkanFrameContext&);
    void recordPointerTooltip(const CanvasScene&, const VulkanFrameContext&);
    [[nodiscard]] TextureResource createTexture(const core::RasterSurface& surface);
    [[nodiscard]] TextureResource createTexture(core::Extent2u extent,
        VkFormat format = VK_FORMAT_R8G8B8A8_SRGB, VkImageUsageFlags extraUsage = 0);
    void initializeComposition();
    void createCompositionTarget();
    void releaseCompositionTarget();
    void recordComposition(const CanvasScene&, const VulkanFrameContext&, bool unavailable = false);
    void uploadTextureRegions(VkCommandBuffer commandBuffer, TextureResource& texture,
        const core::RasterSurface& surface, const core::DirtySet& dirty,
        std::uint32_t frameSlot);
    void synchronizeTextures(VkCommandBuffer commandBuffer, const CanvasScene& scene,
        std::uint32_t frameSlot);
    void synchronizeSurface(VkCommandBuffer, const core::RasterSurface&, std::uint32_t frameSlot);
    void pruneTextures(const CanvasScene& scene, std::uint32_t frameSlot);
    void destroyDeferredTexturesForFrame(std::uint32_t frameSlot);
    void recordCanvasPass(const CanvasScene& scene, const VulkanFrameContext& frame);
    [[nodiscard]] LayerPushConstants layerPushConstants(const CanvasScene& scene,
        VkExtent2D targetExtent, const core::LayerSnapshot& layer,
        core::Extent2u layerExtent) const;
    [[nodiscard]] CanvasPushConstants canvasPushConstants(
        const CanvasScene& scene, VkExtent2D targetExtent) const;

    VkDevice device_ {VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_ {VK_NULL_HANDLE};
    VkRenderPass renderPass_ {VK_NULL_HANDLE};
    VkFormat colorFormat_ {VK_FORMAT_UNDEFINED};
    VkExtent2D targetExtent_ {};
    VkDescriptorSetLayout descriptorSetLayout_ {VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool_ {VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_ {VK_NULL_HANDLE};
    VkSampler sampler_ {VK_NULL_HANDLE};
    VkPipeline backgroundPipeline_ {VK_NULL_HANDLE};
    VkPipeline layerPipeline_ {VK_NULL_HANDLE};
    VkDescriptorSetLayout compositionSetLayout_ {VK_NULL_HANDLE};
    VkDescriptorSet compositionStorageSet_ {VK_NULL_HANDLE};
    VkPipelineLayout compositionLayout_ {VK_NULL_HANDLE};
    VkPipeline compositionPipeline_ {VK_NULL_HANDLE};
    VkDescriptorSetLayout adjustmentSetLayout_ {VK_NULL_HANDLE};
    std::vector<AdjustmentBuffers> adjustmentBuffers_;
    std::size_t clippingBytes_{0};
    std::unordered_map<core::LayerId,AdjustmentProgram> adjustmentPrograms_;
    std::vector<core::SelectionState> adjustmentMasks_;
    std::vector<core::Revision> failedAdjustmentMaskRevisions_;
    std::vector<std::uint32_t> adjustmentMaskOffsets_;
    std::vector<std::uint32_t> adjustmentMaskAtlas_ {0};
    std::uint64_t adjustmentMaskGeneration_ {1};
    TextureResource compositionTexture_;
    std::vector<std::uint64_t> compositionKey_;
    VkPipeline overlayPipeline_ {VK_NULL_HANDLE};
    VkPipeline transformPipeline_ {VK_NULL_HANDLE};
    VkPipeline measurePipeline_ {VK_NULL_HANDLE};
    VkPipeline textQuadPipeline_ {VK_NULL_HANDLE};
    VkPipeline selectionPipeline_ {VK_NULL_HANDLE};
    VkPipeline pointerTooltipPipeline_ {VK_NULL_HANDLE};
    PointerTooltipCache pointerTooltipCache_;
    std::vector<TextureResource> pointerTooltipTextures_;
    std::vector<SelectionBuffer> selectionBuffers_;
    std::vector<SelectionBuffer> selectionRetainedBuffers_;
    std::vector<SelectionBuffer> layerOutlineBuffers_;
    std::vector<SelectionBuffer> capturedRegionBuffers_;
    std::vector<SelectionBuffer> repairRegionBuffers_;
    std::unordered_map<core::SurfaceId, TextureResource> textures_;
    std::vector<std::vector<StagingBlock>> stagingBlocks_;
    std::vector<std::vector<TextureResource>> deferredTextures_;
    VkDeviceSize stagingAlignment_ {4};
    RendererStats stats_;
};

} // namespace imageeditor::render
