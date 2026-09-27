#include "VulkanCanvasBackend.hpp"

#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/render/detail/VulkanEnumeration.hpp"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QVulkanInstance>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

Q_LOGGING_CATEGORY(logVulkanHost, "imageeditor.vulkan.host")

namespace imageeditor::render {

namespace {

void logEnumerationRetries(const char* subject, std::uint32_t attempts)
{
    if (attempts > 1) {
        qCWarning(logVulkanHost) << subject << "required" << attempts
                                 << "enumeration attempts";
    }
}

} // namespace

VulkanCanvasBackend::VulkanCanvasBackend(CanvasWindow& window)
    : window_(window)
    , profilePresentation_(qEnvironmentVariableIntValue("VULKANA_PROFILE_SPOT_HEAL") > 0)
{
}

VulkanCanvasBackend::~VulkanCanvasBackend()
{
    shutdown();
}

void VulkanCanvasBackend::invalidateSwapchain() noexcept
{
    swapchainDirty_ = true;
}

void VulkanCanvasBackend::surfaceAboutToBeDestroyed()
{
    // The native surface belongs to Qt's platform window. All objects that
    // reference it must be gone before QPA destroys that surface.
    shutdown();
}

void VulkanCanvasBackend::shutdown()
{
    releaseDevice();
    surface_ = VK_NULL_HANDLE;
    swapchainDirty_ = true;
}

bool VulkanCanvasBackend::render(const CanvasScene& scene)
{
    if (!ensureDevice() || !ensureSwapchain()) {
        return false;
    }

    auto& frame = frames_[currentFrame_];
    checkVk(vkWaitForFences(device_, 1, &frame.submitted, VK_TRUE, UINT64_MAX),
        "wait for canvas frame");

    std::uint32_t imageIndex = 0;
    const auto acquireResult = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
        frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        return true;
    }
    if (acquireResult == VK_ERROR_SURFACE_LOST_KHR) {
        qCWarning(logVulkanHost) << "Canvas surface was lost during image acquisition";
        shutdown();
        return false;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        qCWarning(logVulkanHost) << "Unable to acquire a canvas image:" << acquireResult;
        shutdown();
        return false;
    }
    if (imageIndex >= swapchainImages_.size()) {
        qFatal("Vulkan returned an invalid swapchain image index");
    }

    const auto priorImageFence = imageFences_[imageIndex];
    if (priorImageFence != VK_NULL_HANDLE && priorImageFence != frame.submitted) {
        checkVk(vkWaitForFences(device_, 1, &priorImageFence, VK_TRUE, UINT64_MAX),
            "wait for acquired swapchain image");
    }
    imageFences_[imageIndex] = frame.submitted;

    checkVk(vkResetCommandPool(device_, frame.commandPool, 0), "reset canvas command pool");
    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "begin canvas command buffer");
    renderer_.recordFrame(scene, VulkanFrameContext {
        .commandBuffer = frame.commandBuffer,
        .framebuffer = swapchainImages_[imageIndex].framebuffer,
        .extent = swapchainExtent_,
        .frameSlot = currentFrame_,
    });
    checkVk(vkEndCommandBuffer(frame.commandBuffer), "end canvas command buffer");

    // Reset only after acquisition and command recording succeeded. Resetting
    // before an out-of-date acquire would leave this slot permanently waiting
    // on an unsignaled fence.
    checkVk(vkResetFences(device_, 1, &frame.submitted), "reset canvas frame fence");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const auto renderFinished = swapchainImages_[imageIndex].renderFinished;
    VkSubmitInfo submitInfo {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinished;
    checkVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frame.submitted),
        "submit canvas frame");

    VkPresentInfoKHR presentInfo {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    auto* instance = window_.vulkanInstance();
    instance->presentAboutToBeQueued(&window_);
    const auto presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
    if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR) {
        instance->presentQueued(&window_);
        ++presentQueuedFrames_;
        const auto uploadedBytes = renderer_.stats().uploadedBytes;
        if (profilePresentation_ && uploadedBytesAtPresent_ != uploadedBytes)
            lastUploadPresentQueuedSteadyNanoseconds_ = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        uploadedBytesAtPresent_ = uploadedBytes;
    }

    currentFrame_ = (currentFrame_ + 1U) % kFrameSlotCount;
    if (acquireResult == VK_SUBOPTIMAL_KHR || presentResult == VK_SUBOPTIMAL_KHR
        || presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        return true;
    }
    if (presentResult == VK_ERROR_SURFACE_LOST_KHR) {
        qCWarning(logVulkanHost) << "Canvas surface was lost during presentation";
        shutdown();
        return false;
    }
    if (presentResult != VK_SUCCESS) {
        qCWarning(logVulkanHost) << "Unable to present the canvas:" << presentResult;
        shutdown();
    }
    return false;
}

