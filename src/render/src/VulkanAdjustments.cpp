#include "imageeditor/render/VulkanCanvasRenderer.hpp"
#include "imageeditor/core/LayerGeometry.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace imageeditor::render {
namespace {
constexpr std::size_t maskBudget = 128U*1024U*1024U;
constexpr std::size_t parameterBudget = 16U*1024U*1024U;
constexpr std::uint32_t uniformFlag = 0x80000000U;
constexpr std::uint32_t tileSize = 128;

std::size_t capacityFor(std::size_t bytes, std::size_t budget)
{
    if (bytes > budget) throw std::length_error("Adjustment GPU data exceeds its bounded allocation budget");
    return std::min(budget,std::max(std::size_t(4096),std::bit_ceil(bytes)));
}
}

void VulkanCanvasRenderer::prepareAdjustmentMasks(const CanvasScene& scene)
{
    std::vector<core::SelectionState> required, referenced;
    const auto appendUnique=[](auto& list,const auto& value) {
        if(value && std::find(list.begin(),list.end(),value)==list.end())list.push_back(value);
    };
    std::unordered_set<core::LayerId> retained;
    for (const auto& layer:scene.document.layersBottomToTop) {
        retained.insert(layer.id);
        if(layer.effectCache)for(const auto& mask:layer.effectCache->masks)if(mask) {
            appendUnique(referenced,mask->coverage);
            if(layer.visible&&layer.opacity>0&&scene.effectBypassLayer!=layer.id
                &&core::hasActiveLayerEffects(layer.effects)&&core::layerEffectCacheValid(layer))
                appendUnique(required,mask->coverage);
        }
        if (!layer.adjustments) continue;
        for (const auto& item:layer.adjustments->items) {
            if (!item.mask) continue;
            appendUnique(referenced,item.mask->coverage);
        }
        if(!layer.visible || !std::isfinite(layer.opacity) || layer.opacity<=0
            || scene.adjustmentBypassLayer==layer.id
            || (scene.filterBypassLayer!=layer.id && core::layerSpatialFilterCacheValid(layer)))continue;
        auto& program=adjustmentPrograms_[layer.id];
        if(program.state!=layer.adjustments) {
            program.compiled=core::compileAdjustmentStack(layer.adjustments);
            program.state=layer.adjustments;
        }
        for(std::size_t i=0;i<core::adjustmentCount;++i) {
            if(program.compiled.parameters[i*core::adjustmentParameterStride]!=0 && program.compiled.masks[i])
                appendUnique(required,program.compiled.masks[i]->coverage);
        }
    }
    const auto byRevision=[](const auto& a,const auto& b){return a->revision()<b->revision();};
    std::sort(required.begin(),required.end(),byRevision);
    std::vector<core::Revision> requiredRevisions;
    for(const auto& mask:required)requiredRevisions.push_back(mask->revision());
    if(!requiredRevisions.empty() && requiredRevisions==failedAdjustmentMaskRevisions_)
        throw std::length_error("Active adjustment masks exceed the 128 MiB GPU budget");
    auto masks=required;
    // Load only masks actually needed for rendering. Keep previously resident
    // masks while referenced so Before/disable/hide does not churn resources;
    // evict unused residents if admitting newly active masks needs the budget.
    for(const auto& mask:adjustmentMasks_)
        if(std::find(referenced.begin(),referenced.end(),mask)!=referenced.end())appendUnique(masks,mask);
    std::sort(masks.begin(),masks.end(),[](const auto& a,const auto& b) { return a->revision()<b->revision(); });
    std::erase_if(adjustmentPrograms_,[&](const auto& pair) { return !retained.contains(pair.first); });
    if (masks==adjustmentMasks_) return;
    const auto fitsBudget=[&](const auto& candidates) {
        std::size_t bytes=4;
        for(const auto& mask:candidates) {
            const auto e=mask->extent();
            const auto columns=(e.width+tileSize-1)/tileSize,rows=(e.height+tileSize-1)/tileSize;
            bytes+=16+std::size_t(columns)*rows*4;
            if(bytes>maskBudget)return false;
            for(std::uint32_t row=0;row<rows;++row)for(std::uint32_t col=0;col<columns;++col) {
                const auto x=col*tileSize,y=row*tileSize;
                if(!mask->constantCoverage({int(x),int(y),int(std::min(tileSize,e.width-x)),int(std::min(tileSize,e.height-y))}))
                    bytes+=tileSize*tileSize;
                if(bytes>maskBudget)return false;
            }
        }
        return true;
    };
    // Preflight compressed size from tile metadata before pixel scans or
    // allocation. A large rejected mask set costs O(tiles), not O(pixels).
    if(!fitsBudget(masks)) {
        masks=required;
        if(!fitsBudget(masks)) {
            failedAdjustmentMaskRevisions_=std::move(requiredRevisions);
            throw std::length_error("Active adjustment masks exceed the 128 MiB GPU budget");
        }
    }
    // Build a replacement entirely before publishing. Uniform tiles (including
    // empty/solid selections) occupy one word; only varying R8 tiles use bytes.
    std::vector<std::uint32_t> atlas, offsets;
    const auto build=[&] {
      ++stats_.adjustmentMaskBuilds;
      atlas={0};offsets.clear();
      for (const auto& mask:masks) {
        const auto extent=mask->extent();
        const auto columns=(extent.width+tileSize-1)/tileSize;
        const auto rows=(extent.height+tileSize-1)/tileSize;
        const auto header=atlas.size();
        const auto count=std::size_t(columns)*rows;
        if ((header+4+count)*4 > maskBudget) throw std::length_error("Adjustment mask index exceeds 128 MiB");
        offsets.push_back(std::uint32_t(header));
        atlas.insert(atlas.end(),{extent.width,extent.height,columns,0});
        atlas.resize(header+4+count,uniformFlag);
        const auto bounds=mask->bounds();
        for (std::uint32_t row=0;row<rows;++row) for (std::uint32_t col=0;col<columns;++col) {
            const auto x=col*tileSize, y=row*tileSize;
            const core::RectI rect {int(x),int(y),int(std::min(tileSize,extent.width-x)),int(std::min(tileSize,extent.height-y))};
            if (rect.clippedTo(bounds).empty()) continue;
            const auto index=header+4+std::size_t(row)*columns+col;
            if(const auto uniform=mask->constantCoverage(rect)) {
                atlas[index]=uniformFlag|*uniform;
                continue;
            }
            std::array<std::uint32_t,tileSize*tileSize/4> words {};
            const auto first=mask->coverageAtDocumentPixel(int(x),int(y));
            bool uniform=true;
            for (int iy=0;iy<rect.height;++iy) for (int ix=0;ix<rect.width;++ix) {
                const auto value=mask->coverageAtDocumentPixel(int(x)+ix,int(y)+iy);
                uniform &= value==first;
                const auto p=std::size_t(iy)*tileSize+std::size_t(ix);
                words[p/4] |= std::uint32_t(value)<<((p%4)*8);
            }
            if (uniform) atlas[index]=uniformFlag|first;
            else {
                if ((atlas.size()+words.size())*4 > maskBudget)
                    throw std::length_error("Active adjustment masks exceed the 128 MiB GPU budget");
                atlas[index]=std::uint32_t(atlas.size());
                atlas.insert(atlas.end(),words.begin(),words.end());
            }
        }
      }
    };
    build();
    failedAdjustmentMaskRevisions_.clear();
    adjustmentMasks_=std::move(masks); adjustmentMaskOffsets_=std::move(offsets);
    adjustmentMaskAtlas_=std::move(atlas); ++adjustmentMaskGeneration_;
}

