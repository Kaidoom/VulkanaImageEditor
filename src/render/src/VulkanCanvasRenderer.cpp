#include "imageeditor/render/VulkanCanvasRenderer.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/RegionalRasterSurface.hpp"

#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/Layer.hpp"
#include "imageeditor/render/CanvasCoordinateMapping.hpp"
#include "imageeditor/render/DirtyRegionCoalescer.hpp"
#include "imageeditor/render/RasterUploadPlan.hpp"

#include <QFile>
#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>

Q_LOGGING_CATEGORY(logVulkanCanvas, "imageeditor.vulkan")

namespace imageeditor::render {
namespace {

constexpr VkDeviceSize kMaxPushConstantBytes = 96;
constexpr VkDeviceSize kMinimumStagingBlockBytes = 1024U * 1024U;
constexpr std::uint32_t kUploadGridSize = 64;

} // namespace

void VulkanCanvasRenderer::initializeDevice(const VulkanDeviceContext& context)
{
    device_ = context.device;
    physicalDevice_ = context.physicalDevice;
    stagingBlocks_.resize(context.frameSlotCount);
    adjustmentBuffers_.resize(context.frameSlotCount);
    deferredTextures_.resize(context.frameSlotCount);
    selectionBuffers_.resize(context.frameSlotCount);
    selectionRetainedBuffers_.resize(context.frameSlotCount);
    layerOutlineBuffers_.resize(context.frameSlotCount);
    capturedRegionBuffers_.resize(context.frameSlotCount);
    repairRegionBuffers_.resize(context.frameSlotCount);
    pointerTooltipTextures_.resize(context.frameSlotCount);
    stagingAlignment_ = std::max<VkDeviceSize>(4,
        context.properties.limits.optimalBufferCopyOffsetAlignment);
    stats_.deviceName = context.properties.deviceName;
    stats_.maximumImageDimension2D = context.properties.limits.maxImageDimension2D;
    qCInfo(logVulkanCanvas) << "Canvas renderer device:" << context.properties.deviceName
                            << "API" << VK_VERSION_MAJOR(context.properties.apiVersion)
                            << VK_VERSION_MINOR(context.properties.apiVersion)
                            << VK_VERSION_PATCH(context.properties.apiVersion);

    VkDescriptorSetLayoutBinding samplerBinding {};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorLayoutInfo.bindingCount = 1;
    descriptorLayoutInfo.pBindings = &samplerBinding;
    checkVk(vkCreateDescriptorSetLayout(device_, &descriptorLayoutInfo, nullptr, &descriptorSetLayout_),
        "create descriptor-set layout");

    const std::array poolSizes {VkDescriptorPoolSize {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1032},
        VkDescriptorPoolSize {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        VkDescriptorPoolSize {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3*context.frameSlotCount}};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1033 + context.frameSlotCount;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "create descriptor pool");

    VkPushConstantRange pushRange {};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = static_cast<std::uint32_t>(kMaxPushConstantBytes);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    checkVk(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
        "create pipeline layout");

    VkSamplerCreateInfo samplerInfo {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0F;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_), "create texture sampler");
    initializeComposition();

    ++stats_.resourceGeneration;
}

void VulkanCanvasRenderer::initializeSwapchain(const VulkanSwapchainContext& context)
{
    renderPass_ = context.renderPass;
    colorFormat_ = context.colorFormat;
    targetExtent_ = context.extent;
    stats_.swapchainWidth = targetExtent_.width;
    stats_.swapchainHeight = targetExtent_.height;
    const auto fullscreenVertex = loadShader("fullscreen.vert.spv");
    const auto checkerFragment = loadShader("checkerboard.frag.spv");
    const auto layerFragment = loadShader("layer.frag.spv");
    const auto overlayFragment = loadShader("overlay.frag.spv");
    const auto transformFragment = loadShader("transform.frag.spv");
    const auto measureFragment = loadShader("measure.frag.spv");
    const auto selectionVertex = loadShader("selection.vert.spv");
    const auto selectionFragment = loadShader("selection.frag.spv");
    const auto tooltipVertex = loadShader("tooltip.vert.spv");
    const auto tooltipFragment = loadShader("tooltip.frag.spv");
    const auto textVertex = loadShader("textquad.vert.spv");
    const auto textFragment = loadShader("textquad.frag.spv");
    textQuadPipeline_ = createPipeline(textVertex, textFragment, true);
    vkDestroyShaderModule(device_, textVertex, nullptr);
    vkDestroyShaderModule(device_, textFragment, nullptr);

    backgroundPipeline_ = createPipeline(fullscreenVertex, checkerFragment, false);
    layerPipeline_ = createPipeline(fullscreenVertex, layerFragment, true);
    overlayPipeline_ = createPipeline(fullscreenVertex, overlayFragment, true);
    transformPipeline_ = createPipeline(fullscreenVertex, transformFragment, true);
    measurePipeline_ = createPipeline(fullscreenVertex, measureFragment, true);
    vkDestroyShaderModule(device_, measureFragment, nullptr);
    selectionPipeline_ = createPipeline(selectionVertex, selectionFragment, true, true);
    pointerTooltipPipeline_ = createPipeline(tooltipVertex, tooltipFragment, true);
    vkDestroyShaderModule(device_, tooltipVertex, nullptr);
    vkDestroyShaderModule(device_, tooltipFragment, nullptr);
    vkDestroyShaderModule(device_, selectionVertex, nullptr);
    vkDestroyShaderModule(device_, selectionFragment, nullptr);
    vkDestroyShaderModule(device_, transformFragment, nullptr);

    vkDestroyShaderModule(device_, overlayFragment, nullptr);
    vkDestroyShaderModule(device_, layerFragment, nullptr);
    vkDestroyShaderModule(device_, checkerFragment, nullptr);
    vkDestroyShaderModule(device_, fullscreenVertex, nullptr);
    createCompositionTarget();

    ++stats_.swapchainGeneration;
    qCInfo(logVulkanCanvas) << "Initialized swapchain resources"
                            << targetExtent_.width << 'x' << targetExtent_.height
                            << "format" << colorFormat_
                            << "generation" << stats_.swapchainGeneration;
}