void VulkanCanvasBackend::checkVk(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        qFatal("Vulkan failed to %s (VkResult %d)", operation, static_cast<int>(result));
    }
}

bool VulkanCanvasBackend::ensureDevice()
{
    if (device_ != VK_NULL_HANDLE) {
        return true;
    }
    auto* instance = window_.vulkanInstance();
    if (!instance || !instance->isValid()) {
        qCWarning(logVulkanHost) << "Canvas has no valid QVulkanInstance";
        return false;
    }
    surface_ = QVulkanInstance::surfaceForWindow(&window_);
    if (surface_ == VK_NULL_HANDLE) {
        qCWarning(logVulkanHost) << "Qt has not created the canvas Vulkan surface yet";
        return false;
    }

    const auto candidate = choosePhysicalDevice();
    if (candidate.physicalDevice == VK_NULL_HANDLE) {
        qFatal("No Vulkan device can render and present the canvas");
    }
    createDevice(candidate);
    createFrameResources();
    renderer_.initializeDevice(VulkanDeviceContext {
        .physicalDevice = physicalDevice_,
        .device = device_,
        .properties = candidate.properties,
        .frameSlotCount = kFrameSlotCount,
    });
    return true;
}

bool VulkanCanvasBackend::ensureSwapchain()
{
    if (swapchain_ != VK_NULL_HANDLE && !swapchainDirty_) {
        return true;
    }
    releaseSwapchain();
    createSwapchain();
    return swapchain_ != VK_NULL_HANDLE;
}

VulkanCanvasBackend::DeviceCandidate VulkanCanvasBackend::choosePhysicalDevice() const
{
    const auto instance = window_.vulkanInstance()->vkInstance();
    auto deviceEnumeration = detail::enumerateVulkanValues<VkPhysicalDevice>(
        [instance](auto* count, auto* devices) {
            return vkEnumeratePhysicalDevices(instance, count, devices);
        });
    logEnumerationRetries("Vulkan physical devices", deviceEnumeration.attempts);
    checkVk(deviceEnumeration.result, "enumerate Vulkan devices");
    const auto& devices = deviceEnumeration.values;

    DeviceCandidate best;
    for (const auto device : devices) {
        if (!hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)
            || !hasUsableSwapchain(device)) {
            continue;
        }

        auto queueEnumeration = detail::enumerateVulkanProperties<VkQueueFamilyProperties>(
            [device](auto* count, auto* queues) {
                vkGetPhysicalDeviceQueueFamilyProperties(device, count, queues);
            });
        logEnumerationRetries("Vulkan queue families", queueEnumeration.attempts);
        if (!queueEnumeration.complete) {
            qFatal("Vulkan queue-family enumeration did not stabilize");
        }
        const auto& queues = queueEnumeration.values;

        auto graphicsFamily = std::numeric_limits<std::uint32_t>::max();
        auto presentFamily = std::numeric_limits<std::uint32_t>::max();
        for (std::uint32_t index = 0;
             index < static_cast<std::uint32_t>(queues.size()); ++index) {
            const auto required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            const bool graphics = (queues[index].queueFlags & required) == required;
            const bool present = window_.vulkanInstance()->supportsPresent(device, index, &window_);
            if (graphics && present) {
                graphicsFamily = index;
                presentFamily = index;
                break;
            }
            if (graphics && graphicsFamily == std::numeric_limits<std::uint32_t>::max()) {
                graphicsFamily = index;
            }
            if (present && presentFamily == std::numeric_limits<std::uint32_t>::max()) {
                presentFamily = index;
            }
        }
        if (graphicsFamily == std::numeric_limits<std::uint32_t>::max()
            || presentFamily == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }

        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(device, &properties);
        int score = 0;
        switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score += 10'000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 5'000; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score += 2'500; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score += 500; break;
        default: break;
        }
        score += static_cast<int>(std::min<std::uint32_t>(
            properties.limits.maxImageDimension2D / 1024U, 1'000U));
        if (score > best.score) {
            best = {
                .physicalDevice = device,
                .properties = properties,
                .graphicsQueueFamily = graphicsFamily,
                .presentQueueFamily = presentFamily,
                .score = score,
            };
        }
    }
    return best;
}