std::int32_t VulkanCanvasRenderer::appendAdjustmentParameters(const core::LayerSnapshot& layer,
    bool bypass,std::vector<float>& values)
{
    if (bypass || !layer.adjustments) return -1;
    auto& program=adjustmentPrograms_[layer.id];
    if (program.state!=layer.adjustments) {
        program.compiled=core::compileAdjustmentStack(layer.adjustments);
        program.state=layer.adjustments;
    }
    if (!program.compiled.active) return -1;
    const auto offset=values.size();
    if ((offset+adjustmentGpuStride)*sizeof(float)>parameterBudget)
        throw std::length_error("Adjustment parameters exceed the 16 MiB per-frame budget");
    values.resize(offset+adjustmentGpuStride,0);
    std::copy(program.compiled.parameters.begin(),program.compiled.parameters.end(),values.begin()+std::ptrdiff_t(offset));
    const auto meta=offset+core::adjustmentCount*core::adjustmentParameterStride;
    const auto transform=core::intrinsicPixelsToLocal(layer);
    const auto writeRows=[&](std::size_t i,const core::AffineTransform& t) {
        values[i]=float(t.m00); values[i+1]=float(t.m01); values[i+2]=float(t.m02);
        values[i+4]=float(t.m10); values[i+5]=float(t.m11); values[i+6]=float(t.m12);
        values[i+8]=float(t.m20); values[i+9]=float(t.m21); values[i+10]=float(t.m22);
    };
    writeRows(meta,transform);
    for (std::size_t i=0;i<core::adjustmentCount;++i) {
        const auto& mask=program.compiled.masks[i];
        if (!mask || program.compiled.parameters[i*core::adjustmentParameterStride]==0) continue;
        const auto found=std::find(adjustmentMasks_.begin(),adjustmentMasks_.end(),mask->coverage);
        if (found==adjustmentMasks_.end()) throw std::runtime_error("Missing captured adjustment mask");
        const auto index=meta+12+i*12;
        writeRows(index,mask->localToMask);
        values[index+3]=std::bit_cast<float>(adjustmentMaskOffsets_[std::size_t(found-adjustmentMasks_.begin())]);
    }
    return std::int32_t(offset);
}