void VulkanCanvasRenderer::releaseSwapchain()
{
    releaseCompositionTarget();
    if (measurePipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, measurePipeline_, nullptr);
        measurePipeline_ = VK_NULL_HANDLE;
    }
    if (textQuadPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, textQuadPipeline_, nullptr);
        textQuadPipeline_ = VK_NULL_HANDLE;
    }
    if (pointerTooltipPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pointerTooltipPipeline_, nullptr);
        pointerTooltipPipeline_ = VK_NULL_HANDLE;
    }
    if (selectionPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, selectionPipeline_, nullptr);
        selectionPipeline_ = VK_NULL_HANDLE;
    }
    if (transformPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, transformPipeline_, nullptr);
        transformPipeline_ = VK_NULL_HANDLE;
    }
    if (backgroundPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, backgroundPipeline_, nullptr);
        backgroundPipeline_ = VK_NULL_HANDLE;
    }
    if (layerPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, layerPipeline_, nullptr);
        layerPipeline_ = VK_NULL_HANDLE;
    }
    if (overlayPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, overlayPipeline_, nullptr);
        overlayPipeline_ = VK_NULL_HANDLE;
    }
    renderPass_ = VK_NULL_HANDLE;
    colorFormat_ = VK_FORMAT_UNDEFINED;
    targetExtent_ = {};
    stats_.swapchainWidth = 0;
    stats_.swapchainHeight = 0;
    qCInfo(logVulkanCanvas) << "Released swapchain resources";
}

void VulkanCanvasRenderer::releaseDevice()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    releaseCompositionTarget();
    if (compositionPipeline_) vkDestroyPipeline(device_, compositionPipeline_, nullptr);
    if (compositionLayout_) vkDestroyPipelineLayout(device_, compositionLayout_, nullptr);
    if (compositionSetLayout_) vkDestroyDescriptorSetLayout(device_, compositionSetLayout_, nullptr);
    compositionPipeline_ = VK_NULL_HANDLE;
    compositionLayout_ = VK_NULL_HANDLE;
    compositionSetLayout_ = VK_NULL_HANDLE;
    releaseAdjustments();
    releaseStaging();
    for (auto& texture : pointerTooltipTextures_) destroyTexture(texture);
    pointerTooltipTextures_.clear();
    for (auto& buffer : selectionBuffers_) {
        if (buffer.mapped) vkUnmapMemory(device_, buffer.resource.memory);
        destroyBuffer(buffer.resource);
    }
    selectionBuffers_.clear();
    for (auto& buffer : selectionRetainedBuffers_) {
        if (buffer.mapped) vkUnmapMemory(device_, buffer.resource.memory);
        destroyBuffer(buffer.resource);
    }
    selectionRetainedBuffers_.clear();
    for (auto& buffer : layerOutlineBuffers_) {
        if (buffer.mapped) vkUnmapMemory(device_, buffer.resource.memory);
        destroyBuffer(buffer.resource);
    }
    layerOutlineBuffers_.clear();
    for (auto& buffer : capturedRegionBuffers_) {
        if (buffer.mapped) vkUnmapMemory(device_, buffer.resource.memory);
        destroyBuffer(buffer.resource);
    }
    capturedRegionBuffers_.clear();
    for (auto& buffer : repairRegionBuffers_) {
        if (buffer.mapped) vkUnmapMemory(device_, buffer.resource.memory);
        destroyBuffer(buffer.resource);
    }
    repairRegionBuffers_.clear();
    for (auto& deferred : deferredTextures_) {
        for (auto& texture : deferred) {
            destroyTexture(texture);
        }
        deferred.clear();
    }
    for (auto& [surfaceId, texture] : textures_) {
        (void)surfaceId;
        destroyTexture(texture);
    }
    textures_.clear();

    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    stagingBlocks_.clear();
    deferredTextures_.clear();
    qCInfo(logVulkanCanvas) << "Released device resources";
}