void VulkanCanvasBackend::createDevice(const DeviceCandidate& candidate)
{
    physicalDevice_ = candidate.physicalDevice;
    graphicsQueueFamily_ = candidate.graphicsQueueFamily;
    presentQueueFamily_ = candidate.presentQueueFamily;

    const float priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    queueInfos.reserve(graphicsQueueFamily_ == presentQueueFamily_ ? 1U : 2U);
    VkDeviceQueueCreateInfo graphicsInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    graphicsInfo.queueFamilyIndex = graphicsQueueFamily_;
    graphicsInfo.queueCount = 1;
    graphicsInfo.pQueuePriorities = &priority;
    queueInfos.push_back(graphicsInfo);
    if (presentQueueFamily_ != graphicsQueueFamily_) {
        VkDeviceQueueCreateInfo presentInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        presentInfo.queueFamilyIndex = presentQueueFamily_;
        presentInfo.queueCount = 1;
        presentInfo.pQueuePriorities = &priority;
        queueInfos.push_back(presentInfo);
    }

    const char* extensions[] {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = extensions;
    checkVk(vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_),
        "create canvas Vulkan device");
    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentQueueFamily_, 0, &presentQueue_);

    qCInfo(logVulkanHost) << "Selected Vulkan device:" << candidate.properties.deviceName
                          << "graphics queue" << graphicsQueueFamily_
                          << "present queue" << presentQueueFamily_;
}

void VulkanCanvasBackend::createFrameResources()
{
    VkSemaphoreCreateInfo semaphoreInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& frame : frames_) {
        VkCommandPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsQueueFamily_;
        checkVk(vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.commandPool),
            "create canvas command pool");

        VkCommandBufferAllocateInfo commandInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = frame.commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(device_, &commandInfo, &frame.commandBuffer),
            "allocate canvas command buffer");
        checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable),
            "create image-available semaphore");
        checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &frame.submitted),
            "create canvas frame fence");
    }
}