std::int32_t VulkanCanvasRenderer::appendEffectParameters(const core::LayerSnapshot& layer,std::vector<float>& values)
{
    const auto offset=values.size();
    if((offset+core::effectParameterCount+48)*sizeof(float)>parameterBudget)
        throw std::length_error("Layer effect parameters exceed GPU budget");
    const auto program=core::compileLayerEffects(layer.effects,core::layerEffectReferenceFrame(layer));
    values.insert(values.end(),program.begin(),program.end());values.resize(values.size()+48,0);
    for(std::size_t i=0;i<6;++i)if(const auto& mask=layer.effectCache->masks[i]) {
        const auto found=std::find(adjustmentMasks_.begin(),adjustmentMasks_.end(),mask->coverage);
        if(found==adjustmentMasks_.end())throw std::runtime_error("Missing prepared layer effect mask");
        const auto n=offset+core::effectParameterCount+i*8;const auto& t=mask->localToMask;
        values[n]=float(t.m00);values[n+1]=float(t.m01);values[n+2]=float(t.m02);
        values[n+4]=float(t.m10);values[n+5]=float(t.m11);values[n+6]=float(t.m12);
        values[n+3]=std::bit_cast<float>(adjustmentMaskOffsets_[std::size_t(found-adjustmentMasks_.begin())]);
    }
    return std::int32_t(offset);
}