void VulkanCanvasRenderer::recordFrame(
    const CanvasScene& scene, const VulkanFrameContext& frame)
{
    if (frame.frameSlot >= stagingBlocks_.size()) {
        qFatal("Canvas renderer received an invalid frame slot");
    }
    resetStagingForFrame(frame.frameSlot);
    destroyDeferredTexturesForFrame(frame.frameSlot);
    synchronizeTextures(frame.commandBuffer, scene, frame.frameSlot);
    pruneTextures(scene, frame.frameSlot);
    try {
        recordComposition(scene, frame);
        stats_.compositionError.clear();
    } catch(const std::length_error& error) {
        // Explicit derived-data limits must never unwind through Qt, lose the
        // acquired image semaphore, or discard an unsaved document. Submit a
        // normal frame with an unmistakable error instead of a partial image.
        stats_.compositionError=error.what();
        auto unavailable=scene;
        unavailable.document.layersBottomToTop.clear();
        unavailable.pointerTooltip="Canvas preview unavailable: "+stats_.compositionError+". Disable an effect or undo; document data is intact.";
        unavailable.cursorLogical={scene.logicalViewport.width*.5,scene.logicalViewport.height*.5};
        unavailable.measureActive=false;
        recordComposition(unavailable,frame,true);
        preparePointerTooltip(unavailable,frame);
        recordCanvasPass(unavailable,frame);
        ++stats_.framesSubmitted;
        return;
    }
    preparePointerTooltip(scene, frame);
    recordCanvasPass(scene, frame);
    ++stats_.framesSubmitted;
}

void VulkanCanvasRenderer::checkVk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        qFatal("Vulkan failed to %s (VkResult %d)", operation, static_cast<int>(result));
    }
}

std::uint32_t VulkanCanvasRenderer::findMemoryType(std::uint32_t typeBits,
    VkMemoryPropertyFlags required) const
{
    VkPhysicalDeviceMemoryProperties properties {};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const bool supported = (typeBits & (1U << index)) != 0;
        const bool hasFlags = (properties.memoryTypes[index].propertyFlags & required) == required;
        if (supported && hasFlags) {
            return index;
        }
    }
    qFatal("No compatible Vulkan memory type found");
    return 0;
}

VulkanCanvasRenderer::BufferResource VulkanCanvasRenderer::createBuffer(VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryProperties) const
{
    BufferResource resource {.size = size};
    VkBufferCreateInfo bufferInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checkVk(vkCreateBuffer(device_, &bufferInfo, nullptr, &resource.buffer), "create buffer");

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device_, resource.buffer, &requirements);
    VkMemoryAllocateInfo allocationInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, memoryProperties);
    checkVk(vkAllocateMemory(device_, &allocationInfo, nullptr, &resource.memory), "allocate buffer memory");
    checkVk(vkBindBufferMemory(device_, resource.buffer, resource.memory, 0), "bind buffer memory");
    return resource;
}

VulkanCanvasRenderer::StagingSlice VulkanCanvasRenderer::reserveStaging(
    std::uint32_t frameSlot, VkDeviceSize size)
{
    if (size == 0 || frameSlot >= stagingBlocks_.size()) {
        qFatal("Canvas renderer received an invalid staging reservation");
    }
    if (size > static_cast<VkDeviceSize>(std::numeric_limits<std::size_t>::max())) {
        qFatal("Canvas renderer staging reservation exceeds addressable memory");
    }

    const auto tryReserve = [this, size](StagingBlock& block)
        -> std::optional<StagingSlice> {
        const auto remainder = block.used % stagingAlignment_;
        const auto padding = remainder == 0 ? 0 : stagingAlignment_ - remainder;
        if (block.used > std::numeric_limits<VkDeviceSize>::max() - padding) {
            return std::nullopt;
        }
        const auto offset = block.used + padding;
        if (offset > block.resource.size || size > block.resource.size - offset) {
            return std::nullopt;
        }
        if (offset > static_cast<VkDeviceSize>(
                std::numeric_limits<std::size_t>::max())) {
            return std::nullopt;
        }
        block.used = offset + size;
        return StagingSlice {
            .buffer = block.resource.buffer,
            .offset = offset,
            .bytes = std::span<std::byte>(block.mapped
                    + static_cast<std::size_t>(offset),
                static_cast<std::size_t>(size)),
        };
    };

    auto& blocks = stagingBlocks_[frameSlot];
    for (auto& block : blocks) {
        if (auto slice = tryReserve(block)) {
            return *slice;
        }
    }

    auto capacity = kMinimumStagingBlockBytes;
    while (capacity < size) {
        if (capacity > std::numeric_limits<VkDeviceSize>::max() / 2) {
            capacity = size;
            break;
        }
        capacity *= 2;
    }
    StagingBlock block;
    block.resource = createBuffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    checkVk(vkMapMemory(device_, block.resource.memory, 0, block.resource.size, 0, &mapped),
        "map persistent staging buffer");
    block.mapped = static_cast<std::byte*>(mapped);
    blocks.push_back(block);
    ++stats_.stagingBufferAllocations;
    stats_.stagingCapacityBytes += capacity;
    auto slice = tryReserve(blocks.back());
    if (!slice) {
        qFatal("New Vulkan staging block could not satisfy its reservation");
    }
    return *slice;
}

void VulkanCanvasRenderer::resetStagingForFrame(std::uint32_t frameSlot) noexcept
{
    for (auto& block : stagingBlocks_[frameSlot]) {
        block.used = 0;
    }
}