void VulkanCanvasBackend::createSwapchain()
{
    VkSurfaceCapabilitiesKHR capabilities {};
    const auto capabilitiesResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice_, surface_, &capabilities);
    if (capabilitiesResult == VK_ERROR_SURFACE_LOST_KHR) {
        qCWarning(logVulkanHost) << "Canvas surface was lost while creating its swapchain";
        return;
    }
    checkVk(capabilitiesResult, "query canvas surface capabilities");

    const auto extent = chooseSurfaceExtent(capabilities);
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    const auto surfaceFormat = chooseSurfaceFormat();
    auto imageCount = std::max(capabilities.minImageCount + 1U, 2U);
    if (capabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchainInfo {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainInfo.surface = surface_;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = surfaceFormat.format;
    swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const std::array queueFamilies {graphicsQueueFamily_, presentQueueFamily_};
    if (graphicsQueueFamily_ != presentQueueFamily_) {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchainInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilies.size());
        swapchainInfo.pQueueFamilyIndices = queueFamilies.data();
    } else {
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    swapchainInfo.preTransform = (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : capabilities.currentTransform;
    swapchainInfo.compositeAlpha = chooseCompositeAlpha(capabilities);
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    checkVk(vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_),
        "create canvas swapchain");

    colorFormat_ = surfaceFormat.format;
    colorSpace_ = surfaceFormat.colorSpace;
    swapchainExtent_ = extent;
    createRenderPass();
    createSwapchainImages();
    renderer_.initializeSwapchain(VulkanSwapchainContext {
        .renderPass = renderPass_,
        .colorFormat = colorFormat_,
        .extent = swapchainExtent_,
    });
    swapchainDirty_ = false;

    qCInfo(logVulkanHost) << "Created canvas swapchain"
                          << swapchainExtent_.width << 'x' << swapchainExtent_.height
                          << "images" << swapchainImages_.size()
                          << "format" << colorFormat_ << "color space" << colorSpace_;
}

void VulkanCanvasBackend::createRenderPass()
{
    VkAttachmentDescription colorAttachment {};
    colorAttachment.format = colorFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorReference {};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;

    VkSubpassDependency dependency {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    checkVk(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_),
        "create canvas render pass");
}

void VulkanCanvasBackend::createSwapchainImages()
{
    auto imageEnumeration = detail::enumerateVulkanValues<VkImage>(
        [this](auto* count, auto* images) {
            return vkGetSwapchainImagesKHR(device_, swapchain_, count, images);
        });
    logEnumerationRetries("Canvas swapchain images", imageEnumeration.attempts);
    checkVk(imageEnumeration.result, "get canvas swapchain images");
    auto& images = imageEnumeration.values;
    const auto imageCount = static_cast<std::uint32_t>(images.size());
    swapchainImages_.resize(imageCount);
    imageFences_.assign(imageCount, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (std::uint32_t index = 0; index < imageCount; ++index) {
        auto& image = swapchainImages_[index];
        image.image = images[index];

        VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &image.view),
            "create canvas swapchain image view");

        VkFramebufferCreateInfo framebufferInfo {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &image.view;
        framebufferInfo.width = swapchainExtent_.width;
        framebufferInfo.height = swapchainExtent_.height;
        framebufferInfo.layers = 1;
        checkVk(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &image.framebuffer),
            "create canvas framebuffer");
        checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &image.renderFinished),
            "create render-finished semaphore");
    }
}

void VulkanCanvasBackend::releaseSwapchain()
{
    if (device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE) {
        return;
    }
    checkVk(vkDeviceWaitIdle(device_), "wait before releasing canvas swapchain");
    renderer_.releaseSwapchain();
    for (auto& image : swapchainImages_) {
        if (image.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_, image.framebuffer, nullptr);
        }
        if (image.renderFinished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, image.renderFinished, nullptr);
        }
        if (image.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, image.view, nullptr);
        }
    }
    swapchainImages_.clear();
    imageFences_.clear();
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    swapchainExtent_ = {};
    colorFormat_ = VK_FORMAT_UNDEFINED;
    swapchainDirty_ = true;
}

void VulkanCanvasBackend::releaseDevice()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    checkVk(vkDeviceWaitIdle(device_), "wait before releasing canvas device");
    releaseSwapchain();
    renderer_.releaseDevice();
    for (auto& frame : frames_) {
        if (frame.submitted != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame.submitted, nullptr);
        }
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, frame.commandPool, nullptr);
        }
        frame = {};
    }
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    presentQueue_ = VK_NULL_HANDLE;
    currentFrame_ = 0;
    qCInfo(logVulkanHost) << "Released canvas Vulkan device";
}

