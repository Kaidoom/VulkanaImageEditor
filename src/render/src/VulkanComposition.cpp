#include "imageeditor/render/VulkanCanvasRenderer.hpp"
#include "imageeditor/render/CanvasCoordinateMapping.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::render {
namespace {
struct alignas(16) CompositePush {
    std::array<float,4> localX, localY, canvasRect;
    std::array<std::int32_t,4> region;
    float opacity;
    std::int32_t mode;
    std::int32_t adjustmentOffset {-1};
    std::int32_t cropMode {0}; // 0 unrestricted, 1 crop, 2 editor-only retained-source preview.
    std::array<float,4> sourceToLocalX,sourceToLocalY,cropRect;
};
static_assert(sizeof(CompositePush) == 128); // Vulkan's guaranteed minimum push-constant budget.
constexpr auto shaderStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
}

void VulkanCanvasRenderer::initializeComposition()
{
    // rgba32f is a core storage-image format, not a driver blend extension or
    // an optional extended-format feature. Verify sampled/storage support.
    VkFormatProperties properties {};
    vkGetPhysicalDeviceFormatProperties(physicalDevice_,VK_FORMAT_R32G32B32A32_SFLOAT,&properties);
    constexpr auto needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed)
        throw std::runtime_error("Vulkan device lacks RGBA32F sampled/storage images required by the compositor");
    VkDescriptorSetLayoutBinding binding {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 1; setInfo.pBindings = &binding;
    checkVk(vkCreateDescriptorSetLayout(device_,&setInfo,nullptr,&compositionSetLayout_),"create compositor image layout");
    const std::array adjustmentBindings {
        VkDescriptorSetLayoutBinding {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        VkDescriptorSetLayoutBinding {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    setInfo.bindingCount=std::uint32_t(adjustmentBindings.size()); setInfo.pBindings=adjustmentBindings.data();
    checkVk(vkCreateDescriptorSetLayout(device_,&setInfo,nullptr,&adjustmentSetLayout_),"create adjustment buffer layout");
    const std::array sets {descriptorSetLayout_,compositionSetLayout_,adjustmentSetLayout_};
    const VkPushConstantRange push {VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(CompositePush)};
    VkPipelineLayoutCreateInfo layout {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = std::uint32_t(sets.size()); layout.pSetLayouts = sets.data();
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    checkVk(vkCreatePipelineLayout(device_,&layout,nullptr,&compositionLayout_),"create compositor pipeline layout");
    const auto shader = loadShader("composite.comp.spv");
    VkComputePipelineCreateInfo pipeline {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
        VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
    pipeline.layout = compositionLayout_;
    const auto result = vkCreateComputePipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&compositionPipeline_);
    vkDestroyShaderModule(device_,shader,nullptr);
    checkVk(result,"create compositor pipeline");
}

void VulkanCanvasRenderer::createCompositionTarget()
{
    // One viewport-sized image, shared on the ordered graphics+compute queue,
    // not one allocation per layer/frame slot. ~127 MiB at 4K, ~225 MiB at 5K.
    const auto bytes = std::uint64_t(targetExtent_.width)*targetExtent_.height*16;
    if (!bytes || bytes > 512ULL*1024*1024)
        throw std::runtime_error("Canvas composition target exceeds the 512 MiB viewport budget");
    compositionTexture_ = createTexture({targetExtent_.width,targetExtent_.height},
        VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_STORAGE_BIT);
    VkDescriptorSetAllocateInfo allocation {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = descriptorPool_; allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &compositionSetLayout_;
    checkVk(vkAllocateDescriptorSets(device_,&allocation,&compositionStorageSet_),"allocate compositor storage descriptor");
    VkDescriptorImageInfo image {VK_NULL_HANDLE,compositionTexture_.view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = compositionStorageSet_; write.dstBinding = 0; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
    compositionKey_.clear(); stats_.compositionBytes = bytes;
}
void VulkanCanvasRenderer::releaseCompositionTarget()
{
    // Host waits for device idle before releasing swapchain/device resources.
    if (compositionStorageSet_) vkFreeDescriptorSets(device_,descriptorPool_,1,&compositionStorageSet_);
    compositionStorageSet_ = VK_NULL_HANDLE;
    destroyTexture(compositionTexture_);
    compositionKey_.clear(); stats_.compositionBytes = 0;
}

void VulkanCanvasRenderer::recordComposition(const CanvasScene& scene, const VulkanFrameContext& frame,bool unavailable)
{
    const core::Extent2d extent {double(scene.document.canvas.extent.width),double(scene.document.canvas.extent.height)};
    const CanvasCoordinateMapping mapping(extent,scene.logicalViewport,{frame.extent.width,frame.extent.height},scene.viewport);
    const auto rect = mapping.documentCanvasFramebufferRect();
    const auto clip = mapping.documentCanvasFramebufferScissor();
    // Exact metadata key (no hash collisions): no raster copies, no dependence
    // on selection, caret, tooltips, names or theme. Overlay-only frames reuse
    // composition; raster uploads retain their existing dirty-region path.
    std::vector<std::uint64_t> key;
    key.push_back(scene.documentInstance);
    key.reserve(8+scene.document.layersBottomToTop.size()*12);
    key.push_back(frame.extent.width); key.push_back(frame.extent.height);
    for (double v : rect) key.push_back(std::bit_cast<std::uint64_t>(v));
    struct Work { CompositePush push; VkDescriptorSet texture; };
    std::vector<Work> work;
    std::vector<float> adjustmentParameters;
    if(!unavailable && !scene.pixelPreviewEnabled)prepareAdjustmentMasks(scene);
    // This replaces only composition input, never the editor's document scene.
    core::LayerSnapshot native;
    native.payload=core::RasterLayerSnapshot{scene.pixelPreview};
    const auto layers=scene.pixelPreviewEnabled
        ? std::span<const core::LayerSnapshot>(&native,scene.pixelPreview && !unavailable?1:0)
        : std::span<const core::LayerSnapshot>(scene.document.layersBottomToTop);
    key.push_back(scene.pixelPreviewEnabled);
    for (const auto& layer : layers) {
        if (!layer.visible || !std::isfinite(layer.opacity) || layer.opacity <= 0) continue;
        const bool bypassFilters=scene.filterBypassLayer==layer.id || scene.adjustmentBypassLayer==layer.id;
        const bool filtered=!bypassFilters && core::layerSpatialFilterCacheValid(layer);
        const bool styled=scene.effectBypassLayer!=layer.id&&!bypassFilters
            &&core::hasActiveLayerEffects(layer.effects)&&core::layerEffectCacheValid(layer);
        const auto surface = bypassFilters?core::intrinsicSurface(layer):core::renderedSurface(layer);
        if (!surface || surface->extent().empty()) continue;
        const auto found = textures_.find(surface->id());
        if (found == textures_.end()) continue;
        const auto t = bypassFilters?core::composeAffine(layer.localToDocument,core::intrinsicPixelsToLocal(layer)):core::renderTransform(layer);
        const auto o = mapping.documentToFramebuffer({0,0});
        const auto x = mapping.documentToFramebuffer({1,0})-o;
        const auto y = mapping.documentToFramebuffer({0,1})-o;
        const auto toFrame = core::composeTransform({x.x,y.x,o.x,x.y,y.y,o.y},t);
        const auto inverse = toFrame.inverted();
        if (!inverse) continue;
        key.push_back(layer.id);
        key.push_back(surface->id()); key.push_back(surface->revision());
        key.push_back(static_cast<std::uint32_t>(layer.blendMode));
        key.push_back(std::bit_cast<std::uint32_t>(layer.opacity));
        key.push_back(layer.adjustmentRevision);
        key.push_back(layer.effectRevision);key.push_back(styled);
        if(styled)for(const auto& mask:layer.effectCache->masks)key.push_back(mask?mask->coverage->revision():0);
        key.push_back(bypassFilters);
        key.push_back(scene.adjustmentBypassLayer==layer.id);
        key.push_back(layer.crop.has_value());
        key.push_back(scene.cropPreviewLayer==layer.id);
        if(layer.crop)for(double v:{layer.crop->x,layer.crop->y,layer.crop->width,layer.crop->height})
            key.push_back(std::bit_cast<std::uint64_t>(v));
        if(layer.crop)for(double v:layer.crop->corners)key.push_back(std::bit_cast<std::uint64_t>(v));
        for (double v : {toFrame.m00,toFrame.m01,toFrame.m02,toFrame.m10,toFrame.m11,toFrame.m12,toFrame.m20,toFrame.m21,toFrame.m22})
            key.push_back(std::bit_cast<std::uint64_t>(v));
        if (clip.empty()) continue;
        const auto e = surface->extent();
        if(!toFrame.validOver({0,0,double(e.width),double(e.height)}))continue;
        double left = double(frame.extent.width), top = double(frame.extent.height), right = 0, bottom = 0;
        for (const auto p : std::array {toFrame.map({0,0}),toFrame.map({double(e.width),0}),
                 toFrame.map({0,double(e.height)}),toFrame.map({double(e.width),double(e.height)})}) {
            left = std::min(left,p.x); top = std::min(top,p.y);
            right = std::max(right,p.x); bottom = std::max(bottom,p.y);
        }
        if(!toFrame.isAffine()){left-=1;top-=1;right+=1;bottom+=1;}
        if(styled) {
            const auto v=layer.effectCache->visualBounds;
            for(auto local:std::array{core::Vec2d{v.x,v.y},core::Vec2d{v.right(),v.y},core::Vec2d{v.right(),v.bottom()},core::Vec2d{v.x,v.bottom()}}) {
                const auto p=mapping.documentToFramebuffer(layer.localToDocument.map(local));
                left=std::min(left,p.x);top=std::min(top,p.y);right=std::max(right,p.x);bottom=std::max(bottom,p.y);
            }
        }
        if(layer.crop && scene.cropPreviewLayer!=layer.id){
            if(layer.crop->empty())continue;
            const auto& c=*layer.crop;
            double cropLeft=std::numeric_limits<double>::max(),cropTop=cropLeft;
            double cropRight=-cropLeft,cropBottom=-cropLeft;
            for(auto local:std::array{core::Vec2d{c.x,c.y},core::Vec2d{c.right(),c.y},
                    core::Vec2d{c.right(),c.bottom()},core::Vec2d{c.x,c.bottom()}}){
                const auto p=mapping.documentToFramebuffer(layer.localToDocument.map(local));
                cropLeft=std::min(cropLeft,p.x);cropTop=std::min(cropTop,p.y);
                cropRight=std::max(cropRight,p.x);cropBottom=std::max(cropBottom,p.y);
            }
            // Conservative one-pixel margin keeps analytic AA intact. The
            // shader still evaluates the exact rotated/sheared intersection.
            left=std::max(left,cropLeft-1);top=std::max(top,cropTop-1);
            right=std::min(right,cropRight+1);bottom=std::min(bottom,cropBottom+1);
        }
        left = std::clamp(std::floor(left),double(clip.x),double(clip.x)+clip.width);
        top = std::clamp(std::floor(top),double(clip.y),double(clip.y)+clip.height);
        right = std::clamp(std::ceil(right),double(clip.x),double(clip.x)+clip.width);
        bottom = std::clamp(std::ceil(bottom),double(clip.y),double(clip.y)+clip.height);
        if (right <= left || bottom <= top) continue;
        CompositePush push {
            {float(inverse->m00),float(inverse->m01),float(inverse->m02),0},
            {float(inverse->m10),float(inverse->m11),float(inverse->m12),0},
            {float(rect[0]),float(rect[1]),float(rect[2]),float(rect[3])},
            {std::int32_t(left),std::int32_t(top),std::int32_t(right-left),std::int32_t(bottom-top)},
            layer.opacity,static_cast<std::int32_t>(layer.blendMode),
            appendAdjustmentParameters(layer,filtered||scene.adjustmentBypassLayer==layer.id,adjustmentParameters),layer.crop?(scene.cropPreviewLayer==layer.id?2:1):0,{},{},{}};
        if(styled)push.mode|=(appendEffectParameters(layer,adjustmentParameters)+1)<<8;
        const auto pixelsToLocal=bypassFilters?core::intrinsicPixelsToLocal(layer):core::renderPixelsToLocal(layer);
        if(layer.crop||styled){
            push.sourceToLocalX={float(pixelsToLocal.m00),float(pixelsToLocal.m01),float(pixelsToLocal.m02),0};
            push.sourceToLocalY={float(pixelsToLocal.m10),float(pixelsToLocal.m11),float(pixelsToLocal.m12),0};
        }
        if(layer.crop){
            push.cropRect={float(layer.crop->x),float(layer.crop->y),float(layer.crop->right()),float(layer.crop->bottom())};
            const auto cuts=layer.crop->resolvedCorners();
            // Four unused affine-row lanes keep the push block within 128 B.
            push.localX[3]=float(cuts[0]);push.localY[3]=float(cuts[1]);
            push.sourceToLocalX[3]=float(cuts[2]);push.sourceToLocalY[3]=float(cuts[3]);
        }
        if(scene.pixelPreviewEnabled)push.cropMode=3; // native-pixel display sampling
        if(!inverse->isAffine()||!pixelsToLocal.isAffine()) {
            // Bottom rows live in the existing parameter buffer, preserving
            // the guaranteed 128-byte push-constant budget. Affine dispatches
            // keep their original arithmetic and avoid the extra lookup.
            const auto offset=adjustmentParameters.size();
            adjustmentParameters.insert(adjustmentParameters.end(),{float(inverse->m20),float(inverse->m21),float(inverse->m22),0,
                float(pixelsToLocal.m20),float(pixelsToLocal.m21),float(pixelsToLocal.m22),0});
            push.cropMode|=int(offset+1)<<2;
        }
        work.push_back({push,found->second.descriptorSet});
    }
    if (compositionTexture_.uploadedRevision && key == compositionKey_) return;
    const auto adjustmentSet=uploadAdjustmentData(adjustmentParameters,frame);
    const auto cmd = frame.commandBuffer;
    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = compositionTexture_.image;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    barrier.oldLayout = compositionTexture_.uploadedRevision ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = compositionTexture_.uploadedRevision ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,compositionTexture_.uploadedRevision ? shaderStages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    const VkClearColorValue transparent {{0,0,0,0}};
    vkCmdClearColorImage(cmd,compositionTexture_.image,VK_IMAGE_LAYOUT_GENERAL,&transparent,1,&barrier.subresourceRange);
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,shaderStages,0,0,nullptr,0,nullptr,1,&barrier);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compositionPipeline_);
    for (const auto& entry : work) {
        const std::array descriptors {entry.texture,compositionStorageSet_,adjustmentSet};
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compositionLayout_,0,
            std::uint32_t(descriptors.size()),descriptors.data(),0,nullptr);
        vkCmdPushConstants(cmd,compositionLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(CompositePush),&entry.push);
        vkCmdDispatch(cmd,(std::uint32_t(entry.push.region[2])+15)/16,(std::uint32_t(entry.push.region[3])+15)/16,1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,0,nullptr,0,nullptr,1,&barrier);
        ++stats_.compositionDispatches;
        stats_.compositionPixels += std::uint64_t(entry.push.region[2])*std::uint64_t(entry.push.region[3]);
    }
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    compositionTexture_.uploadedRevision = 1;
    compositionKey_ = std::move(key);
    ++stats_.compositionPasses;
}
}
