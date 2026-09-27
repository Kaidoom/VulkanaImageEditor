#pragma once

#include <vulkan/vulkan.h>

#include "imageeditor/render/CanvasScene.hpp"
#include "imageeditor/render/RendererStats.hpp"
#include "imageeditor/render/VulkanCanvasRenderer.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace imageeditor::render {

class CanvasWindow;

// Owns Vulkan device and window-system integration resources while relying on
// QVulkanInstance/QPA for the native surface. This keeps platform-specific
// Wayland (and best-effort XCB) details out of application code.
class VulkanCanvasBackend final {
public:
    explicit VulkanCanvasBackend(CanvasWindow& window);
    ~VulkanCanvasBackend();

    VulkanCanvasBackend(const VulkanCanvasBackend&) = delete;
    VulkanCanvasBackend& operator=(const VulkanCanvasBackend&) = delete;

    void invalidateSwapchain() noexcept;
    void surfaceAboutToBeDestroyed();
    void shutdown();

    // Returns true when a follow-up update should be requested, for example
    // after an out-of-date or suboptimal swapchain result.
    [[nodiscard]] bool render(const CanvasScene& scene);
    [[nodiscard]] RendererStats stats() const
    {
        auto result = renderer_.stats();
        result.presentQueuedFrames = presentQueuedFrames_;
        result.uploadedBytesAtPresent = uploadedBytesAtPresent_;
        result.lastUploadPresentQueuedSteadyNanoseconds = lastUploadPresentQueuedSteadyNanoseconds_;
        return result;
    }

private:
    static constexpr std::uint32_t kFrameSlotCount = 2;

    struct FrameSlot {
        VkCommandPool commandPool {VK_NULL_HANDLE};
        VkCommandBuffer commandBuffer {VK_NULL_HANDLE};
        VkSemaphore imageAvailable {VK_NULL_HANDLE};
        VkFence submitted {VK_NULL_HANDLE};
    };

    struct SwapchainImage {
        VkImage image {VK_NULL_HANDLE};
        VkImageView view {VK_NULL_HANDLE};
        VkFramebuffer framebuffer {VK_NULL_HANDLE};
        // Presentation waits are not covered by a frame submission fence.
        // Index this semaphore by acquired swapchain image, not frame slot.
        VkSemaphore renderFinished {VK_NULL_HANDLE};
    };

    struct DeviceCandidate {
        VkPhysicalDevice physicalDevice {VK_NULL_HANDLE};
        VkPhysicalDeviceProperties properties {};
        std::uint32_t graphicsQueueFamily {0};
        std::uint32_t presentQueueFamily {0};
        int score {-1};
    };

    static void checkVk(VkResult result, const char* operation);
    [[nodiscard]] bool ensureDevice();
    [[nodiscard]] bool ensureSwapchain();
    [[nodiscard]] DeviceCandidate choosePhysicalDevice() const;
    void createDevice(const DeviceCandidate& candidate);
    void createFrameResources();
    void createSwapchain();
    void createRenderPass();
    void createSwapchainImages();
    void releaseSwapchain();
    void releaseDevice();
    [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat() const;
    [[nodiscard]] VkExtent2D chooseSurfaceExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    [[nodiscard]] VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(
        const VkSurfaceCapabilitiesKHR& capabilities) const;
    [[nodiscard]] bool hasDeviceExtension(VkPhysicalDevice device, const char* extension) const;
    [[nodiscard]] bool hasUsableSwapchain(VkPhysicalDevice device) const;

    CanvasWindow& window_;
    VulkanCanvasRenderer renderer_;
    VkSurfaceKHR surface_ {VK_NULL_HANDLE}; // Borrowed from the Qt platform window.
    VkPhysicalDevice physicalDevice_ {VK_NULL_HANDLE};
    VkDevice device_ {VK_NULL_HANDLE};
    VkQueue graphicsQueue_ {VK_NULL_HANDLE};
    VkQueue presentQueue_ {VK_NULL_HANDLE};
    std::uint32_t graphicsQueueFamily_ {0};
    std::uint32_t presentQueueFamily_ {0};
    std::array<FrameSlot, kFrameSlotCount> frames_ {};
    std::uint32_t currentFrame_ {0};
    VkSwapchainKHR swapchain_ {VK_NULL_HANDLE};
    VkRenderPass renderPass_ {VK_NULL_HANDLE};
    VkFormat colorFormat_ {VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR colorSpace_ {VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkExtent2D swapchainExtent_ {};
    std::vector<SwapchainImage> swapchainImages_;
    std::vector<VkFence> imageFences_;
    bool swapchainDirty_ {true};
    std::uint64_t presentQueuedFrames_ {0}, uploadedBytesAtPresent_ {0};
    std::uint64_t lastUploadPresentQueuedSteadyNanoseconds_ {0};
    bool profilePresentation_ {false};
};

} // namespace imageeditor::render