VkSurfaceFormatKHR VulkanCanvasBackend::chooseSurfaceFormat() const
{
    auto formatEnumeration = detail::enumerateVulkanValues<VkSurfaceFormatKHR>(
        [this](auto* count, auto* formats) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(
                physicalDevice_, surface_, count, formats);
        });
    logEnumerationRetries("Canvas surface formats", formatEnumeration.attempts);
    checkVk(formatEnumeration.result, "get canvas surface formats");
    const auto& formats = formatEnumeration.values;
    if (formats.empty()) {
        qFatal("Canvas surface exposes no Vulkan formats");
    }
    if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED) {
        return {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }

    constexpr std::array preferredFormats {
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
    };
    VkSurfaceFormatKHR selected = formats.front();
    bool selectedPreferred = false;
    for (const auto preferred : preferredFormats) {
        const auto found = std::find_if(formats.cbegin(), formats.cend(), [preferred](const auto& format) {
            return format.format == preferred
                && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (found != formats.cend()) {
            selected = *found;
            selectedPreferred = true;
            break;
        }
    }
    if (!selectedPreferred) {
        const auto nonlinear = std::find_if(formats.cbegin(), formats.cend(), [](const auto& format) {
            return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (nonlinear != formats.cend()) {
            selected = *nonlinear;
        }
    }

    // Qt's Wayland integration may create a color-management surface itself.
    // Matching QVulkanWindow's policy avoids a second driver-owned protocol
    // object for the same wl_surface when pass-through is available.
    const auto* instance = window_.vulkanInstance();
    const bool passThroughEnabled = instance
        && instance->extensions().contains("VK_EXT_swapchain_colorspace");
    if (QGuiApplication::platformName() == QStringLiteral("wayland") && passThroughEnabled) {
        const auto passThrough = std::find_if(formats.cbegin(), formats.cend(), [&selected](const auto& format) {
            return format.format == selected.format
                && format.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT;
        });
        if (passThrough != formats.cend()) {
            selected = *passThrough;
        }
    }
    return selected;
}

VkExtent2D VulkanCanvasBackend::chooseSurfaceExtent(
    const VkSurfaceCapabilitiesKHR& capabilities) const
{
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    if (window_.width() <= 0 || window_.height() <= 0) {
        return {};
    }
    const auto dpr = window_.devicePixelRatio();
    const auto pixelWidth = static_cast<std::uint32_t>(std::max(
        0LL, std::llround(static_cast<double>(window_.width()) * dpr)));
    const auto pixelHeight = static_cast<std::uint32_t>(std::max(
        0LL, std::llround(static_cast<double>(window_.height()) * dpr)));
    return {
        std::clamp(pixelWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(pixelHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

VkCompositeAlphaFlagBitsKHR VulkanCanvasBackend::chooseCompositeAlpha(
    const VkSurfaceCapabilitiesKHR& capabilities) const
{
    constexpr std::array candidates {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const auto candidate : candidates) {
        if ((capabilities.supportedCompositeAlpha
                & static_cast<VkCompositeAlphaFlagsKHR>(candidate)) != 0) {
            return candidate;
        }
    }
    qFatal("Canvas surface exposes no supported composite alpha mode");
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

bool VulkanCanvasBackend::hasDeviceExtension(
    VkPhysicalDevice device, const char* extension) const
{
    auto extensionEnumeration = detail::enumerateVulkanValues<VkExtensionProperties>(
        [device](auto* count, auto* extensions) {
            return vkEnumerateDeviceExtensionProperties(
                device, nullptr, count, extensions);
        });
    logEnumerationRetries("Vulkan device extensions", extensionEnumeration.attempts);
    checkVk(extensionEnumeration.result, "get Vulkan device extensions");
    const auto& extensions = extensionEnumeration.values;
    return std::any_of(extensions.cbegin(), extensions.cend(), [extension](const auto& available) {
        return std::strcmp(available.extensionName, extension) == 0;
    });
}

bool VulkanCanvasBackend::hasUsableSwapchain(VkPhysicalDevice device) const
{
    auto formatEnumeration = detail::enumerateVulkanValues<VkSurfaceFormatKHR>(
        [this, device](auto* count, auto* formats) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(
                device, surface_, count, formats);
        });
    logEnumerationRetries("Candidate surface formats", formatEnumeration.attempts);
    if (formatEnumeration.result != VK_SUCCESS || formatEnumeration.values.empty()) {
        return false;
    }
    auto presentModeEnumeration = detail::enumerateVulkanValues<VkPresentModeKHR>(
        [this, device](auto* count, auto* modes) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(
                device, surface_, count, modes);
        });
    logEnumerationRetries("Candidate present modes", presentModeEnumeration.attempts);
    return presentModeEnumeration.result == VK_SUCCESS
        && !presentModeEnumeration.values.empty();
}

} // namespace imageeditor::render
