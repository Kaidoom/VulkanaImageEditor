#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SelectedPixelTransform.hpp"
#include "imageeditor/render/CanvasCoordinateMapping.hpp"
#include "imageeditor/render/VulkanCanvasRenderer.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/QtTextLayoutService.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace c = imageeditor::core;
namespace r = imageeditor::render;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
int validationMessages = 0;
void check(bool value, std::string_view message)
{
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void vkCheck(VkResult result, std::string_view operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
VKAPI_ATTR VkBool32 VKAPI_CALL debugReport(VkDebugReportFlagsEXT,
    VkDebugReportObjectTypeEXT, std::uint64_t, std::size_t, std::int32_t,
    const char* prefix, const char* message, void*)
{
    ++validationMessages;
    std::cerr << "Vulkan validation [" << prefix << "]: " << message << '\n';
    return VK_FALSE;
}

// No QWindow/surface/swapchain or desktop capture. This host owns submission
// and reads a genuine sRGB framebuffer rendered by the shipping canvas code.
class OffscreenCanvas {
public:
    explicit OffscreenCanvas(bool validation, VkExtent2D targetExtent = {96,80})
        : targetExtent_(targetExtent)
    {
        try { initialize(validation); }
        catch (...) { destroy(); throw; }
    }
    ~OffscreenCanvas() { destroy(); }
    OffscreenCanvas(const OffscreenCanvas&) = delete;
    OffscreenCanvas& operator=(const OffscreenCanvas&) = delete;

    static constexpr VkExtent2D extent {96, 80};
    QImage render(const r::CanvasScene& scene, bool readback = true)
    {
        const auto started = std::chrono::steady_clock::now();
        vkCheck(vkResetCommandPool(device_, pool_, 0), "reset command pool");
        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(command_, &begin), "begin frame");
        renderer_.recordFrame(scene, {command_, framebuffer_, targetExtent_, 0});
        if (readback) {
            VkBufferImageCopy copy {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {targetExtent_.width, targetExtent_.height, 1};
            vkCmdCopyImageToBuffer(command_, target_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                readback_, 1, &copy);
            VkBufferMemoryBarrier host {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback_;
            host.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 0, nullptr, 1, &host, 0, nullptr);
        }
        vkCheck(vkEndCommandBuffer(command_), "end frame");
        vkCheck(vkResetFences(device_, 1, &fence_), "reset fence");
        VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        vkCheck(vkQueueSubmit(queue_, 1, &submit, fence_), "submit frame");
        vkCheck(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 30'000'000'000ull), "wait for frame");
        frameMilliseconds_ = std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-started).count();
        if (!readback) return {};
        void* bytes = nullptr;
        vkCheck(vkMapMemory(device_, readbackMemory_, 0, VK_WHOLE_SIZE, 0, &bytes), "map readback");
        const QImage view(static_cast<const uchar*>(bytes), int(targetExtent_.width), int(targetExtent_.height),
            int(targetExtent_.width) * 4, QImage::Format_RGBA8888);
        auto result = view.copy();
        vkUnmapMemory(device_, readbackMemory_);
        return result;
    }
    r::RendererStats stats() const { return renderer_.stats(); }
    double frameMilliseconds() const { return frameMilliseconds_; }

private:
    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memory {};
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        throw std::runtime_error("No suitable Vulkan memory type");
    }
    VkDeviceMemory allocate(const VkMemoryRequirements& required, VkMemoryPropertyFlags properties)
    {
        VkMemoryAllocateInfo info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = required.size;
        info.memoryTypeIndex = memoryType(required.memoryTypeBits, properties);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        vkCheck(vkAllocateMemory(device_, &info, nullptr, &memory), "allocate memory");
        return memory;
    }
    void initialize(bool validation)
    {
        constexpr const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        constexpr std::array validationExtensions {
            VK_EXT_DEBUG_REPORT_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        constexpr auto synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = &synchronization;
        if (validation) {
            std::uint32_t count = 0;
            vkCheck(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate validation layers");
            std::vector<VkLayerProperties> layers(count);
            vkCheck(vkEnumerateInstanceLayerProperties(&count, layers.data()), "read validation layers");
            if (std::ranges::none_of(layers, [](const auto& p) {
                    return std::string_view(p.layerName) == "VK_LAYER_KHRONOS_validation";
                })) throw std::runtime_error("--validation requires VK_LAYER_KHRONOS_validation");
        }
        VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Vulkana blend rendering tests";
        // The shipping app/shaders require Vulkan1.2 (SPIR-V1.5).
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        if (validation) {
            instanceInfo.enabledLayerCount = 1;
            instanceInfo.ppEnabledLayerNames = &validationLayer;
            instanceInfo.enabledExtensionCount = std::uint32_t(validationExtensions.size());
            instanceInfo.ppEnabledExtensionNames = validationExtensions.data();
            instanceInfo.pNext = &validationFeatures;
        }
        vkCheck(vkCreateInstance(&instanceInfo, nullptr, &instance_), "create instance");
        if (validation) {
            const auto create = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugReportCallbackEXT"));
            if (!create) throw std::runtime_error("Missing debug report extension entry point");
            VkDebugReportCallbackCreateInfoEXT callback {VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
            callback.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT
                | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
            callback.pfnCallback = debugReport;
            vkCheck(create(instance_, &callback, nullptr, &debug_), "create validation callback");
        }
        std::uint32_t count = 0;
        vkCheck(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate Vulkan devices");
        std::vector<VkPhysicalDevice> devices(count);
        vkCheck(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "read Vulkan devices");
        for (auto candidate : devices) {
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> properties(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
            for (std::uint32_t i = 0; i < families; ++i)
                if ((properties[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                    == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                    physical_ = candidate;
                    queueFamily_ = i;
                    break;
                }
            if (physical_) break;
        }
        if (!physical_) throw std::runtime_error("No Vulkan graphics+compute queue available");
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        std::cout << "Offscreen Vulkan device: " << properties.deviceName << '\n';
        const float priority = 1;
        VkDeviceQueueCreateInfo queue {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = queueFamily_;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queue;
        vkCheck(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "create logical device");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = queueFamily_;
        vkCheck(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "create command pool");
        VkCommandBufferAllocateInfo commands {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = pool_;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(device_, &commands, &command_), "allocate command buffer");
        VkFenceCreateInfo fence {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCheck(vkCreateFence(device_, &fence, nullptr, &fence_), "create frame fence");

        VkImageCreateInfo image {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_SRGB;
        image.extent = {targetExtent_.width, targetExtent_.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(device_, &image, nullptr, &target_), "create target image");
        VkMemoryRequirements required {};
        vkGetImageMemoryRequirements(device_, target_, &required);
        targetMemory_ = allocate(required, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkBindImageMemory(device_, target_, targetMemory_, 0), "bind target memory");
        VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = target_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(device_, &view, nullptr, &targetView_), "create target view");

        VkAttachmentDescription attachment {};
        attachment.format = image.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference color {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        std::array<VkSubpassDependency, 2> dependencies {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount = 1;
        pass.pAttachments = &attachment;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = std::uint32_t(dependencies.size());
        pass.pDependencies = dependencies.data();
        vkCheck(vkCreateRenderPass(device_, &pass, nullptr, &pass_), "create render pass");
        VkFramebufferCreateInfo framebuffer {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = pass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &targetView_;
        framebuffer.width = targetExtent_.width;
        framebuffer.height = targetExtent_.height;
        framebuffer.layers = 1;
        vkCheck(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffer_), "create framebuffer");

        VkBufferCreateInfo buffer {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = VkDeviceSize(targetExtent_.width) * targetExtent_.height * 4;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &buffer, nullptr, &readback_), "create readback buffer");
        vkGetBufferMemoryRequirements(device_, readback_, &required);
        readbackMemory_ = allocate(required, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkBindBufferMemory(device_, readback_, readbackMemory_, 0), "bind readback memory");
        rendererDevice_ = true;
        renderer_.initializeDevice({physical_, device_, properties, 1});
        rendererSwapchain_ = true;
        renderer_.initializeSwapchain({pass_, image.format, targetExtent_});
    }
    void destroy() noexcept
    {
        if (device_) {
            vkDeviceWaitIdle(device_);
            if (rendererSwapchain_) renderer_.releaseSwapchain();
            if (rendererDevice_) renderer_.releaseDevice();
            if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
            if (readbackMemory_) vkFreeMemory(device_, readbackMemory_, nullptr);
            if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
            if (pass_) vkDestroyRenderPass(device_, pass_, nullptr);
            if (targetView_) vkDestroyImageView(device_, targetView_, nullptr);
            if (target_) vkDestroyImage(device_, target_, nullptr);
            if (targetMemory_) vkFreeMemory(device_, targetMemory_, nullptr);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (debug_) {
            const auto release = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT"));
            if (release) release(instance_, debug_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    VkInstance instance_ {};
    VkDebugReportCallbackEXT debug_ {};
    VkPhysicalDevice physical_ {};
    VkDevice device_ {};
    VkQueue queue_ {};
    std::uint32_t queueFamily_ {};
    VkCommandPool pool_ {};
    VkCommandBuffer command_ {};
    VkFence fence_ {};
    VkImage target_ {};
    VkDeviceMemory targetMemory_ {};
    VkImageView targetView_ {};
    VkRenderPass pass_ {};
    VkFramebuffer framebuffer_ {};
    VkBuffer readback_ {};
    VkDeviceMemory readbackMemory_ {};
    r::VulkanCanvasRenderer renderer_;
    bool rendererDevice_ {}, rendererSwapchain_ {};
    VkExtent2D targetExtent_ {};
    double frameMilliseconds_ {};
};

std::shared_ptr<c::ContiguousRasterSurface> patterned(c::Extent2u extent, bool source)
{
    std::vector<std::byte> bytes(std::size_t(extent.width) * extent.height * 4);
    for (std::uint32_t y = 0; y < extent.height; ++y) for (std::uint32_t x = 0; x < extent.width; ++x) {
        const auto offset = (std::size_t(y) * extent.width + x) * 4;
        const int ramp = source ? int((x * 255) / (extent.width - 1)) : int((y * 255) / (extent.height - 1));
        const int alpha = ((x / 7 + y / 9) % 3 == 0) ? 255 : ramp;
        const bool transparent = ((x / 5 + y / 7) % 5) == 0;
        // Alternating poisonous hidden RGB next to alpha edges exposes filtering
        // of straight channels before premultiplication.
        const std::array pixel = transparent ? std::array {255,0,255,0}
            : std::array {int((x * 31 + y * 7 + (source ? 173 : 17)) % 256),
                int((x * 9 + y * 21 + 81) % 256), int((x * 7 + y * 11 + 153) % 256), alpha};
        for (std::size_t channel = 0; channel < 4; ++channel)
            bytes[offset + channel] = std::byte(pixel[channel]);
    }
    return std::make_shared<c::ContiguousRasterSurface>(extent, std::move(bytes));
}
r::CanvasScene sceneFor(const c::Document& document)
{
    r::CanvasScene scene;
    scene.document = document.snapshot();
    scene.logicalViewport = {OffscreenCanvas::extent.width, OffscreenCanvas::extent.height};
    scene.checkerLight = {205,211,216,255};
    scene.checkerDark = {94,103,117,255};
    scene.canvasBackground = {35,41,47,255};
    return scene;
}
QColor qt(c::Rgba8 color) { return {color.red,color.green,color.blue,color.alpha}; }
c::Rgba8 fromQt(QColor color)
{ return {std::uint8_t(color.red()),std::uint8_t(color.green()),std::uint8_t(color.blue()),std::uint8_t(color.alpha())}; }

QImage verify(const c::Document& document, const r::CanvasScene& scene, const QImage& actual,
    std::string_view name, bool documentPixelCenters = true)
{
    const c::Extent2d docSize {double(document.canvas().extent.width),double(document.canvas().extent.height)};
    const r::CanvasCoordinateMapping mapping(docSize,scene.logicalViewport,
        {OffscreenCanvas::extent.width,OffscreenCanvas::extent.height},scene.viewport);
    const auto origin = mapping.documentToFramebuffer({0,0});
    const auto unit = mapping.documentToFramebuffer({1,1});
    const c::AffineTransform toFrame {unit.x-origin.x,0,origin.x,0,unit.y-origin.y,origin.y};
    auto sourceLayers = document.layers();
    std::vector<const c::Layer*> pointers;
    for (auto& layer : sourceLayers) {
        layer.visible = document.isEffectivelyVisible(layer.id);
        pointers.push_back(&layer);
    }
    const c::PinnedDocumentSampler sampler(pointers,
        {OffscreenCanvas::extent.width,OffscreenCanvas::extent.height},toFrame,{},&document.tree());
    const auto rect = mapping.documentCanvasFramebufferRect();
    QImage expected(actual.size(),QImage::Format_RGBA8888);
    int worst = 0, badPixels = 0, inspected = 0;
    QPoint worstPoint;
    for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x) {
        const auto point = c::Vec2d {x+.5,y+.5};
        const bool inside = point.x >= rect[0] && point.y >= rect[1]
            && point.x < rect[0]+rect[2] && point.y < rect[1]+rect[3];
        const auto checker = ((int(std::floor((point.x-rect[0])/12))
            + int(std::floor((point.y-rect[1])/12))) & 1) ? scene.checkerDark : scene.checkerLight;
        c::Rgba8 reference = scene.canvasBackground;
        if (inside) {
            const auto sample = documentPixelCenters
                ? c::sampleDocumentColor(document,{},point,c::ColorSampleSource::MergedVisible).color
                : sampler.sample(point);
            reference = c::encodeColor(c::compositeLayer(c::decodeColor(checker),
                c::decodeColor(sample),1,c::BlendMode::Normal));
        }
        expected.setPixelColor(x,y,qt(reference));
        // Canvas border is a deliberate overlay, not document content.
        const double edgeDistance = std::min({std::abs(point.x-rect[0]),std::abs(point.y-rect[1]),
            std::abs(point.x-rect[0]-rect[2]),std::abs(point.y-rect[1]-rect[3])});
        if (edgeDistance < 1.6) continue;
        ++inspected;
        const auto gpu = fromQt(actual.pixelColor(x,y));
        const int error = std::max({std::abs(int(gpu.red)-reference.red),std::abs(int(gpu.green)-reference.green),
            std::abs(int(gpu.blue)-reference.blue),std::abs(int(gpu.alpha)-reference.alpha)});
        if (error > worst) { worst = error; worstPoint = {x,y}; }
        // CPU sampling returns straight RGBA8, introducing one unavoidable
        // quantization before compositing over the checker. GPU stays float.
        if (error > 2) ++badPixels;
    }
    check(inspected > 5000,"GPU comparison must cover a substantial image, not a few lucky pixels");
    if (badPixels) {
        ++failures;
        std::cerr << "FAIL GPU/CPU " << name << ": " << badPixels << '/' << inspected
                  << " pixels exceed 2 bytes; max=" << worst << " at " << worstPoint.x() << ',' << worstPoint.y()
                  << "; GPU=" << actual.pixelColor(worstPoint).name(QColor::HexArgb).toStdString()
                  << " CPU=" << expected.pixelColor(worstPoint).name(QColor::HexArgb).toStdString() << '\n';
    }
    return expected;
}

struct Review {
    QImage image;
    explicit Review(std::size_t count=c::allBlendModes.size())
    {
        if (!qEnvironmentVariableIsEmpty("IMAGEEDITOR_BLEND_REVIEW")) {
            image = QImage(480,int((count+1)/2)*112,QImage::Format_RGB32);
            image.fill(QColor(28,31,37));
        }
    }
    void add(std::size_t index, std::string_view name, const QImage& gpu, const QImage& cpu)
    {
        if (image.isNull()) return;
        QPainter painter(&image);
        const int x = int(index%2)*240+6, y = int(index/2)*112+5;
        painter.fillRect(x-6,y-5,240,112,QColor(28,31,37));
        painter.setPen(Qt::white);
        painter.drawText(x,y+13,QString::fromUtf8(name.data(),qsizetype(name.size()))+"  GPU | CPU");
        painter.drawImage(x,y+21,gpu);
        painter.drawImage(x+102,y+21,cpu);
    }
    void save()
    {
        if (!image.isNull()) check(image.save(qEnvironmentVariable("IMAGEEDITOR_BLEND_REVIEW")),"save blend review sheet");
    }
};

void allModesAndCacheBehavior(OffscreenCanvas& gpu, Review& review)
{
    c::Document document({{96,80},96});
    auto base = c::Layer::raster("Alpha backdrop",patterned({96,80},false));
    auto foreground = c::Layer::raster("Transformed alpha edges",patterned({48,48},true));
    foreground.localToDocument = {1.27,-.23,25.125,.19,1.11,4.375};
    foreground.opacity = .73f;
    const auto id = foreground.id;
    check(document.insertLayer(0,std::move(base)) && document.insertLayer(1,std::move(foreground)),"insert blend fixture");
    auto scene = sceneFor(document);
    for (std::size_t i = 0; i < c::allBlendModes.size(); ++i) {
        document.setLayerBlendMode(id,c::allBlendModes[i]);
        document.setLayerOpacity(id,.73f);
        scene.document = document.snapshot();
        const auto before = gpu.stats();
        const auto image = gpu.render(scene);
        const auto after = gpu.stats();
        const auto reference = verify(document,scene,image,c::blendModeName(c::allBlendModes[i]));
        review.add(i,c::blendModeName(c::allBlendModes[i]),image,reference);
        if (i) {
            check(after.uploadedBytes == before.uploadedBytes,"blend mode changes must not upload raster textures");
            check(after.compositionPasses > before.compositionPasses,"mode changes must invalidate composition");
        }
        const auto cached = gpu.render(scene);
        const auto repeat = gpu.stats();
        check(cached == image,"repeated identical frame must produce identical pixels");
        check(repeat.compositionPasses == after.compositionPasses
            && repeat.compositionDispatches == after.compositionDispatches,"idle redraw reuses composition");
        check(repeat.uploadedBytes == after.uploadedBytes,"idle redraw must not upload content");
        document.setLayerOpacity(id,1.f);
        scene.document = document.snapshot();
        verify(document,scene,gpu.render(scene),
            std::string(c::blendModeName(c::allBlendModes[i]))+" at full layer opacity");
        check(gpu.stats().uploadedBytes == repeat.uploadedBytes,"opacity changes must not upload content");
    }
    std::cout << "Compared all " << c::allBlendModes.size()
              << " blend modes at full/fractional opacity against CPU samples\n";
    const auto before = gpu.stats();
    scene.overlayAccent = {239,112,11,255};
    scene.cursorLogical = {30,30};
    scene.cursorInside = true;
    scene.activeTool = c::ToolId::Brush;
    scene.selectionPhase = .7;
    gpu.render(scene);
    const auto after = gpu.stats();
    check(after.compositionPasses == before.compositionPasses
        && after.compositionDispatches == before.compositionDispatches,"overlay/cursor changes must reuse composition");
    check(after.uploadedBytes == before.uploadedBytes,"overlay/cursor changes must reuse layer textures");
    scene.cursorInside = false;
    scene.activeTool = c::ToolId::Move;
    document.setLayerTransform(id,{-1.13,.18,83.25,.14,1.24,1.875});
    scene.document = document.snapshot();
    verify(document,scene,gpu.render(scene),"negative-scale transformed source");
    scene.logicalViewport = {64,80.0/1.5};
    scene.devicePixelRatio = 1.5;
    scene.viewport.setZoom(.83);
    scene.viewport.setPan({-11.25,8.5});
    verify(document,scene,gpu.render(scene),"fractional display scale with zoom and pan",false);
    check(gpu.stats().uploadedBytes == after.uploadedBytes,"transform/zoom/pan must not reupload raster content");
}

void arithmeticEndpointsAndGradients(OffscreenCanvas& gpu)
{
    // Repeated grids cover every pair (including exact endpoint intersections)
    // away from the deliberate canvas-border overlay. Values straddle clipping
    // boundaries and include the smallest nonzero RGBA8 channel values.
    constexpr std::array<std::uint8_t,16> levels {
        0,1,2,3,16,63,64,126,127,128,129,191,192,253,254,255};
    for (const bool partialAlpha : {false,true}) {
        const auto extent = c::Extent2u {96,80};
        std::vector<std::byte> lower(96*80*4), upper(lower.size());
        for (int y=0;y<80;++y) for (int x=0;x<96;++x) {
            const auto b=levels[std::size_t(x%16)], s=levels[std::size_t(y%16)];
            const std::array<std::uint8_t,4> background {
                b,s,std::uint8_t(255-b),partialAlpha ? levels[std::size_t((y/16)*3)] : std::uint8_t(255)};
            const std::array<std::uint8_t,4> source {
                s,b,std::uint8_t(255-s),partialAlpha ? levels[std::size_t((x/16)*3)] : std::uint8_t(255)};
            const auto offset = std::size_t(y*96+x)*4;
            for (std::size_t channel=0;channel<4;++channel) {
                lower[offset+channel]=std::byte(background[channel]);
                upper[offset+channel]=std::byte(source[channel]);
            }
        }
        c::Document document({extent,96});
        auto base=c::Layer::raster("Endpoint backdrop",std::make_shared<c::ContiguousRasterSurface>(extent,std::move(lower)));
        auto source=c::Layer::raster("Endpoint source",std::make_shared<c::ContiguousRasterSurface>(extent,std::move(upper)));
        const auto id=source.id;
        check(document.insertLayer(0,std::move(base)) && document.insertLayer(1,std::move(source)),"insert endpoint grid");
        auto scene=sceneFor(document);
        for (const auto mode:{c::BlendMode::ColorDodge,c::BlendMode::LinearDodge,
                 c::BlendMode::ColorBurn,c::BlendMode::LinearBurn,c::BlendMode::Subtract,c::BlendMode::Divide}) {
            document.setLayerBlendMode(id,mode);
            for (float opacity:{0.f,.37f,1.f}) {
                document.setLayerOpacity(id,opacity);
                scene.document=document.snapshot();
                verify(document,scene,gpu.render(scene),std::string(c::blendModeName(mode))+" endpoints and opacity");
            }
            // A small fractional shift creates near-zero/near-unit filtered
            // channels. There is no epsilon cutoff in the named blend stage.
            const auto before=gpu.stats();
            document.setLayerTransform(id,{.m02=1.0/4096,.m12=-1.0/2048});
            scene.document=document.snapshot();
            verify(document,scene,gpu.render(scene),std::string(c::blendModeName(mode))+" near-endpoint filtering");
            check(gpu.stats().uploadedBytes==before.uploadedBytes,"endpoint transform reuses textures");
            document.setLayerTransform(id,{});
        }
    }
}

void whiteBackdropEndpointStacks(OffscreenCanvas& gpu)
{
    const auto extent=c::Extent2u {96,80};
    constexpr std::array<std::uint8_t,8> alphas {1,2,4,9,27,64,128,254};
    std::vector<std::byte> lower(96*80*4,std::byte {255}), upper(lower);
    for (int y=0;y<80;++y) for (int x=0;x<96;++x) {
        const auto offset=std::size_t(y*96+x)*4+3;
        lower[offset]=std::byte(alphas[std::size_t(x%8)]);
        upper[offset]=std::byte(alphas[std::size_t(y%8)]);
    }
    c::Document document({extent,96});
    auto base=c::Layer::raster("White backdrop",std::make_shared<c::ContiguousRasterSurface>(extent,std::move(lower)));
    auto white=c::Layer::raster("White coverage",std::make_shared<c::ContiguousRasterSurface>(extent,std::move(upper)));
    white.opacity=.37f; const auto id=white.id;
    auto black=c::Layer::raster("Burn endpoint",std::make_shared<c::ContiguousRasterSurface>(extent,c::Rgba8 {0,0,0,255}));
    black.blendMode=c::BlendMode::ColorBurn;
    check(document.insertLayer(0,std::move(base)) && document.insertLayer(1,std::move(white))
        && document.insertLayer(2,std::move(black)),"insert stacked-white endpoint fixture");
    auto scene=sceneFor(document);
    for (auto mode:c::allBlendModes) {
        if (mode==c::BlendMode::Difference || mode==c::BlendMode::Exclusion || mode==c::BlendMode::Subtract) continue;
        document.setLayerBlendMode(id,mode); scene.document=document.snapshot();
        const auto image=gpu.render(scene);
        bool correct=true;
        for (int y=2;y<78;++y) for (int x=2;x<94;++x) {
            // Independent expected coverage: all-white stays unassociated
            // white, and opaque black Color Burn preserves that overlap only.
            const double ab=alphas[std::size_t(x%8)]/255.0;
            const double as=alphas[std::size_t(y%8)]/255.0*double(.37f);
            const int value=c::linearToSrgb(as+ab*(1-as));
            const auto actual=image.pixelColor(x,y);
            const bool pixelCorrect=actual.alpha()==255 && std::abs(actual.red()-value)<=2
                && std::abs(actual.green()-value)<=2 && std::abs(actual.blue()-value)<=2;
            if (correct && !pixelCorrect)
                std::cerr << "White stack " << c::blendModeName(mode) << " at " << x << ',' << y
                    << ": expected " << value << ", GPU " << actual.name(QColor::HexArgb).toStdString() << '\n';
            correct &= pixelCorrect;
        }
        check(correct,std::string(c::blendModeName(mode))+" white stack retains exact Burn endpoint");
    }
}

void loadFontFixture()
{
    const auto filename = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("assets/fonts/NotoSans-Regular.ttf");
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read pinned Noto Sans fixture");
    check(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex()
        == "b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5","pinned font fixture digest");
    if (QFontDatabase::addApplicationFont(filename) < 0) throw std::runtime_error("Cannot load pinned font fixture");
}
void typedStacksAndPassThrough(OffscreenCanvas& gpu)
{
    c::Document document({{96,80},96});
    auto raster = c::Layer::raster("Backdrop",patterned({96,80},false));
    const auto rasterId = raster.id;
    check(document.insertLayer(0,std::move(raster)),"insert typed backdrop");
    u::QtShapeRenderService shapes;
    c::ShapeLayer data;
    data.kind = c::ShapeKind::Ellipse;
    data.size = {49.5,33.75};
    data.fillColor = {12,211,137,143};
    data.strokeEnabled = true;
    data.strokeColor = {237,22,117,181};
    data.strokeWidth = 3.125;
    auto shape = c::Layer::shape("Antialiased fill and stroke",data);
    shape.localToDocument = {.92,-.23,24.75,.15,1.1,24.5};
    shape.opacity = .81f;
    shape.blendMode = c::BlendMode::SoftLight;
    shape.renderCache = shapes.render({data,1.5});
    const auto shapeId = shape.id;
    check(document.insertLayer(1,std::move(shape)),"insert shape cache");
    c::TextLayer text;
    text.utf8 = "O ffi\nBlend";
    text.defaultStyle.font = {"Noto Sans","Regular",400,false};
    text.defaultStyle.sizePixels = 18;
    text.defaultStyle.color = {246,133,18,153};
    text = c::normalizedText(text);
    u::QtTextLayoutService fonts;
    auto type = c::Layer::text("Antialiased text",text);
    type.localToDocument = {-.92,.15,77.125,.08,1.04,9.25};
    type.opacity = .69f;
    type.blendMode = c::BlendMode::Color;
    type.renderCache = fonts.layout({text,1.5}).cache;
    const auto textId = type.id;
    check(document.insertLayer(2,std::move(type)),"insert text cache");
    auto scene = sceneFor(document);
    const auto flat = gpu.render(scene);
    verify(document,scene,flat,"raster + antialiased shape + formatted text");
    const auto before = gpu.stats();
    const auto old = document.tree();
    auto tree = old;
    const auto group = c::makeLayerId(), folder = c::makeLayerId();
    tree.roots = {rasterId,folder};
    tree.containers.push_back({group,"Pass-through group",c::ContainerKind::Group,c::ColorLabel::None,{shapeId,textId},true});
    tree.containers.push_back({folder,"Folder",c::ContainerKind::Folder,c::ColorLabel::None,{group},true});
    check(document.replaceStructure(old,tree),"organize typed blend stack into nested pass-through containers");
    scene.document = document.snapshot();
    const auto grouped = gpu.render(scene);
    check(grouped == flat,"pass-through organization must be pixel-exact on Vulkan");
    verify(document,scene,grouped,"pass-through group nested in folder");
    check(gpu.stats().uploadedBytes == before.uploadedBytes,"group creation must preserve all texture caches");
    const c::ItemVisibilityUpdate hidden {folder,true,false};
    check(document.setItemVisibilities(std::span(&hidden,1)),"hide parent folder");
    scene.document = document.snapshot();
    verify(document,scene,gpu.render(scene),"parent folder visibility suppresses typed descendants");
    const c::ItemVisibilityUpdate shown {folder,false,true};
    check(document.setItemVisibilities(std::span(&shown,1)),"show parent folder");
    document.setLayerBlendMode(shapeId,c::BlendMode::HardLight);
    document.setLayerBlendMode(textId,c::BlendMode::Luminosity);
    scene.document = document.snapshot();
    verify(document,scene,gpu.render(scene),"typed content with updated stacked modes");
    const std::array arithmetic {c::BlendMode::ColorDodge,c::BlendMode::LinearDodge,
        c::BlendMode::ColorBurn,c::BlendMode::LinearBurn,c::BlendMode::Subtract,c::BlendMode::Divide};
    for (std::size_t i=0;i<arithmetic.size();++i) {
        document.setLayerBlendMode(shapeId,arithmetic[i]);
        document.setLayerBlendMode(textId,arithmetic[(i+1)%arithmetic.size()]);
        scene.document=document.snapshot();
        verify(document,scene,gpu.render(scene),std::string(c::blendModeName(arithmetic[i]))+" nested text/shape stack");
    }
    check(gpu.stats().uploadedBytes==before.uploadedBytes,"typed blend changes reuse editable render caches/textures");
    check(std::holds_alternative<c::ShapeLayer>(document.layer(shapeId)->payload)
        && std::holds_alternative<c::TextLayer>(document.layer(textId)->payload),"blending never flattens editable typed data");
}

void projectiveRendering(OffscreenCanvas& gpu)
{
    c::Document document({{96,80},96});
    auto layer=c::Layer::raster("Asymmetric projective grid",patterned({64,48},true));
    const auto id=layer.id;const auto source=std::get<c::RasterLayer>(layer.payload).surface;
    layer.localToDocument=*c::rectangleToQuad({0,0,64,48},{{{11,9},{78,2},{85,65},{5,72}}});
    check(document.insertLayer(0,std::move(layer)),"insert projective raster");
    auto scene=sceneFor(document);verify(document,scene,gpu.render(scene),"homogeneous grid over transparency");
    const auto uploaded=gpu.stats().uploadedBytes;
    auto matrix=document.layer(id)->localToDocument;matrix=c::composeTransform({1,0,1.5,0,1,-.5},matrix);
    document.setLayerTransform(id,matrix);scene.document=document.snapshot();
    verify(document,scene,gpu.render(scene),"continued movement preserves projective sampling");
    check(gpu.stats().uploadedBytes==uploaded,"projective geometry changes do not upload source again");
    document.layer(id)->crop=c::LayerCrop{{3.5,4.2,52,38}};
    scene.document=document.snapshot();verify(document,scene,gpu.render(scene),"projective crop footprint");
    document.setSelection(c::SelectionMask::rectangle({96,80},{24,15,18,23},155));
    c::History history;
    {c::SelectedPixelTransformSession pixels(document,id);
        check(pixels.preview({1,0,-17.5,0,1,10.25}),"preview selected pixels on projective source");
        check(pixels.completeAction(),"complete selected pixel gesture");
        scene.document=document.snapshot();verify(document,scene,gpu.render(scene),"one provisional native raw layer");
        check(pixels.commit(history)==c::TransformCommitResult::Committed,"apply selected pixels");}
    scene.document=document.snapshot();verify(document,scene,gpu.render(scene),"committed regional surface origin");
    check(history.undo(document),"undo selected pixels");
    check(std::get<c::RasterLayer>(document.layer(id)->payload).surface==source,"exact original restored");

    c::Document large({{96,80},96});
    auto raw=c::Layer::raster("Regional GPU reuse",patterned({512,512},true));const auto rawId=raw.id;
    check(large.insertLayer(0,std::move(raw)),"insert regional upload fixture");
    large.setSelection(c::SelectionMask::rectangle({96,80},{12,12,13,11}));
    scene=sceneFor(large);(void)gpu.render(scene);const auto before=gpu.stats();
    c::SelectedPixelTransformSession regional(large,rawId);
    for(const auto delta:std::array{c::Vec2d{9.25,2.5},c::Vec2d{-30.25,-22.5},c::Vec2d{17.25,8.5}}){
        check(regional.preview({1,0,delta.x,0,1,delta.y}),"preview changes storage extent safely");
        check(regional.completeAction(),"complete regional resize gesture");
        scene.document=large.snapshot();verify(large,scene,gpu.render(scene),"regional texture reuse and resize");
    }
    check(gpu.stats().fullUploads==before.fullUploads,"regional previews never reupload the entire immutable source");
    check(gpu.stats().regionalSourceCopies>=before.regionalSourceCopies+3,"GPU base reused for initial and resized storage");
    check(gpu.stats().uploadedBytes-before.uploadedBytes<512*512*4,"regional preview uploads remain below one whole-layer upload");
    regional.cancel();scene.document=large.snapshot();verify(large,scene,gpu.render(scene),"regional preview cancel restores original texture");
}

void mergePreservesNativeFramebuffer(OffscreenCanvas& gpu)
{
    const auto compare = [&](const c::Document& document, std::string_view name,
                             int tolerance, c::Vec2d pan) {
        auto scene = sceneFor(document);
        scene.viewport.setZoom(1);
        scene.viewport.setPan(pan);
        const auto before = gpu.render(scene);
        std::vector<c::LayerId> selected;
        for (const auto& layer : document.layers()) selected.push_back(layer.id);
        const auto result = u::flattenLayerItems(document, selected);
        check(bool(result), std::string(name) + " merge succeeds");
        if (!result) {
            std::cerr << result.error.toStdString() << '\n';
            return;
        }
        const c::Extent2u extent {std::uint32_t(result.image.width()), std::uint32_t(result.image.height())};
        const auto stride = std::size_t(extent.width) * 4;
        std::vector<std::byte> pixels(stride * extent.height);
        for (std::uint32_t y = 0; y < extent.height; ++y)
            std::memcpy(pixels.data() + std::size_t(y) * stride, result.image.constScanLine(int(y)), stride);
        auto baked = c::Layer::raster("Merged result",
            std::make_shared<c::ContiguousRasterSurface>(extent, std::move(pixels)));
        baked.localToDocument.m02 = result.origin.x;
        baked.localToDocument.m12 = result.origin.y;
        c::Document merged(document.canvas());
        check(merged.insertLayer(0, std::move(baked)), "insert merged raster for Vulkan comparison");
        scene.document = merged.snapshot();
        const auto after = gpu.render(scene);
        int worst = 0, different = 0;
        for (int y = 0; y < before.height(); ++y) for (int x = 0; x < before.width(); ++x) {
            const auto a = before.pixelColor(x, y), b = after.pixelColor(x, y);
            const auto difference = std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                std::abs(a.blue() - b.blue()), std::abs(a.alpha() - b.alpha())});
            worst = std::max(worst, difference);
            if (difference > tolerance) ++different;
        }
        check(different == 0, std::string(name) + " preserves the actual Vulkan framebuffer");
        if (different)
            std::cerr << "  " << name << ": " << different << " pixels exceed " << tolerance
                      << " display bytes; maximum difference=" << worst << '\n';
    };

    c::Document aligned({{96,80},96});
    auto first = c::Layer::raster("Identity-aligned detail", patterned({25,23}, false));
    first.localToDocument.m02 = 5;
    first.localToDocument.m12 = 7;
    auto second = c::Layer::raster("Separate alpha detail", patterned({27,19}, true));
    second.localToDocument.m02 = 48;
    second.localToDocument.m12 = 39;
    check(aligned.insertLayer(0, std::move(first)) && aligned.insertLayer(1, std::move(second)),
        "insert exact raster merge fixture");
    compare(aligned, "Identity raster merge", 0, {3,-2});

    c::Document mixed({{96,80},96});
    auto backdrop = c::Layer::raster("Translucent backdrop", patterned({96,80}, false));
    auto transformed = c::Layer::raster("One source resample", patterned({37,29}, true));
    transformed.localToDocument = {.87,-.21,39.125,.18,1.09,25.375};
    transformed.opacity = .72f;
    transformed.blendMode = c::BlendMode::Multiply;
    check(mixed.insertLayer(0, std::move(backdrop)) && mixed.insertLayer(1, std::move(transformed)),
        "insert mixed merge raster sources");
    c::ShapeLayer ellipse;
    ellipse.kind = c::ShapeKind::Ellipse;
    ellipse.size = {39.5,25.75};
    ellipse.fillColor = {21,203,117,159};
    ellipse.strokeEnabled = true;
    ellipse.strokeWidth = 2.75;
    ellipse.strokeColor = {241,39,109,187};
    auto shape = c::Layer::shape("Direct transformed shape", ellipse);
    shape.localToDocument = {.93,-.27,21.75,.19,1.07,20.25};
    shape.opacity = .83f;
    shape.blendMode = c::BlendMode::SoftLight;
    shape.renderCache = u::QtShapeRenderService {}.renderDocument(ellipse, shape.localToDocument, 65536);
    c::TextLayer text;
    text.utf8 = "Aa ffi";
    text.defaultStyle.font = {"Noto Sans","Regular",400,false};
    text.defaultStyle.sizePixels = 18;
    text.defaultStyle.color = {245,123,17,163};
    text = c::normalizedText(text);
    auto label = c::Layer::text("Direct transformed text", text);
    label.localToDocument = {1,.13,8.375,-.09,1,43.125};
    label.opacity = .79f;
    label.blendMode = c::BlendMode::Screen;
    label.renderCache = u::QtTextLayout(text).rasterizeDocument(label.localToDocument, 65536);
    check(mixed.insertLayer(2, std::move(shape)) && mixed.insertLayer(3, std::move(label)),
        "insert mixed authoritative typed sources");
    // The live compositor remains linear-float until display. Baking introduces
    // one straight-RGBA8 quantization, so allow its two-byte display bound.
    // Native scale/integer pan compare the same document pixel footprint;
    // fractional viewport resampling and source-over do not generally commute.
    compare(mixed, "Mixed raster/shape/text merge", 2, {0,0});
}

void separatedMergeMatchesConsolidatedFramebuffer(OffscreenCanvas& gpu)
{
    c::Document original({{96,80},96});
    auto lower=c::Layer::raster("Selected opaque base",std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{96,80},c::Rgba8{220,60,30,255}));
    auto middle=c::Layer::raster("Unselected intervener",std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{96,80},c::Rgba8{20,220,90,255}));
    c::ShapeLayer shape;shape.kind=c::ShapeKind::Ellipse;shape.size={54.5,38.75};
    shape.fillColor={70,100,240,190};shape.strokeEnabled=true;shape.strokeWidth=2.5;
    auto upper=c::Layer::shape("Selected blend",shape);
    upper.localToDocument={.95,-.2,25.25,.15,1,13.5};upper.blendMode=c::BlendMode::Multiply;
    upper.renderCache=u::QtShapeRenderService{}.renderDocument(shape,upper.localToDocument,65536);
    const auto low=lower.id,high=upper.id;
    check(original.insertLayer(0,lower)&&original.insertLayer(1,middle)&&original.insertLayer(2,upper),"insert separated GPU fixture");
    c::Document reference(original.canvas());
    // Independently construct the documented order: surviving intervener below
    // the entire selected block. Do not demand equality with the original stack.
    check(reference.insertLayer(0,middle)&&reference.insertLayer(1,lower)&&reference.insertLayer(2,upper),"insert reference consolidated order");
    auto scene=sceneFor(reference);scene.viewport.setZoom(1);scene.viewport.setPan({0,0});
    const auto expected=gpu.render(scene);
    scene.document=original.snapshot();const auto before=gpu.render(scene);
    check(before!=expected,"interleaving fixture intentionally changes appearance");
    const auto merge=u::flattenLayerItems(original,std::array{high,low});
    check(bool(merge),"non-adjacent shielded Multiply merge is supported");if(!merge)return;
    const c::Extent2u extent{std::uint32_t(merge.image.width()),std::uint32_t(merge.image.height())};
    std::vector<std::byte> pixels(std::size_t(extent.width)*extent.height*4);
    for(std::uint32_t y=0;y<extent.height;++y)std::memcpy(pixels.data()+std::size_t(y)*extent.width*4,
        merge.image.constScanLine(int(y)),std::size_t(extent.width)*4);
    auto baked=c::Layer::raster("Merged",std::make_shared<c::ContiguousRasterSurface>(extent,std::move(pixels)));
    baked.localToDocument.m02=merge.origin.x;baked.localToDocument.m12=merge.origin.y;
    c::Document result(original.canvas());
    check(result.insertLayer(0,middle)&&result.insertLayer(1,std::move(baked)),"insert consolidated result");
    scene.document=result.snapshot();const auto actual=gpu.render(scene);
    int bad=0,worst=0;
    for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x) {
        const auto a=actual.pixelColor(x,y),b=expected.pixelColor(x,y);
        const auto delta=std::max({std::abs(a.red()-b.red()),std::abs(a.green()-b.green()),
            std::abs(a.blue()-b.blue()),std::abs(a.alpha()-b.alpha())});
        worst=std::max(worst,delta);if(delta>2)++bad;
    }
    check(!bad,"non-adjacent merge matches independent consolidated Vulkan scene");
    if(bad)std::cerr<<"Consolidated GPU mismatch pixels="<<bad<<" worst="<<worst<<'\n';
}

void pixelPreviewRendering(OffscreenCanvas& gpu)
{
    c::Document doc({{32,24},96});
    auto layer=c::Layer::raster("Source",patterned({32,24},true));
    check(doc.insertLayer(0,layer),"insert Pixel Preview source");
    const auto native=u::flattenDocument(doc);check(bool(native),"native preview source prepared");if(!native)return;
    auto scene=sceneFor(doc);scene.pixelPreviewEnabled=true;scene.pixelPreview=u::surfaceFromNativeImage(native.image);
    // The live presentation source is deliberately wrong: preview must read
    // only its canonical native buffer without altering the document snapshot.
    scene.document.layersBottomToTop[0].payload=c::RasterLayerSnapshot{
        std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{32,24},c::Rgba8{0,255,0,255})};
    const auto previewId=scene.pixelPreview->id();
    bool first=true;std::uint64_t uploaded=0;
    for(double dpr:{1.0,1.25,2.0})for(double zoom:{.4,.75,1.0,2.5,4.0}) {
        scene.logicalViewport={96/dpr,80/dpr};scene.devicePixelRatio=dpr;
        scene.viewport.setZoom(zoom);scene.viewport.setPan({.31,-.17});
        const auto actual=gpu.render(scene);const auto stats=gpu.stats();
        if(first){uploaded=stats.uploadedBytes;first=false;}else check(stats.uploadedBytes==uploaded,"preview pan/zoom/DPR reuse native texture");
        const r::CanvasCoordinateMapping mapping({32,24},scene.logicalViewport,{96,80},scene.viewport);
        const auto rect=mapping.documentCanvasFramebufferRect();
        const auto origin=mapping.documentToFramebuffer({0,0}),unit=mapping.documentToFramebuffer({1,1});
        const double sx=unit.x-origin.x,sy=unit.y-origin.y,fx=1/sx,fy=1/sy;
        int bad=0,worst=0;
        const auto texel=[&](int x,int y){const auto* p=native.image.constScanLine(y)+4*x;return c::decodeColor({p[0],p[1],p[2],p[3]});};
        for(int y=0;y<80;++y)for(int x=0;x<96;++x) {
            const c::Vec2d p{x+.5,y+.5};auto expected=scene.canvasBackground;
            if(p.x>=rect[0] && p.y>=rect[1] && p.x<rect[0]+rect[2] && p.y<rect[1]+rect[3]) {
                const double lx=(p.x-origin.x)/sx,ly=(p.y-origin.y)/sy;
                c::PremultipliedColor source{};
                if(fx<=1 && fy<=1)source=texel(int(std::floor(lx)),int(std::floor(ly)));
                else {
                    const double left=std::max(0.,lx-fx*.5),right=std::min(32.,lx+fx*.5);
                    const double top=std::max(0.,ly-fy*.5),bottom=std::min(24.,ly+fy*.5);
                    std::array<double,4> sum{};double weight=0;
                    for(int yy=int(std::floor(top));yy<int(std::ceil(bottom));++yy)
                        for(int xx=int(std::floor(left));xx<int(std::ceil(right));++xx) {
                            const double w=(std::min(right,double(xx+1))-std::max(left,double(xx)))
                                *(std::min(bottom,double(yy+1))-std::max(top,double(yy)));
                            const auto color=texel(xx,yy);for(std::size_t k=0;k<4;++k)sum[k]+=color[k]*w;weight+=w;
                        }
                    for(std::size_t k=0;k<4;++k)source[k]=float(sum[k]/weight);
                }
                const auto checker=((int(std::floor((p.x-rect[0])/12))+int(std::floor((p.y-rect[1])/12)))&1)?scene.checkerDark:scene.checkerLight;
                expected=c::encodeColor(c::compositeLayer(c::decodeColor(checker),source,1,c::BlendMode::Normal));
            }
            const auto a=actual.pixelColor(x,y),b=qt(expected);
            const auto delta=std::max({std::abs(a.red()-b.red()),std::abs(a.green()-b.green()),std::abs(a.blue()-b.blue())});
            // Native/magnified cells allow one display-encoding byte. Area
            // reduction additionally uses float GPU weights vs double oracle.
            worst=std::max(worst,delta);if(delta>(fx<=1 && fy<=1?1:2))++bad;
        }
        check(!bad,"native Pixel Preview nearest/area filtering matches independent CPU reference");
        if(bad)std::cerr<<"Pixel Preview zoom="<<zoom<<" dpr="<<dpr<<" bad="<<bad<<" worst="<<worst<<'\n';
    }
    const auto stats=gpu.stats();scene.cursorInside=true;scene.cursorLogical={35,30};scene.activeTool=c::ToolId::Brush;
    gpu.render(scene);check(gpu.stats().compositionPasses==stats.compositionPasses,"preview cursor uses independent overlay, not document recomposition");
    check(gpu.stats().uploadedBytes==uploaded && scene.pixelPreview->id()==previewId,"overlay keeps native texture unchanged");
}

#include "AdjustmentRenderingChecks.inc"
#include "LayerCropRenderingChecks.inc"

void benchmarks(bool validation)
{
    std::cout << "Optional blend benchmark: record+submit+fence wall time; no GPU readback,"
                 " no CPU reference-image scan, validation=" << (validation ? "on" : "off") << '\n';
    const auto mean = [](const std::vector<double>& values) {
        double sum = 0;
        for (auto v : values) sum += v;
        return sum / double(values.size());
    };
    for (const auto extent : {VkExtent2D{3840,2160},VkExtent2D{5120,2880}}) {
        c::Document document({{extent.width,extent.height},96});
        auto background = std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{extent.width,extent.height},c::Rgba8{83,141,217,223});
        auto middle = std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{extent.width,extent.height},c::Rgba8{211,89,27,137});
        auto foreground = std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{extent.width,extent.height},c::Rgba8{17,219,111,183});
        auto base = c::Layer::raster("Benchmark background",background);
        auto mid = c::Layer::raster("Benchmark multiply",middle);
        mid.blendMode = c::BlendMode::Multiply;
        mid.opacity = .81f;
        auto top = c::Layer::raster("Benchmark soft light",foreground);
        top.blendMode = c::BlendMode::SoftLight;
        top.opacity = .69f;
        const auto id = top.id;
        check(document.insertLayer(0,std::move(base)) && document.insertLayer(1,std::move(mid))
            && document.insertLayer(2,std::move(top)),"benchmark fixture insertion");
        auto scene = sceneFor(document);
        scene.logicalViewport = {double(extent.width),double(extent.height)};
        OffscreenCanvas gpu(validation,extent);
        gpu.render(scene,false);
        const auto cold = gpu.frameMilliseconds();
        const auto initial = gpu.stats();
        std::vector<double> modeTimes,editTimes,overlayTimes;
        for (auto mode : {c::BlendMode::Screen,c::BlendMode::Overlay,c::BlendMode::SoftLight,
                 c::BlendMode::ColorDodge,c::BlendMode::LinearDodge,c::BlendMode::ColorBurn,
                 c::BlendMode::LinearBurn,c::BlendMode::Subtract,c::BlendMode::Divide}) {
            document.setLayerBlendMode(id,mode);
            scene.document = document.snapshot();
            const auto before = gpu.stats();
            gpu.render(scene,false);
            modeTimes.push_back(gpu.frameMilliseconds());
            check(gpu.stats().compositionPasses > before.compositionPasses,"benchmark mode change recomposes");
            check(gpu.stats().uploadedBytes == initial.uploadedBytes,"benchmark mode changes have zero image upload");
        }
        std::array<std::byte,16*16*4> patch {};
        for (int edit = 0; edit < 3; ++edit) {
            for (std::size_t pixel = 0; pixel < patch.size(); pixel += 4) {
                patch[pixel] = std::byte(211-edit*31);
                patch[pixel+1] = std::byte(19+edit*23);
                patch[pixel+2] = std::byte(73);
                patch[pixel+3] = std::byte(143);
            }
            const auto before = gpu.stats();
            foreground->replaceRgba8({96+edit*24,96,16,16},patch,16*4);
            scene.document = document.snapshot();
            gpu.render(scene,false);
            editTimes.push_back(gpu.frameMilliseconds());
            check(gpu.stats().compositionPasses > before.compositionPasses,"benchmark changed pixels recompose");
            check(gpu.stats().fullUploads == before.fullUploads,"small high-res edits never upload a full layer");
            check(gpu.stats().uploadedBytes-before.uploadedBytes == patch.size(),
                "small high-res edit uploads only the changed patch");
        }
        const auto composed = gpu.stats();
        for (int frame = 0; frame < 4; ++frame) {
            scene.cursorInside = true;
            scene.activeTool = c::ToolId::Brush;
            scene.cursorLogical = {400.0+frame*23,300};
            scene.brushSizeDocument = 120;
            scene.selectionPhase = .2*frame;
            gpu.render(scene,false);
            overlayTimes.push_back(gpu.frameMilliseconds());
        }
        const auto done = gpu.stats();
        check(done.compositionPasses == composed.compositionPasses
            && done.compositionDispatches == composed.compositionDispatches,
            "high-res overlay frames do not recompose content");
        check(done.uploadedBytes == composed.uploadedBytes,"high-res overlay frames do not upload layer images");
        std::cout << extent.width << 'x' << extent.height << " 3 full-size alpha layers: cold=" << cold
                  << " ms; mode mean (" << modeTimes.size() << " modes)=" << mean(modeTimes) << " ms; 16x16 edit mean=" << mean(editTimes)
                  << " ms; overlay mean=" << mean(overlayTimes) << " ms; composition allocation="
                  << double(done.compositionBytes)/(1024*1024) << " MiB; image uploaded="
                  << double(done.uploadedBytes)/(1024*1024) << " MiB; composition passes="
                  << done.compositionPasses << "; dispatches=" << done.compositionDispatches << '\n';
    }
}
void spatialFilterRendering(OffscreenCanvas& gpu,Review& review)
{
    c::Document document({{72,56},96});
    auto layer=c::Layer::raster("Spatial source",patterned({28,24},true));
    layer.localToDocument={1.4,.15,10,-.2,1.1,10};
    layer.opacity=.63F;layer.blendMode=c::BlendMode::Screen;
    const auto id=layer.id;document.insertLayer(0,layer);
    auto scene=sceneFor(document);gpu.render(scene);
    const auto originalUploads=gpu.stats().fullUploads;
    for(const auto type:c::allSpatialFilterTypes) {
        auto state=std::make_shared<c::SpatialFilterStack>();
        auto& filter=state->items[std::size_t(type)];filter.enabled=true;
        if(type==c::SpatialFilterType::Gaussian)filter.parameters=c::GaussianBlurParameters{3,5};
        if(type==c::SpatialFilterType::Motion)filter.parameters=c::MotionBlurParameters{9.5,37};
        if(type==c::SpatialFilterType::Lens)filter.parameters=c::LensBlurParameters{4.5,5,13};
        auto adjustments=std::make_shared<c::AdjustmentStack>();
        adjustments->items[0].enabled=true;adjustments->items[0].parameters=c::ExposureParameters{.7};
        document.setLayerAdjustments(id,adjustments);document.setLayerFilters(id,state);
        auto cache=c::prepareLayerSpatialFilters(*document.layer(id));
        check(c::publishLayerSpatialFilters(document,id,cache),"publish GPU filter cache");
        scene.document=document.snapshot();
        const auto actual=gpu.render(scene);
        const auto expected=verify(document,scene,actual,c::spatialFilterName(type),false);
        review.add(std::size_t(type),c::spatialFilterName(type),actual,expected);
        const auto before=gpu.stats();
        scene.cursorInside=true;scene.cursorLogical={35,35};
        gpu.render(scene,false);
        check(gpu.stats().uploadedBytes==before.uploadedBytes,"filter cursor update has no upload");
        check(gpu.stats().compositionPasses==before.compositionPasses,"filter cursor update reuses composition");
        scene.filterBypassLayer=id;gpu.render(scene,false);
        check(gpu.stats().uploadedBytes==before.uploadedBytes,"filter bypass retains original GPU texture");
        scene.filterBypassLayer.reset();
        document.setLayerCrop(id,c::LayerCrop{2,3,23,17});
        scene.document=document.snapshot();
        verify(document,scene,gpu.render(scene),"filtered crop",false);
        document.setLayerCrop(id,{});
    }
    check(gpu.stats().fullUploads==originalUploads+3,"each filter result uploads once, original never reuploads");
}
#include "LayerEffectRenderingChecks.inc"
void layerMaskRendering(OffscreenCanvas& gpu)
{
    c::Document doc({{96,80},96});
    auto layer=c::Layer::raster("Masked",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{96,80},c::Rgba8{90,160,220,170}));
    const auto id=layer.id;check(doc.insertLayer(0,layer),"insert mask fixture");
    auto scene=sceneFor(doc);gpu.render(scene);const auto originalUploads=gpu.stats().uploadedBytes;
    for(uint8_t value:{uint8_t(255),uint8_t(128),uint8_t(0)}) {
        auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::rectangle({96,80},{8,10,49,50},value);
        mask->localToMask={1,0,.25,0,1,.5};mask->outside=0;
        check(doc.setLayerMask(id,mask),"set coverage");
        scene.document=doc.snapshot();verify(doc,scene,gpu.render(scene),"layer mask fractional coverage");
        check(gpu.stats().uploadedBytes==originalUploads,"mask paint never uploads source RGBA");
        const auto before=gpu.stats();gpu.render(scene);
        check(gpu.stats().compositionPasses==before.compositionPasses,"unchanged layer mask reuses composition");
        mask=std::make_shared<c::LayerMask>(*mask);mask->enabled=false;doc.setLayerMask(id,mask);
        scene.document=doc.snapshot();verify(doc,scene,gpu.render(scene),"disabled layer mask identity");
    }
    auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::rectangle({96,80},{15,11,54,53},128);mask->outside=255;
    doc.setLayerMask(id,mask);
    auto effects=std::make_shared<c::LayerEffectStack>();effects->items[0].enabled=true;effects->items[1].enabled=true;
    auto styled=*doc.layer(id);styled.effects=effects;styled=c::prepareSpatialFilterLayer(styled);
    c::Document effectDoc(doc.canvas());check(effectDoc.insertLayer(0,styled),"insert styled mask fixture");
    scene=sceneFor(effectDoc);verify(effectDoc,scene,gpu.render(scene),"mask multiplies complete styled result once");
    auto projective=*c::rectangleToQuad({0,0,96,80},{{{1,2},{84,8},{91,72},{5,77}}});
    effectDoc.setLayerTransform(id,projective);scene=sceneFor(effectDoc);
    verify(effectDoc,scene,gpu.render(scene),"projective layer mask");
}
void clippingRendering(OffscreenCanvas& gpu)
{
    {
        c::Document doc({{96,80},96});
        auto below=c::Layer::raster("Input",patterned({96,80},true));
        auto adjustment=c::Layer::adjustment("Stack exposure");
        auto state=std::make_shared<c::AdjustmentStack>();state->items[0].enabled=true;state->items[0].parameters=c::ExposureParameters{1};
        adjustment.adjustments=state;
        doc.insertLayer(0,below);doc.insertLayer(1,adjustment);
        auto scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"adjustment layer float composite");
        for(float strength:{0.0F,.5F,1.0F}) {
            doc.setLayerOpacity(adjustment.id,strength);
            auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::filled({96,80},128);mask->outside=0;
            doc.setLayerMask(adjustment.id,mask);
            scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"adjustment layer soft strength");
        }
        const auto before=gpu.stats();scene=sceneFor(doc);gpu.render(scene);
        check(gpu.stats().uploadedBytes==before.uploadedBytes && gpu.stats().compositionPasses==before.compositionPasses,"adjustment layer idle/source reuse");
        auto changed=std::make_shared<c::AdjustmentStack>(*state);changed->items[0].parameters=c::ExposureParameters{2};
        doc.setLayerAdjustments(adjustment.id,changed);scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"adjustment cached lower input");
        check(gpu.stats().adjustmentInputBuilds==before.adjustmentInputBuilds && gpu.stats().adjustmentInputReuses>before.adjustmentInputReuses,"parameter edit reuses lower composite");
        check(gpu.stats().uploadedBytes==before.uploadedBytes,"parameter edit uploads no source pixels");
        auto above=c::Layer::raster("Above",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{16,16},c::Rgba8{180,20,90,128}));
        doc.insertLayer(doc.layers().size(),above);const auto beforeAbove=gpu.stats();scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"above operator input reuse");
        check(gpu.stats().adjustmentInputBuilds==beforeAbove.adjustmentInputBuilds,"above edit retains lower input");
        check(doc.takeLayer(above.id).has_value(),"remove above fixture");
        const auto group=c::makeLayerId();auto tree=doc.tree();tree.roots={group};
        tree.containers.push_back({group,"Local",c::ContainerKind::Folder,c::ColorLabel::None,{below.id,adjustment.id}});
        doc.replaceStructure(doc.tree(),tree);doc.setAdjustmentScope(adjustment.id,c::AdjustmentScope::ThisGroup);
        scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"adjustment local domain");
        doc.setLayerVisibility(adjustment.id,false);scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"hidden adjustment retains local domain");
        doc.setLayerVisibility(adjustment.id,true);tree=doc.tree();tree.container(group)->kind=c::ContainerKind::ClippingMaskGroup;
        doc.replaceStructure(doc.tree(),tree);scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"adjustment within clipping domain");
        tree=doc.tree();std::swap(tree.container(group)->children[0],tree.container(group)->children[1]);doc.replaceStructure(doc.tree(),tree);
        scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"adjustment cannot provide clipping base coverage");
        tree=doc.tree();tree.container(group)->kind=c::ContainerKind::Folder;std::swap(tree.container(group)->children[0],tree.container(group)->children[1]);
        doc.replaceStructure(doc.tree(),tree);doc.setAdjustmentScope(adjustment.id,c::AdjustmentScope::AllBelow);
        scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"pass-through adjustment domain");
        const auto pass=gpu.stats();doc.setLayerAdjustments(adjustment.id,state);scene=sceneFor(doc);gpu.render(scene);
        check(gpu.stats().adjustmentInputBuilds==pass.adjustmentInputBuilds,"pass-through nested input reused");
    }
    c::Document doc({{96,80},96});
    auto base=c::Layer::raster("Base",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{48,45},c::Rgba8{130,90,180,128}));
    base.localToDocument={1,0,12,0,1,13};base.opacity=.6F;
    auto upper=c::Layer::raster("Upper",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{75,70},c::Rgba8{220,140,60,210}));
    auto second=c::Layer::raster("Second",std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{32,40},c::Rgba8{50,180,210,190}));
    second.localToDocument={1,0,30,0,1,20};
    auto outside=c::Layer::raster("Unrelated backdrop",patterned({96,80},false));
    for(const auto& l:{outside,base,upper,second})check(doc.insertLayer(doc.layers().size(),l),"clip fixture insertion");
    auto tree=doc.tree();const auto group=c::makeLayerId();tree.roots={outside.id,group};
    tree.containers.push_back({group,"Clipping",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{base.id,upper.id,second.id}});
    check(doc.replaceStructure(doc.tree(),tree),"clip fixture hierarchy");
    for(auto mode:c::allBlendModes) {
        doc.setLayerBlendMode(upper.id,mode);doc.setLayerBlendMode(base.id,mode);
        auto scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"clipping mode");
        const auto before=gpu.stats();gpu.render(scene);
        check(gpu.stats().uploadedBytes==before.uploadedBytes && gpu.stats().compositionPasses==before.compositionPasses,"clipping idle reuse");
    }
    doc.setLayerBlendMode(base.id,c::BlendMode::Normal);
    auto cachedScene=sceneFor(doc);gpu.render(cachedScene);
    const auto cached=gpu.stats();
    doc.setLayerOpacity(second.id,.5F);cachedScene=sceneFor(doc);gpu.render(cachedScene);
    check(gpu.stats().clippingBaseBuilds==cached.clippingBaseBuilds && gpu.stats().clippingBaseReuses>cached.clippingBaseReuses,"upper-only edit reuses clipping base");
    check(gpu.stats().uploadedBytes==cached.uploadedBytes,"clipping edit never reuploads unchanged source");
    tree=doc.tree();std::swap(tree.container(group)->children[1],tree.container(group)->children[2]);
    check(doc.replaceStructure(doc.tree(),tree),"reorder only upper members");
    const auto beforeReorder=gpu.stats();cachedScene=sceneFor(doc);verify(doc,cachedScene,gpu.render(cachedScene),"reordered upper members");
    check(gpu.stats().clippingBaseBuilds==beforeReorder.clippingBaseBuilds,"upper reorder reuses base coverage");
    auto mask=std::make_shared<c::LayerMask>();mask->coverage=c::SelectionMask::rectangle({48,45},{5,5,30,30},128);mask->outside=0;
    doc.setLayerMask(base.id,mask);doc.setLayerCrop(base.id,c::LayerCrop{2,2,40,40});
    auto scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"clipping soft mask and crop");
    check(gpu.stats().clippingBaseBuilds>beforeReorder.clippingBaseBuilds,"base mask and crop invalidate coverage");
    doc.setLayerTransform(base.id,*c::rectangleToQuad({0,0,48,45},{{{8,12},{66,8},{59,71},{12,55}}}));
    scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"clipping projective base");
    doc.setLayerVisibility(base.id,false);scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"hidden clipping base");
    doc.setLayerVisibility(base.id,true);
    // Direct container base is evaluated as a subtree, not one descendant.
    tree=doc.tree();const auto nested=c::makeLayerId();
    tree.containers.push_back({nested,"Base subtree",c::ContainerKind::Folder,c::ColorLabel::None,{base.id,upper.id}});
    tree.container(group)->children={nested,second.id};check(doc.replaceStructure(doc.tree(),tree),"container base");
    scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"container clipping base");
    tree.container(nested)->kind=c::ContainerKind::ClippingMaskGroup;check(doc.replaceStructure(doc.tree(),tree),"nested clipping");
    scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"nested clipping base");
    const auto beforeContainerEdit=gpu.stats();doc.setLayerOpacity(second.id,.3F);
    scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"cached container base");
    check(gpu.stats().clippingBaseBuilds==beforeContainerEdit.clippingBaseBuilds
        &&gpu.stats().clippingBaseReuses>beforeContainerEdit.clippingBaseReuses,"upper edits reuse complete container base");
    for(std::size_t type=0;type<c::layerEffectCount;++type) {
        auto effects=std::make_shared<c::LayerEffectStack>();auto& effect=effects->items[type];
        effect.enabled=true;effect.size=4.5;effect.distance=6;effect.spread=.2;effect.color={220,80,20,190};
        doc.setLayerEffects(base.id,effects);doc.setLayerEffects(second.id,effects);
        *doc.layer(base.id)=c::prepareSpatialFilterLayer(*doc.layer(base.id));
        *doc.layer(second.id)=c::prepareSpatialFilterLayer(*doc.layer(second.id));
        scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"nested clipping base and upper styles");
        const c::PinnedDocumentSampler sampler(doc,{},c::ColorSampleSource::MergedVisible);
        std::array<c::PremultipliedColor,96> row;
        for(int y:{2,18,40,65}) {
            sampler.sampleRow(0,y,row);
            for(int x=0;x<96;++x)check(c::encodeColor(row[std::size_t(x)])==sampler.sample({x+.5,y+.5}),"clipping scanline matches scalar styles");
        }
    }
    tree=doc.tree();tree.container(nested)->children.clear();
    tree.roots={outside.id,base.id,upper.id,group};
    check(doc.replaceStructure(doc.tree(),tree),"empty direct container base");
    scene=sceneFor(doc);verify(doc,scene,gpu.render(scene),"empty base must not promote upper");
    c::Document textDoc({{96,80},96});
    c::TextLayer text;text.utf8="O8";text.defaultStyle.font.family="Noto Sans";text.defaultStyle.sizePixels=54;
    text.defaultStyle.color={255,255,255,255};auto title=c::Layer::text("Editable base",c::normalizedText(text));
    title.localToDocument={1,.15,4,-.1,1,15};title.renderCache=u::prepareDocumentSampleCache(title,1024*1024);
    auto photo=c::Layer::raster("Photo",patterned({96,80},false));
    c::ShapeLayer shape;shape.kind=c::ShapeKind::Ellipse;shape.size={35,28};shape.fillColor={220,120,40,190};
    auto highlight=c::Layer::shape("Highlight",shape);highlight.localToDocument={1,0,25,0,1,30};highlight.blendMode=c::BlendMode::Screen;
    highlight.renderCache=u::prepareDocumentSampleCache(highlight,1024*1024);
    for(const auto& l:{title,photo,highlight})textDoc.insertLayer(textDoc.layers().size(),l);
    auto textTree=textDoc.tree();const auto textGroup=c::makeLayerId();textTree.roots={textGroup};
    textTree.containers.push_back({textGroup,"Text clipping",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{title.id,photo.id,highlight.id}});
    check(textDoc.replaceStructure(textDoc.tree(),textTree),"typed clipping fixture");
    scene=sceneFor(textDoc);verify(textDoc,scene,gpu.render(scene),"photo and highlights clipped into editable text",false);
    scene.logicalViewport={76.8,64};scene.viewport.setZoom(1.2);
    verify(textDoc,scene,gpu.render(scene),"clipping fractional DPI",false);
}
void clippingBenchmarks(bool validation)
{
    for(const auto extent:{c::Extent2u{3840,2160},c::Extent2u{5120,2880}}) {
        OffscreenCanvas gpu(validation,{extent.width,extent.height});c::Document doc({extent,96});
        auto base=c::Layer::raster("Base",std::make_shared<c::ContiguousRasterSurface>(extent,c::Rgba8{80,130,190,128}));
        auto upper=c::Layer::raster("Upper",std::make_shared<c::ContiguousRasterSurface>(extent,c::Rgba8{210,130,45,180}));
        doc.insertLayer(0,base);doc.insertLayer(1,upper);auto tree=doc.tree();const auto id=c::makeLayerId();tree.roots={id};
        tree.containers.push_back({id,"Clipping",c::ContainerKind::ClippingMaskGroup,c::ColorLabel::None,{base.id,upper.id}});doc.replaceStructure(doc.tree(),tree);
        auto scene=sceneFor(doc);scene.logicalViewport={double(extent.width),double(extent.height)};
        gpu.render(scene,false);const auto cold=gpu.frameMilliseconds();const auto before=gpu.stats();
        double warm=0;
        for(int i=0;i<8;++i){doc.setLayerOpacity(upper.id,.6F+float(i)*.03F);scene.document=doc.snapshot();gpu.render(scene,false);warm+=gpu.frameMilliseconds();}
        check(gpu.stats().clippingBaseBuilds==before.clippingBaseBuilds,"large upper edits retain base cache");
        check(gpu.stats().uploadedBytes==before.uploadedBytes,"large upper edits retain source textures");
        const auto idle=gpu.stats();gpu.render(scene,false);check(gpu.stats().compositionPasses==idle.compositionPasses,"large clipping idle reuse");
        std::cout<<"Clipping "<<extent.width<<'x'<<extent.height<<" cold_ms "<<cold<<" warm_ms "<<warm/8
            <<" work_bytes "<<gpu.stats().clippingWorkingBytes<<" retained_buffer_bytes "<<gpu.stats().adjustmentBufferCapacity<<'\n';
    }
}
void adjustmentLayerBenchmarks(bool validation)
{
    for(const auto extent:{c::Extent2u{3840,2160},c::Extent2u{5120,2880}}) {
        OffscreenCanvas gpu(validation,{extent.width,extent.height});c::Document doc({extent,96});
        for(int i=0;i<4;++i)doc.insertLayer(doc.layers().size(),c::Layer::raster("Input",std::make_shared<c::ContiguousRasterSurface>(extent,c::Rgba8{uint8_t(40+i*35),80,140,128})));
        const auto raster=doc.layers().front().id;
        c::LayerId target{};
        for(int i=0;i<3;++i) {
            auto layer=c::Layer::adjustment("Correction");auto s=std::make_shared<c::AdjustmentStack>();
            s->items[0].enabled=true;s->items[0].parameters=c::ExposureParameters{.2};layer.adjustments=s;
            if(!target)target=layer.id;
            doc.insertLayer(doc.layers().size(),layer);
        }
        const auto start=std::chrono::steady_clock::now();auto scene=sceneFor(doc);gpu.render(scene,false);
        const auto cold=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const auto before=gpu.stats();double scrub=0;
        for(int i=0;i<12;++i) {
            auto s=std::make_shared<c::AdjustmentStack>();s->items[0].enabled=true;s->items[0].parameters=c::ExposureParameters{.2+i*.02};
            doc.setLayerAdjustments(target,s);const auto tick=std::chrono::steady_clock::now();scene=sceneFor(doc);gpu.render(scene,false);
            scrub+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tick).count();
        }
        check(gpu.stats().uploadedBytes==before.uploadedBytes,"large adjustment scrub reuses textures");
        check(gpu.stats().adjustmentInputBuilds==before.adjustmentInputBuilds,"large adjustment scrub reuses lower input");
        const auto warm=gpu.stats();
        auto& surface=*std::get<c::RasterLayer>(doc.layer(raster)->payload).surface;
        std::array<std::byte,4> pixel{std::byte{210},std::byte{80},std::byte{10},std::byte{255}};
        surface.replaceRgba8({10,10,1,1},pixel,4);
        const auto tick=std::chrono::steady_clock::now();scene=sceneFor(doc);gpu.render(scene,false);
        const auto paint=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tick).count();
        check(gpu.stats().adjustmentInputBuilds>warm.adjustmentInputBuilds,"lower edit invalidates input");
        const auto idle=gpu.stats();gpu.render(scene,false);check(gpu.stats().compositionPasses==idle.compositionPasses,"adjustment idle reuse");
        std::cout<<"AdjustmentLayers "<<extent.width<<'x'<<extent.height<<" cold_ms "<<cold<<" scrub_ms "<<scrub/12<<" lower_edit_ms "<<paint
            <<" input_reuses "<<gpu.stats().adjustmentInputReuses<<" work_bytes "<<gpu.stats().clippingWorkingBytes<<" retained_buffer_bytes "<<gpu.stats().adjustmentBufferCapacity<<'\n';
    }
}
} // namespace