void VulkanCanvasRenderer::releaseStaging()
{
    for (auto& blocks : stagingBlocks_) {
        for (auto& block : blocks) {
            if (block.mapped != nullptr) {
                vkUnmapMemory(device_, block.resource.memory);
                block.mapped = nullptr;
            }
            destroyBuffer(block.resource);
        }
        blocks.clear();
    }
    stats_.stagingCapacityBytes = 0;
}

void VulkanCanvasRenderer::destroyBuffer(BufferResource& resource) const
{
    if (resource.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, resource.buffer, nullptr);
    }
    if (resource.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, resource.memory, nullptr);
    }
    resource = {};
}

void VulkanCanvasRenderer::destroyTexture(TextureResource& resource) const
{
    if (resource.descriptorSet != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, descriptorPool_, 1, &resource.descriptorSet);
    }
    if (resource.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, resource.view, nullptr);
    }
    if (resource.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, resource.image, nullptr);
    }
    if (resource.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, resource.memory, nullptr);
    }
    resource = {};
}

VkShaderModule VulkanCanvasRenderer::loadShader(const char* fileName) const
{
    const QString path = platform::shaderPath(QString::fromLatin1(fileName));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Unable to open SPIR-V shader: %s", qPrintable(path));
    }
    const auto bytes = file.readAll();
    if (bytes.isEmpty() || bytes.size() % 4 != 0) {
        qFatal("Invalid SPIR-V shader: %s", qPrintable(path));
    }
    VkShaderModuleCreateInfo createInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = static_cast<std::size_t>(bytes.size());
    createInfo.pCode = reinterpret_cast<const std::uint32_t*>(bytes.constData());
    VkShaderModule module = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &module), "create shader module");
    return module;
}

VkPipeline VulkanCanvasRenderer::createPipeline(VkShaderModule vertexShader,
    VkShaderModule fragmentShader, bool blending, bool selection) const
{
    std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkVertexInputBindingDescription binding {0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_INSTANCE};
    const VkVertexInputAttributeDescription attribute {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    if (selection) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attribute;
    }
    VkPipelineInputAssemblyStateCreateInfo inputAssembly {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = blending ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo blendState {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blendAttachment;

    const std::array dynamicStates {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
        "create graphics pipeline");
    return pipeline;
}

VulkanCanvasRenderer::TextureResource VulkanCanvasRenderer::createTexture(
    const core::RasterSurface& surface)
{
    // Content decoding belongs to the shared compositor, not implementation-
    // dependent fixed-function sRGB texture conversion (especially near gray
    // where the nonseparable modes are sensitive to tiny channel differences).
    return createTexture(surface.extent(), VK_FORMAT_R8G8B8A8_UNORM);
}

VulkanCanvasRenderer::TextureResource VulkanCanvasRenderer::createTexture(core::Extent2u extent,
    VkFormat format, VkImageUsageFlags extraUsage)
{
    TextureResource texture {.extent = extent};
    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {texture.extent.width, texture.extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    checkVk(vkCreateImage(device_, &imageInfo, nullptr, &texture.image), "create layer image");

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device_, texture.image, &requirements);
    VkMemoryAllocateInfo allocationInfo {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVk(vkAllocateMemory(device_, &allocationInfo, nullptr, &texture.memory), "allocate layer image memory");
    checkVk(vkBindImageMemory(device_, texture.image, texture.memory, 0), "bind layer image memory");

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &texture.view), "create layer image view");

    VkDescriptorSetAllocateInfo descriptorInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    descriptorInfo.descriptorPool = descriptorPool_;
    descriptorInfo.descriptorSetCount = 1;
    descriptorInfo.pSetLayouts = &descriptorSetLayout_;
    checkVk(vkAllocateDescriptorSets(device_, &descriptorInfo, &texture.descriptorSet),
        "allocate layer descriptor set");

    VkDescriptorImageInfo imageDescriptor {};
    imageDescriptor.sampler = sampler_;
    imageDescriptor.imageView = texture.view;
    imageDescriptor.imageLayout = extraUsage & VK_IMAGE_USAGE_STORAGE_BIT
        ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = texture.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageDescriptor;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return texture;
}

void VulkanCanvasRenderer::uploadTextureRegions(VkCommandBuffer commandBuffer,
    TextureResource& texture, const core::RasterSurface& surface, const core::DirtySet& dirty,
    std::uint32_t frameSlot)
{
    const auto preparationStarted = std::chrono::steady_clock::now();
    std::vector<core::RectI> regions;
    if (dirty.fullRefresh) {
        regions.push_back({0, 0, static_cast<std::int32_t>(texture.extent.width),
            static_cast<std::int32_t>(texture.extent.height)});
        ++stats_.fullUploads;
    } else {
        stats_.regionalDirtyRegions += dirty.regions.size();
        regions = coalesceDirtyRegionsForUpload(
            dirty.regions, texture.extent, kUploadGridSize);
        if (!regions.empty()) {
            ++stats_.regionalUploadBatches;
        }
        stats_.regionalUploads += regions.size();
    }
    if (regions.empty()) {
        texture.uploadedRevision = dirty.revision;
        return;
    }

    const auto uploadPlan = makeRasterUploadPlan(regions, stagingAlignment_);
    if (uploadPlan.stagingBytes
        > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        qFatal("Raster upload exceeds addressable staging memory");
    }
    auto staging = reserveStaging(
        frameSlot, static_cast<VkDeviceSize>(uploadPlan.stagingBytes));
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(uploadPlan.copies.size());
    for (const auto& planned : uploadPlan.copies) {
        surface.copyRgba8(planned.region,
            staging.bytes.subspan(
                static_cast<std::size_t>(planned.bufferOffset),
                static_cast<std::size_t>(planned.byteCount)),
            planned.rowBytes);

        VkBufferImageCopy copy {};
        copy.bufferOffset = staging.offset
            + static_cast<VkDeviceSize>(planned.bufferOffset);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = {planned.region.x, planned.region.y, 0};
        copy.imageExtent = {
            static_cast<std::uint32_t>(planned.region.width),
            static_cast<std::uint32_t>(planned.region.height),
            1,
        };
        copies.push_back(copy);
    }
    if (copies.size() > std::numeric_limits<std::uint32_t>::max()) {
        qFatal("Raster upload has too many copy regions");
    }

    const auto preparationElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - preparationStarted).count();
    const auto preparationNanoseconds = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, preparationElapsed));
    stats_.uploadPreparationNanoseconds += preparationNanoseconds;
    stats_.maximumUploadPreparationNanoseconds = std::max(
        stats_.maximumUploadPreparationNanoseconds, preparationNanoseconds);
    stats_.lastUploadPreparationNanoseconds = preparationNanoseconds;
    stats_.lastUploadRegionCount = regions.size();

    VkImageMemoryBarrier toTransfer {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = texture.uploadedRevision == 0 ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = texture.uploadedRevision == 0
        ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer,
        texture.uploadedRevision == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    vkCmdCopyBufferToImage(commandBuffer, staging.buffer, texture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(copies.size()), copies.data());
    stats_.uploadedBytes += uploadPlan.pixelBytes;

    VkImageMemoryBarrier toSample {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSample.image = texture.image;
    toSample.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSample.subresourceRange.levelCount = 1;
    toSample.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toSample);
    texture.uploadedRevision = dirty.revision;
}