VkDescriptorSet VulkanCanvasRenderer::uploadAdjustmentData(std::span<const float> data,const VulkanFrameContext& frame)
{
    auto& resources=adjustmentBuffers_.at(frame.frameSlot);
    constexpr std::array<float,1> dummy {};
    if (data.empty()) data=dummy;
    bool descriptorsChanged=false;
    const auto resize=[&](BufferResource& buffer,std::size_t bytes,std::size_t budget,bool mapped) {
        if (buffer.size>=bytes) return false;
        // The host has waited for this frame slot's fence; other slots own
        // separate buffers/descriptors, so no in-flight resource is modified.
        if (mapped && resources.mappedParameters) {
            vkUnmapMemory(device_,buffer.memory); resources.mappedParameters=nullptr;
        }
        stats_.adjustmentBufferCapacity-=buffer.size;
        destroyBuffer(buffer);
        buffer=createBuffer(capacityFor(bytes,budget),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | (mapped ? VkBufferUsageFlags(0) : VkBufferUsageFlags(VK_BUFFER_USAGE_TRANSFER_DST_BIT)), mapped
                ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        stats_.adjustmentBufferCapacity+=buffer.size; ++stats_.adjustmentBufferAllocations;
        if (mapped) {
            void* address=nullptr;
            checkVk(vkMapMemory(device_,buffer.memory,0,VK_WHOLE_SIZE,0,&address),"map adjustment parameters");
            resources.mappedParameters=static_cast<float*>(address);
        }
        descriptorsChanged=true;
        return true;
    };
    const bool newParameters=resize(resources.parameters,data.size_bytes(),parameterBudget,true);
    if (newParameters || resources.parameterKey.size()!=data.size()
        || !std::equal(data.begin(),data.end(),resources.parameterKey.begin())) {
        std::memcpy(resources.mappedParameters,data.data(),data.size_bytes());
        resources.parameterKey.assign(data.begin(),data.end());
        ++stats_.adjustmentParameterUploads; stats_.adjustmentParameterBytes+=data.size_bytes();
    }
    const auto maskBytes=adjustmentMaskAtlas_.size()*sizeof(std::uint32_t);
    const bool hasMasks=maskBytes>sizeof(std::uint32_t);
    const bool newMasks=hasMasks&&resize(resources.masks,maskBytes,maskBudget,false);
    if (hasMasks&&(newMasks || resources.maskGeneration!=adjustmentMaskGeneration_)) {
        const auto staging=reserveStaging(frame.frameSlot,maskBytes);
        std::memcpy(staging.bytes.data(),adjustmentMaskAtlas_.data(),maskBytes);
        const VkBufferCopy copy {staging.offset,0,maskBytes};
        vkCmdCopyBuffer(frame.commandBuffer,staging.buffer,resources.masks.buffer,1,&copy);
        VkBufferMemoryBarrier barrier {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer=resources.masks.buffer; barrier.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(frame.commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,0,nullptr,1,&barrier,0,nullptr);
        resources.maskGeneration=adjustmentMaskGeneration_;
        ++stats_.adjustmentMaskUploads; stats_.adjustmentMaskBytes+=maskBytes;
    }
    if (!resources.descriptor) {
        VkDescriptorSetAllocateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool=descriptorPool_; info.descriptorSetCount=1; info.pSetLayouts=&adjustmentSetLayout_;
        checkVk(vkAllocateDescriptorSets(device_,&info,&resources.descriptor),"allocate adjustment descriptor");
        descriptorsChanged=true;
    }
    if (descriptorsChanged) {
        // With no captured masks the shader never reads this binding. Alias
        // the already-valid parameter buffer instead of staging/uploading a
        // dummy word in each slot of an otherwise unadjusted document.
        const auto& masks=resources.masks.buffer?resources.masks:resources.parameters;
        const std::array infos {VkDescriptorBufferInfo {resources.parameters.buffer,0,resources.parameters.size},
            VkDescriptorBufferInfo {masks.buffer,0,masks.size}};
        std::array<VkWriteDescriptorSet,2> writes {};
        for (std::size_t i=0;i<writes.size();++i) {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet=resources.descriptor; writes[i].dstBinding=std::uint32_t(i);
            writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device_,std::uint32_t(writes.size()),writes.data(),0,nullptr);
    }
    return resources.descriptor;
}

void VulkanCanvasRenderer::releaseAdjustments()
{
    for (auto& resources:adjustmentBuffers_) {
        if (resources.descriptor) vkFreeDescriptorSets(device_,descriptorPool_,1,&resources.descriptor);
        if (resources.mappedParameters) vkUnmapMemory(device_,resources.parameters.memory);
        destroyBuffer(resources.parameters); destroyBuffer(resources.masks);
    }
    adjustmentBuffers_.clear(); adjustmentPrograms_.clear(); adjustmentMasks_.clear();
    failedAdjustmentMaskRevisions_.clear();
    adjustmentMaskAtlas_={0}; adjustmentMaskOffsets_.clear(); adjustmentMaskGeneration_=1;
    stats_.adjustmentBufferCapacity=0;
    if (adjustmentSetLayout_) vkDestroyDescriptorSetLayout(device_,adjustmentSetLayout_,nullptr);
    adjustmentSetLayout_=VK_NULL_HANDLE;
}
}