int main(int argc, char** argv)
{
    bool validation = false, benchmark = false, adjustments = false, crop = false, filters = false,effects=false,masks=false,clipping=false,adjustmentLayers=false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--validation") validation = true;
        if (std::string_view(argv[i]) == "--benchmark") benchmark = true;
        if (std::string_view(argv[i]) == "--adjustments") adjustments = true;
        if (std::string_view(argv[i]) == "--crop") crop = true;
        if (std::string_view(argv[i]) == "--filters") filters = true;
        if (std::string_view(argv[i]) == "--effects") effects = true;
        if (std::string_view(argv[i]) == "--masks") masks = true;
        if (std::string_view(argv[i]) == "--clipping") clipping = true;
        if (std::string_view(argv[i]) == "--adjustment-layers") adjustmentLayers = true;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM","offscreen");
    QGuiApplication app(argc,argv);
    QCoreApplication::setApplicationName("VulkanaBlendRenderingTests");
    try {
        if (benchmark && effects) bevelBenchmarks(validation);
        else if (benchmark && adjustmentLayers) adjustmentLayerBenchmarks(validation);
        else if (benchmark && clipping) clippingBenchmarks(validation);
        else if (benchmark && adjustments) adjustmentBenchmarks(validation);
        else if (benchmark) benchmarks(validation);
        else {
            loadFontFixture();
            Review review(effects?c::layerEffectCount:filters?c::spatialFilterCount:adjustments?c::adjustmentCount+2:c::allBlendModes.size());
            {
                OffscreenCanvas gpu(validation);
                if(clipping)clippingRendering(gpu);
                else if(masks)layerMaskRendering(gpu);
                else if(effects)layerEffectRendering(gpu,review);
                else if(filters)spatialFilterRendering(gpu,review);
                else if (crop) layerCropRendering(gpu);
                else if (adjustments) {
                    adjustmentRendering(gpu,review);
                    capturedRegionRendering(gpu);
                    adjustmentTypedLayers(gpu);
                    adjustmentRamps(gpu,review);
                    adjustmentResourceLimits(gpu);
                } else {
                    allModesAndCacheBehavior(gpu,review);
                    arithmeticEndpointsAndGradients(gpu);
                    whiteBackdropEndpointStacks(gpu);
                    typedStacksAndPassThrough(gpu);
                    mergePreservesNativeFramebuffer(gpu);
                    separatedMergeMatchesConsolidatedFramebuffer(gpu);
                    pixelPreviewRendering(gpu);
                    projectiveRendering(gpu);
                }
            }
            review.save();
        }
        check(validationMessages == 0,"Vulkan validation must remain clean, including destruction");
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "Blend rendering test aborted: " << error.what() << '\n';
    }
    if (failures) std::cerr << failures << " blend-rendering checks failed\n";
    else if (benchmark) std::cout << "Optional 4K/5K blend benchmark and cache/upload assertions passed\n";
    else if(masks) std::cout << "Vulkan layer masks: CPU agreement, fractional/projective coverage, styles, disable, source upload and idle reuse passed\n";
    else if(filters) std::cout << "Vulkan spatial filters: padded cache, CPU agreement, crop, blending, comparison and texture reuse passed\n";
    else if(crop) std::cout << "Vulkan layer crop: typed sources, alpha edges, zoom/DPR, preview and texture reuse passed\n";
    else std::cout << (adjustments
        ? "Vulkan adjustments: all ten stages, combinations, ramps/endpoints, alpha edges, captured masks, typed content and cache reuse passed\n"
        : "Vulkan blends: all modes, alpha edges, transformed typed content, pass-through groups, and cache reuse passed\n");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