void VulkanCanvasRenderer::synchronizeSurface(VkCommandBuffer commandBuffer,
                                              const core::RasterSurface &surface, std::uint32_t frameSlot)
{
    auto found = textures_.find(surface.id());
    if (found != textures_.end() && found->second.extent != surface.extent()) {
        // Submitted frames may still read the old image. Retire it using the
        // existing frame fences, never a device-wide idle wait.
        auto replacement = createTexture(surface);
        deferredTextures_[frameSlot].push_back(std::move(found->second));
        found->second = std::move(replacement);
    }
    if (found == textures_.end())
        found = textures_.emplace(surface.id(), createTexture(surface)).first;
    auto &texture = found->second;
    auto dirty = surface.dirtySince(texture.uploadedRevision);
    if (texture.uploadedRevision == 0) {
        const auto *regional = dynamic_cast<const core::RegionalRasterSurface *>(&surface);
        const auto base = regional ? textures_.find(regional->baseSurface()->id()) : textures_.end();
        if (regional && base != textures_.end() &&
            base->second.uploadedRevision == regional->baseSurface()->revision()) {
            const auto bounds = regional->state().bounds;
            const auto sourceBounds = regional->baseBounds();
            const auto overlap = bounds.clippedTo(sourceBounds);
            dirty = {surface.revision(), false, {}};
            for (const auto &[key, tile] : regional->state().tiles) {
                (void)tile;
                auto part = core::RectI{key.first * 64, key.second * 64, 64, 64}.clippedTo(bounds);
                if (part.empty())
                    continue;
                part.x -= bounds.x;
                part.y -= bounds.y;
                dirty.regions.push_back(part);
            }
            const auto barrier = [](VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                    VkAccessFlags source, VkAccessFlags destination) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.image = image;
                b.oldLayout = oldLayout;
                b.newLayout = newLayout;
                b.srcAccessMask = source;
                b.dstAccessMask = destination;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                return b;
            };
            const std::array prepare{barrier(texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                             VK_ACCESS_TRANSFER_WRITE_BIT),
                                     barrier(base->second.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                             VK_ACCESS_TRANSFER_READ_BIT)};
            vkCmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, prepare.data());
            const VkClearColorValue clear{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(commandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                                 1, &range);
            const auto cleared = barrier(texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &cleared);
            if (!overlap.empty()) {
                VkImageCopy copy{};
                copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.srcOffset = {overlap.x - sourceBounds.x, overlap.y - sourceBounds.y, 0};
                copy.dstOffset = {overlap.x - bounds.x, overlap.y - bounds.y, 0};
                copy.extent = {std::uint32_t(overlap.width), std::uint32_t(overlap.height), 1};
                vkCmdCopyImage(commandBuffer, base->second.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
            const std::array finish{barrier(texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
                                    barrier(base->second.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT)};
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 2, finish.data());
            texture.uploadedRevision = surface.revision();
            ++stats_.regionalSourceCopies;
        }
    }
    if (!dirty.empty())
        uploadTextureRegions(commandBuffer, texture, surface, dirty, frameSlot);
}

void VulkanCanvasRenderer::synchronizeTextures(VkCommandBuffer commandBuffer, const CanvasScene &scene,
                                               std::uint32_t frameSlot)
{
    if (scene.pixelPreviewEnabled) {
        if (const auto &surface = scene.pixelPreview) {
            synchronizeSurface(commandBuffer, *surface, frameSlot);
        }
        return;
    }
    for (const auto &layer : scene.document.layersBottomToTop) {
        if (!layer.visible) {
            continue;
        }
        const core::RasterLayerSnapshot raster{scene.filterBypassLayer == layer.id ||
                                                       scene.adjustmentBypassLayer == layer.id
                                                   ? core::intrinsicSurface(layer)
                                                   : core::renderedSurface(layer)};
        if (!raster.surface) {
            continue;
        }
        synchronizeSurface(commandBuffer,*raster.surface,frameSlot);
    }
}

void VulkanCanvasRenderer::pruneTextures(const CanvasScene& scene, std::uint32_t frameSlot)
{
    std::unordered_set<core::SurfaceId> liveSurfaces;
    std::unordered_set<core::SurfaceId> retainedSurfaces;
    for (const auto& weak : scene.retainedSources) if (const auto source = weak.lock()) retainedSurfaces.insert(source->id());
    if(scene.pixelPreview)liveSurfaces.insert(scene.pixelPreview->id());
    for (const auto& layer : scene.document.layersBottomToTop) {
        // Keep an already-uploaded original source alive while effects change.
        // Comparison/bypass must not repeatedly upload unchanged raster pixels.
        if(const auto source=core::intrinsicSurface(layer)) {
            liveSurfaces.insert(source->id());
            if(const auto* regional=dynamic_cast<const core::RegionalRasterSurface*>(source.get()))
                retainedSurfaces.insert(regional->baseSurface()->id());
        }
        const core::RasterLayerSnapshot raster {core::renderedSurface(layer)};
        if (raster.surface) {
            liveSurfaces.insert(raster.surface->id());
        }
    }

    auto& deferred = deferredTextures_[frameSlot];
    for (auto iterator = textures_.begin(); iterator != textures_.end();) {
        if (liveSurfaces.contains(iterator->first)) {
            iterator->second.lastUsedFrame = stats_.framesSubmitted;
            ++iterator;
            continue;
        }
        if (retainedSurfaces.contains(iterator->first)) { ++iterator; continue; }
        deferred.push_back(std::move(iterator->second));
        iterator = textures_.erase(iterator);
    }
    constexpr std::uint64_t budget = 512ULL * 1024 * 1024;
    const auto bytes = [](const auto& texture) { return std::uint64_t(texture.extent.width) * texture.extent.height * 4; };
    std::uint64_t total = 0;
    for (const auto& [id, texture] : textures_) { (void)id; total += bytes(texture); }
    while (total > budget) {
        auto oldest = textures_.end();
        for (auto it = textures_.begin(); it != textures_.end(); ++it)
            if (!liveSurfaces.contains(it->first) && (oldest == textures_.end() || it->second.lastUsedFrame < oldest->second.lastUsedFrame)) oldest = it;
        if (oldest == textures_.end()) break; // Never evict a required active input.
        total -= bytes(oldest->second);
        deferred.push_back(std::move(oldest->second)); textures_.erase(oldest);
        ++stats_.inactiveTextureEvictions;
    }
    stats_.textureCacheBytes = total; stats_.textureCacheEntries = textures_.size();
}

void VulkanCanvasRenderer::destroyDeferredTexturesForFrame(std::uint32_t frameSlot)
{
    auto& textures = deferredTextures_[frameSlot];
    for (auto& texture : textures) {
        destroyTexture(texture);
    }
    textures.clear();
}

void VulkanCanvasRenderer::recordCanvasPass(
    const CanvasScene& scene, const VulkanFrameContext& frame)
{
    const auto commandBuffer = frame.commandBuffer;
    VkClearValue clear {};
    clear.color = {{0.043F, 0.047F, 0.058F, 1.0F}};

    VkRenderPassBeginInfo beginInfo {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    beginInfo.renderPass = renderPass_;
    beginInfo.framebuffer = frame.framebuffer;
    beginInfo.renderArea.extent = frame.extent;
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport {};
    viewport.width = static_cast<float>(frame.extent.width);
    viewport.height = static_cast<float>(frame.extent.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    VkRect2D scissor {{0, 0}, beginInfo.renderArea.extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const auto canvasPush = canvasPushConstants(scene, frame.extent);
    const auto linearTheme = [](core::Rgba8 c) { return std::array<float, 4> {
        static_cast<float>(core::srgbToLinear(c.red)), static_cast<float>(core::srgbToLinear(c.green)),
        static_cast<float>(core::srgbToLinear(c.blue)), 1.0F}; };
    // Separate 64-byte packet fits the existing 96-byte layout range. Do not
    // extend CanvasPush beyond Vulkan's guaranteed 128-byte minimum.
    const std::array backgroundPush {canvasPush.canvasRect, linearTheme(scene.canvasBackground),
        linearTheme(scene.checkerLight), linearTheme(scene.checkerDark)};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, backgroundPipeline_);
    vkCmdPushConstants(commandBuffer, pipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        static_cast<std::uint32_t>(sizeof(backgroundPush)), &backgroundPush);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    const core::Extent2d documentExtent {double(scene.document.canvas.extent.width),
        double(scene.document.canvas.extent.height)};
    const CanvasCoordinateMapping mapping(documentExtent,scene.logicalViewport,
        {frame.extent.width,frame.extent.height},scene.viewport);
    const auto layerScissor = mapping.documentCanvasFramebufferScissor();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layerPipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
        0, 1, &compositionTexture_.descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    recordSelection(scene, frame);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
    vkCmdPushConstants(commandBuffer, pipelineLayout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        static_cast<std::uint32_t>(sizeof(canvasPush)), &canvasPush);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    if (scene.transformOverlay) {
        const auto& overlay = *scene.transformOverlay;
        const auto handles = overlay.frameHandles();
        std::array<core::Vec2d, 4> corners;
        for (std::size_t i = 0; i < corners.size(); ++i)
            corners[i] = mapping.documentToFramebuffer(handles[i * 2]);
        TransformPushConstants push;
        push.topCorners = {float(corners[0].x), float(corners[0].y), float(corners[1].x), float(corners[1].y)};
        push.bottomCorners = {float(corners[2].x), float(corners[2].y), float(corners[3].x), float(corners[3].y)};
        const auto scale = mapping.logicalViewportToFramebuffer({1, 1});
        push.style = {float(scale.x), float(scale.y), float(scene.transformHighlight), overlay.chamferHandles?1.0F:0.0F};
        if(overlay.cropGeometry){const auto cuts=overlay.cropGeometry->resolvedCorners();
            for(std::size_t i=0;i<4;++i){
                push.cutX[i]=float(cuts[i]/std::max(1e-9,overlay.cropGeometry->width));
                push.cutY[i]=float(cuts[i]/std::max(1e-9,overlay.cropGeometry->height));
            }
        }
        push.accent = {float(core::srgbToLinear(scene.overlayAccent.red)),
            float(core::srgbToLinear(scene.overlayAccent.green)),
            float(core::srgbToLinear(scene.overlayAccent.blue)), float(scene.overlayAccent.alpha) / 255.0F};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, transformPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            static_cast<std::uint32_t>(sizeof(push)), &push);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }
    const auto overlayLine = [&](core::Vec2d from, core::Vec2d to, bool alignment) {
        const auto a = mapping.documentToFramebuffer(from);
        const auto b = mapping.documentToFramebuffer(to);
        const auto scale = mapping.logicalViewportToFramebuffer({1, 1});
        const std::array<std::array<float, 4>, 3> push {{
            {float(a.x), float(a.y), float(b.x), float(b.y)},
            {float(scale.x), float(scale.y), alignment ? 1.0F : 0.0F, 0},
            linearTheme(scene.overlayAccent)}};
        // Small/axis-aligned measurements need not shade the whole workspace.
        // Clip only to the framebuffer, never to document content bounds.
        const auto padding = (alignment ? 3 : 7) * std::max(scale.x, scale.y);
        const int left = int(std::clamp(std::floor(std::min(a.x, b.x) - padding), 0.0, double(frame.extent.width)));
        const int top = int(std::clamp(std::floor(std::min(a.y, b.y) - padding), 0.0, double(frame.extent.height)));
        const int right = int(std::clamp(std::ceil(std::max(a.x, b.x) + padding), 0.0, double(frame.extent.width)));
        const int bottom = int(std::clamp(std::ceil(std::max(a.y, b.y) + padding), 0.0, double(frame.extent.height)));
        if (right > left && bottom > top) {
            const VkRect2D measureScissor {{left, top}, {std::uint32_t(right - left), std::uint32_t(bottom - top)}};
            vkCmdSetScissor(commandBuffer, 0, 1, &measureScissor);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, measurePipeline_);
            vkCmdPushConstants(commandBuffer, pipelineLayout_,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                static_cast<std::uint32_t>(sizeof(push)), &push);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        }
    };
    if (scene.measurement || (scene.activeTool==core::ToolId::Cloning && scene.cloneSourceAnchor)) {
        const auto sourcePoint=scene.cloneSourceOffset && scene.cursorInside
            ?scene.constrainedBrushPosition.value_or(mapping.logicalViewportToDocument(scene.cursorLogical))+*scene.cloneSourceOffset
            :scene.cloneSourceAnchor.value_or(core::Vec2d{});
        overlayLine(scene.measurement ? scene.measurement->a : sourcePoint,
            scene.measurement ? scene.measurement->b : sourcePoint, false);
    }
    for (const auto& guide : scene.snapGuides)
        if (guide) overlayLine(guide->a, guide->b, true);
    if (!layerScissor.empty() && !scene.textQuads.empty()) {
        const VkRect2D clipped {{layerScissor.x, layerScissor.y},
            {layerScissor.width, layerScissor.height}};
        vkCmdSetScissor(commandBuffer, 0, 1, &clipped);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textQuadPipeline_);
        for (const auto& quad : scene.textQuads) {
            std::array<float, 16> push {};
            for (std::size_t i = 0; i < 4; ++i) {
                const auto p = mapping.documentToFramebuffer(quad.corners[i]);
                push[i * 2] = float(p.x / double(frame.extent.width) * 2 - 1);
                push[i * 2 + 1] = float(p.y / double(frame.extent.height) * 2 - 1);
            }
            push[8] = float(core::srgbToLinear(quad.color.red));
            push[9] = float(core::srgbToLinear(quad.color.green));
            push[10] = float(core::srgbToLinear(quad.color.blue));
            push[11] = float(quad.color.alpha) / 255;
            const auto bounds = mapping.documentCanvasFramebufferRect();
            for (std::size_t i = 0; i < 4; ++i)
                push[12 + i] = float(bounds[i]);
            vkCmdPushConstants(commandBuffer, pipelineLayout_,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push), push.data());
            vkCmdDraw(commandBuffer, 6, 1, 0, 0);
        }
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }
    recordPointerTooltip(scene, frame);
    vkCmdEndRenderPass(commandBuffer);
}

