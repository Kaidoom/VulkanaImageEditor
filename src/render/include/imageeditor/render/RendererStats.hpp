#pragma once

#include <cstdint>
#include <string>

namespace imageeditor::render {

struct RendererStats {
    std::string compositionError;
    std::uint64_t adjustmentParameterUploads {0};
    std::uint64_t adjustmentParameterBytes {0};
    std::uint64_t adjustmentMaskUploads {0};
    std::uint64_t adjustmentMaskBuilds {0};
    std::uint64_t adjustmentMaskBytes {0};
    std::uint64_t adjustmentBufferAllocations {0};
    std::uint64_t adjustmentBufferCapacity {0};
    std::uint64_t compositionPasses {0};
    std::uint64_t compositionDispatches {0};
    std::uint64_t compositionPixels {0};
    std::uint64_t compositionBytes {0};
    std::uint64_t pointerTooltipUploads {0};
    std::uint64_t pointerTooltipUploadedBytes {0};
    std::uint64_t pointerTooltipRasterizations {0};
    std::uint64_t pointerTooltipTextureAllocations {0};
    std::uint64_t selectionGeometryUploads {0};
    std::uint64_t selectionUploadedBytes {0};
    std::uint64_t layerOutlineGeometryUploads {0};
    std::uint64_t layerOutlineUploadedBytes {0};
    std::uint64_t framesSubmitted {0};
    // Successful/suboptimal vkQueuePresentKHR calls, not physical scanout.
    // The upload watermark belongs to that queued frame. The timestamp records
    // the first successful presentation of each changed upload watermark, not
    // subsequent overlay-only frames; opt-in VULKANA_PROFILE_SPOT_HEAL=1.
    std::uint64_t presentQueuedFrames {0};
    std::uint64_t uploadedBytesAtPresent {0};
    std::uint64_t lastUploadPresentQueuedSteadyNanoseconds {0};
    std::uint64_t fullUploads {0};
    std::uint64_t regionalSourceCopies {0};
    std::uint64_t textureCacheBytes {0}, textureCacheEntries {0}, inactiveTextureEvictions {0};
    std::uint64_t regionalUploadBatches {0};
    std::uint64_t regionalDirtyRegions {0};
    std::uint64_t regionalUploads {0};
    std::uint64_t uploadedBytes {0};
    std::uint64_t stagingBufferAllocations {0};
    std::uint64_t stagingCapacityBytes {0};
    std::uint64_t uploadPreparationNanoseconds {0};
    std::uint64_t maximumUploadPreparationNanoseconds {0};
    std::uint64_t lastUploadPreparationNanoseconds {0};
    std::uint64_t lastUploadRegionCount {0};
    std::uint64_t resourceGeneration {0};
    std::uint64_t swapchainGeneration {0};
    std::uint32_t swapchainWidth {0};
    std::uint32_t swapchainHeight {0};
    std::uint64_t deferredResizeEvents {0};
    std::uint64_t resizeCommits {0};
    std::uint64_t resizePresentationSuspends {0};
    std::uint64_t resizePresentationResumes {0};
    std::string deviceName;
};

} // namespace imageeditor::render
