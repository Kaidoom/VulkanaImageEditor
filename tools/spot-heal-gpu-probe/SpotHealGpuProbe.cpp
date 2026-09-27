// Original developer-only probe. It does not implement a new Spot Heal solver
// or advertise application GPU acceleration; see the accompanying README.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void check(VkResult result, std::string_view operation)
{
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
unsigned validationMessages = 0;
VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT,
    std::uint64_t, std::size_t, std::int32_t, const char*, const char* message, void*)
{
    ++validationMessages;
    std::cerr << "Validation: " << message << '\n';
    return VK_FALSE;
}

struct alignas(32) Pixel {
    std::array<double, 4> colorAlpha {};
    std::array<float, 2> texture {};
    double baseWeight {1};
    std::uint32_t gradient {1};
    std::array<std::uint32_t, 3> padding {};
};
static_assert(sizeof(Pixel) == 64 && offsetof(Pixel, baseWeight) == 40 && offsetof(Pixel, gradient) == 48);
struct alignas(16) Candidate {
    std::array<std::uint32_t, 4> offsets {};
    double locality {};
    std::array<std::uint32_t, 2> reserved {};
};
static_assert(sizeof(Candidate) == 32);
struct Settings { std::uint32_t count, radius, width, reserved; };

double cpuScore(const std::vector<Pixel>& target, const std::vector<Pixel>& original,
    const Candidate& candidate, Settings settings)
{
    double cost = 0, weights = 0;
    auto sample = candidate.offsets[0];
    for (unsigned y = 0; y <= 2 * settings.radius; ++y) {
        auto source = candidate.offsets[1] + y * settings.width;
        for (unsigned x = 0; x <= 2 * settings.radius; ++x, ++source, ++sample) {
            const auto& a = target[sample];
            const auto& b = original[source];
            if (a.baseWeight == 0) continue;
            double difference = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const double d = a.colorAlpha[c] - b.colorAlpha[c];
                difference += d * d;
            }
            const double da = float(a.colorAlpha[3]) - float(b.colorAlpha[3]);
            difference = difference / 3 + .15 * da * da;
            if (a.gradient)
                for (std::size_t c = 0; c < 2; ++c) {
                    const double gradient = a.texture[c] - b.texture[c];
                    difference += .65 * gradient * gradient;
                }
            const double weight = a.baseWeight * std::min(float(a.colorAlpha[3]), float(b.colorAlpha[3]));
            cost += weight * difference;
            weights += weight;
        }
    }
    return weights < .25 ? std::numeric_limits<double>::infinity() : cost / weights + candidate.locality;
}

struct Buffer {
    VkBuffer buffer {VK_NULL_HANDLE};
    VkDeviceMemory memory {VK_NULL_HANDLE};
    void* mapped {nullptr};
    VkDeviceSize bytes {};
};

class Probe {
public:
    explicit Probe(bool validation)
    {
        const auto start = Clock::now();
        try { initialize(validation); }
        catch (...) { destroy(); throw; }
        std::cout << "device_pipeline_cold_ms=" << ms(start) << '\n';
    }
    ~Probe() { destroy(); }