VulkanCanvasRenderer::LayerPushConstants VulkanCanvasRenderer::layerPushConstants(
    const CanvasScene& scene, VkExtent2D targetExtent,
    const core::LayerSnapshot& layer, core::Extent2u layerExtent) const
{
    LayerPushConstants push;
    const CanvasCoordinateMapping mapping(
        {
            static_cast<double>(scene.document.canvas.extent.width),
            static_cast<double>(scene.document.canvas.extent.height),
        },
        scene.logicalViewport,
        {targetExtent.width, targetExtent.height},
        scene.viewport);
    push.clipFromUnit = mapping.layerUnitToVulkanClip(core::renderTransform(layer), layerExtent);
    push.opacity = layer.opacity;
    const auto canvasRect = mapping.documentCanvasFramebufferRect();
    std::transform(canvasRect.begin(), canvasRect.end(), push.canvasRect.begin(),
        [](double value) { return static_cast<float>(value); });
    return push;
}

VulkanCanvasRenderer::CanvasPushConstants VulkanCanvasRenderer::canvasPushConstants(
    const CanvasScene& scene, VkExtent2D targetExtent) const
{
    CanvasPushConstants push;
    const core::Extent2d documentExtent {
        static_cast<double>(scene.document.canvas.extent.width),
        static_cast<double>(scene.document.canvas.extent.height),
    };
    const CanvasCoordinateMapping mapping(documentExtent, scene.logicalViewport,
        {targetExtent.width, targetExtent.height}, scene.viewport);
    const auto canvasRect = mapping.documentCanvasFramebufferRect();
    std::transform(canvasRect.begin(), canvasRect.end(), push.canvasRect.begin(),
        [](double value) { return static_cast<float>(value); });

    const bool quick=scene.activeTool==core::ToolId::SmartSelect && scene.quickSelectionCursor;
    const bool brushCursor = scene.cursorInside && !scene.eyedropperActive && !scene.measureActive
        && (scene.activeTool == core::ToolId::Brush || scene.activeTool == core::ToolId::Eraser
            || scene.activeTool == core::ToolId::Cloning || scene.activeTool==core::ToolId::LocalBlur || quick);
    const auto cursor = brushCursor && !quick && scene.constrainedBrushPosition
        ? mapping.documentToFramebuffer(*scene.constrainedBrushPosition)
        : mapping.logicalViewportToFramebuffer(scene.cursorLogical);
    const auto cursorScale = mapping.logicalViewportToFramebuffer({1.0, 1.0});
    const auto logicalRadius = (quick?scene.quickSelectionSize:scene.brushSizeDocument) * scene.viewport.zoom() * 0.5;
    const auto framebufferRadius = logicalRadius
        * 0.5 * (cursorScale.x + cursorScale.y);
    push.cursorAndFlags = {
        static_cast<float>(cursor.x),
        static_cast<float>(cursor.y),
        static_cast<float>(framebufferRadius),
        brushCursor ? 1.0F : 0.0F,
    };
    push.cursorStyle = {
        static_cast<float>(framebufferRadius * scene.brushTipAspectRatio),
        static_cast<float>(scene.brushHardness),
        static_cast<float>(scene.brushTipAngleDegrees
            * std::numbers::pi / 180.0),
        scene.brushHardness < 0.995 ? 1.0F : 0.0F,
    };
    if(quick)push.cursorStyle={static_cast<float>(framebufferRadius),1,0,0};
    push.pickerStyle = {
        scene.cursorInside && scene.eyedropperActive ? 1.0F : 0.0F,
        scene.eyedropperSampleValid ? 1.0F : 0.0F,
        static_cast<float>(0.5 * (cursorScale.x + cursorScale.y)), 0.0F};
    const auto linearColor = [](core::Rgba8 color) {
        return std::array<float, 4> {static_cast<float>(core::srgbToLinear(color.red)),
            static_cast<float>(core::srgbToLinear(color.green)),
            static_cast<float>(core::srgbToLinear(color.blue)), float(color.alpha) / 255.0F};
    };
    push.pickerReference = linearColor(scene.eyedropperReference);
    push.pickerCandidate = linearColor(scene.eyedropperCandidate);
    return push;
}

} // namespace imageeditor::render