    void upload(const std::vector<Pixel>& target, const std::vector<Pixel>& original,
        const std::vector<Candidate>& candidates)
    {
        const auto start = Clock::now();
        const std::array sizes {target.size() * sizeof(Pixel), original.size() * sizeof(Pixel),
            candidates.size() * sizeof(Candidate), candidates.size() * sizeof(double)};
        if (buffers_[0].buffer) throw std::runtime_error("probe upload called twice");
        VkDeviceSize total = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            buffers_[i] = buffer(sizes[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (i != 3) total += sizes[i];
        }
        staging_ = buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        readback_ = buffer(sizes[3], VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        std::array<const void*, 3> source {target.data(), original.data(), candidates.data()};
        VkDeviceSize offset = 0;
        begin();
        for (std::size_t i = 0; i < 3; ++i) {
            std::memcpy(static_cast<char*>(staging_.mapped) + offset, source[i], sizes[i]);
            VkBufferCopy copy {offset, 0, sizes[i]};
            vkCmdCopyBuffer(command_, staging_.buffer, buffers_[i].buffer, 1, &copy);
            offset += sizes[i];
        }
        VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
        submit();
        std::array<VkDescriptorBufferInfo, 4> info {};
        std::array<VkWriteDescriptorSet, 4> writes {};
        for (std::uint32_t i = 0; i < 4; ++i) {
            info[i] = {buffers_[i].buffer, 0, VK_WHOLE_SIZE};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = descriptors_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &info[i];
        }
        vkUpdateDescriptorSets(device_, 4, writes.data(), 0, nullptr);
        std::cout << "buffer_allocate_upload_cold_ms=" << ms(start) << ",input_bytes=" << total
                  << ",output_capacity_bytes=" << sizes[3] << '\n';
    }

    struct Timing { double endToEndMs {}, deviceMs {}; };
    Timing score(Settings settings, std::span<double> output)
    {
        const auto start = Clock::now();
        begin();
        vkCmdResetQueryPool(command_, queries_, 0, 2);
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &descriptors_, 0, nullptr);
        vkCmdPushConstants(command_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(settings), &settings);
        vkCmdDispatch(command_, (settings.count + 63) / 64, 1, 1);
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries_, 1);
        VkMemoryBarrier transfer {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &transfer, 0, nullptr, 0, nullptr);
        const VkBufferCopy copy {0, 0, VkDeviceSize(settings.count) * sizeof(double)};
        vkCmdCopyBuffer(command_, buffers_[3].buffer, readback_.buffer, 1, &copy);
        VkMemoryBarrier host {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
        submit();
        std::memcpy(output.data(), readback_.mapped, std::size_t(copy.size));
        const double elapsed = ms(start);
        std::array<std::uint64_t, 2> timestamps {};
        check(vkGetQueryPoolResults(device_, queries_, 0, 2, sizeof(timestamps), timestamps.data(),
            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT), "read device timestamps");
        return {elapsed, double(timestamps[1] - timestamps[0]) * timestampPeriod_ / 1.0e6};
    }

private:
    Buffer buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags)
    {
        Buffer value;
        value.bytes = bytes;
        VkBufferCreateInfo create {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = bytes;
        create.usage = usage;
        check(vkCreateBuffer(device_, &create, nullptr, &value.buffer), "create buffer");
        VkMemoryRequirements required {};
        vkGetBufferMemoryRequirements(device_, value.buffer, &required);
        VkPhysicalDeviceMemoryProperties properties {};
        vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
        std::uint32_t type = properties.memoryTypeCount;
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((required.memoryTypeBits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) { type = i; break; }
        if (type == properties.memoryTypeCount) throw std::runtime_error("No suitable buffer memory");
        VkMemoryAllocateInfo allocate {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = required.size;
        allocate.memoryTypeIndex = type;
        check(vkAllocateMemory(device_, &allocate, nullptr, &value.memory), "allocate buffer memory");
        check(vkBindBufferMemory(device_, value.buffer, value.memory, 0), "bind buffer");
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            check(vkMapMemory(device_, value.memory, 0, VK_WHOLE_SIZE, 0, &value.mapped), "map staging/readback");
        return value;
    }

    void begin()
    {
        check(vkResetCommandPool(device_, pool_, 0), "reset command pool");
        VkCommandBufferBeginInfo info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &info), "begin compute");
    }
    void submit()
    {
        check(vkEndCommandBuffer(command_), "end compute");
        check(vkResetFences(device_, 1, &fence_), "reset fence");
        VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        check(vkQueueSubmit(queue_, 1, &submit, fence_), "submit compute");
        // Fence waits are bounded to 1 ms; no device/queue-wide idle wait.
        auto status = VK_TIMEOUT;
        const auto started = Clock::now();
        while (status == VK_TIMEOUT && ms(started) < 10'000)
            status = vkWaitForFences(device_, 1, &fence_, VK_TRUE, 1'000'000);
        check(status, "wait compute fence");
    }

    void initialize(bool validation)
    {
        VkApplicationInfo application {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Vulkana Spot Heal scorer probe";
        application.apiVersion = VK_API_VERSION_1_2;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
        VkInstanceCreateInfo instance {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance.pApplicationInfo = &application;
        if (validation) {
            instance.enabledLayerCount = instance.enabledExtensionCount = 1;
            instance.ppEnabledLayerNames = &layer;
            instance.ppEnabledExtensionNames = &extension;
        }
        check(vkCreateInstance(&instance, nullptr, &instance_), "create instance");
        if (validation) {
            VkDebugReportCallbackCreateInfoEXT info {VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT};
            info.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
            info.pfnCallback = report;
            const auto create = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugReportCallbackEXT"));
            check(create(instance_, &info, nullptr, &debug_), "create validation callback");
        }
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "read GPUs");
        for (auto candidate : devices) {
            VkPhysicalDeviceFeatures features {};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (!features.shaderFloat64) continue;
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t i = 0; i < familyCount; ++i)
                if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && families[i].timestampValidBits) {
                    physical_ = candidate; family_ = i;
                    if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break;
                }
            if (physical_) break;
        }
        if (!physical_) throw std::runtime_error("No timestamp-capable FP64 Vulkan compute device");
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        timestampPeriod_ = properties.limits.timestampPeriod;
        std::cout << "device=" << properties.deviceName << ",queue_family=" << family_ << ",shader_float64=1\n";
        const float priority = .5F;
        VkDeviceQueueCreateInfo queue {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family_; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features {}; features.shaderFloat64 = VK_TRUE;
        VkDeviceCreateInfo device {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue; device.pEnabledFeatures = &features;
        check(vkCreateDevice(physical_, &device, nullptr, &device_), "create compute device");
        vkGetDeviceQueue(device_, family_, 0, &queue_);
        VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex = family_;
        check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "create command pool");
        VkCommandBufferAllocateInfo command {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = pool_; command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &command, &command_), "allocate command buffer");
        VkFenceCreateInfo fence {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device_, &fence, nullptr, &fence_), "create fence");
        VkQueryPoolCreateInfo query {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; query.queryType = VK_QUERY_TYPE_TIMESTAMP; query.queryCount = 2;
        check(vkCreateQueryPool(device_, &query, nullptr, &queries_), "create queries");
        std::array<VkDescriptorSetLayoutBinding, 4> bindings {};
        for (std::uint32_t i = 0; i < 4; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo set {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount = 4; set.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device_, &set, nullptr, &setLayout_), "create descriptor layout");
        const VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo descriptorPool {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorPool.maxSets = 1; descriptorPool.poolSizeCount = 1; descriptorPool.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(device_, &descriptorPool, nullptr, &descriptorPool_), "create descriptor pool");
        VkDescriptorSetAllocateInfo allocation {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = descriptorPool_; allocation.descriptorSetCount = 1; allocation.pSetLayouts = &setLayout_;
        check(vkAllocateDescriptorSets(device_, &allocation, &descriptors_), "allocate descriptors");
        const VkPushConstantRange push {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Settings)};
        VkPipelineLayoutCreateInfo layout {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1; layout.pSetLayouts = &setLayout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "create pipeline layout");
        std::ifstream shaderFile(SPOT_HEAL_PROBE_SHADER, std::ios::binary | std::ios::ate);
        if (!shaderFile) throw std::runtime_error("Cannot open scorer shader");
        const auto bytes = static_cast<std::size_t>(shaderFile.tellg());
        std::vector<std::uint32_t> code((bytes + 3) / 4);
        shaderFile.seekg(0); shaderFile.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(bytes));
        VkShaderModuleCreateInfo shader {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; shader.codeSize = bytes; shader.pCode = code.data();
        VkShaderModule module {}; check(vkCreateShaderModule(device_, &shader, nullptr, &module), "create shader module");
        VkComputePipelineCreateInfo pipeline {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        pipeline.layout = layout_;
        const auto status = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
        vkDestroyShaderModule(device_, module, nullptr); check(status, "create scorer pipeline");
    }

    void destroy()
    {
        if (device_) {
            auto free = [&](Buffer& value) {
                if (value.mapped) vkUnmapMemory(device_, value.memory);
                if (value.buffer) vkDestroyBuffer(device_, value.buffer, nullptr);
                if (value.memory) vkFreeMemory(device_, value.memory, nullptr);
            };
            for (auto& value : buffers_) free(value);
            free(staging_); free(readback_);
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
            if (descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
            if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
            if (queries_) vkDestroyQueryPool(device_, queries_, nullptr);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (debug_) reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT"))(instance_, debug_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
    }

    VkInstance instance_ {}; VkDebugReportCallbackEXT debug_ {}; VkPhysicalDevice physical_ {}; VkDevice device_ {};
    std::uint32_t family_ {}; double timestampPeriod_ {};
    VkQueue queue_ {}; VkCommandPool pool_ {}; VkCommandBuffer command_ {}; VkFence fence_ {}; VkQueryPool queries_ {};
    VkDescriptorSetLayout setLayout_ {}; VkDescriptorPool descriptorPool_ {}; VkDescriptorSet descriptors_ {};
    VkPipelineLayout layout_ {}; VkPipeline pipeline_ {};
    std::array<Buffer, 4> buffers_ {}; Buffer staging_, readback_;
};
} // namespace

int main(int argc, char** argv)
{
    try {
        const bool validation = argc > 1 && std::string_view(argv[1]) == "--validation";
        constexpr unsigned width = 527, radius = 7, patchPixels = 225, maxCandidates = 4096;
        std::vector<Pixel> original(width * width), target(16 * patchPixels);
        for (std::size_t p = 0; p < original.size(); ++p) {
            auto& v = original[p];
            const float alpha = p % 3 ? 1.F : .37F;
            for (unsigned c = 0; c < 3; ++c) {
                // Premultiply and decode exactly as the cached CPU descriptors.
                const float channel = float((p * (17 + c * 13)) % 1009) / 1009.F;
                v.colorAlpha[c] = double(channel * alpha) / alpha;
            }
            v.colorAlpha[3] = alpha;
            v.texture = {float(p % 13) / 71.F, float(p % 19) / 87.F};
        }
        for (std::size_t p = 0; p < target.size(); ++p) {
            target[p] = original[(p * 53 + 1319) % original.size()];
            target[p].baseWeight = p % 17 ? (p % 3 ? .35 : 1) : 0;
            target[p].gradient = p % 7 != 0;
        }
        std::vector<Candidate> candidates(maxCandidates);
        for (unsigned i = 0; i < maxCandidates; ++i) {
            auto& c = candidates[i];
            c.offsets[0] = (i / 256) * patchPixels;
            const auto x = (i * 53) % (width - 2 * radius);
            const auto y = (i * 71) % (width - 2 * radius);
            c.offsets[1] = y * width + x;
            c.locality = 1.0e-7 * std::log1p(double(x * x + y * y) / double(radius * radius + 1));
        }
        std::cout << std::fixed << std::setprecision(6);
        Probe probe(validation);
        probe.upload(target, original, candidates);
        double checksum = 0, maximumError = 0;
        std::size_t differing = 0;
        for (unsigned count : {64U, 128U, 256U, 512U, 2048U, 4096U}) {
            const Settings settings {count, radius, width, 0};
            std::vector<double> cpu(count), gpu(count);
            for (unsigned i = 0; i < count; ++i) cpu[i] = cpuScore(target, original, candidates[i], settings);
            const auto cold = probe.score(settings, gpu);
            for (unsigned i = 0; i < count; ++i) {
                maximumError = std::max(maximumError, std::abs(cpu[i] - gpu[i]));
                differing += cpu[i] != gpu[i];
            }
            constexpr unsigned repeats = 40;
            const auto cpuStart = Clock::now();
            for (unsigned repeat = 0; repeat < repeats; ++repeat) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
                for (unsigned i = 0; i < count; ++i) cpu[i] = cpuScore(target, original, candidates[i], settings);
                checksum += cpu[repeat % count];
            }
            const auto cpuMs = ms(cpuStart) / repeats;
            std::vector<double> warm, device;
            for (unsigned repeat = 0; repeat < repeats; ++repeat) {
                const auto timing = probe.score(settings, gpu);
                warm.push_back(timing.endToEndMs); device.push_back(timing.deviceMs);
                checksum += gpu[repeat % count];
            }
            std::sort(warm.begin(), warm.end()); std::sort(device.begin(), device.end());
            std::cout << "candidates=" << count << ",target_batches=" << (count + 255) / 256
                      << ",patch_radius=" << radius << ",cpu_cached_ms=" << cpuMs
                      << ",gpu_first_dispatch_roundtrip_ms=" << cold.endToEndMs
                      << ",gpu_warm_roundtrip_median_ms=" << warm[repeats / 2]
                      << ",gpu_warm_device_median_ms=" << device[repeats / 2]
                      << ",gpu_roundtrip_p95_ms=" << warm[repeats * 95 / 100] << '\n';
        }
        std::cout << std::scientific << std::setprecision(17)
                  << "score_max_abs_error=" << maximumError << ",nonidentical_scores=" << differing
                  << ",checksum=" << checksum << ",validation_messages=" << validationMessages << '\n';
        return maximumError > 1.0e-12 || validationMessages ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
