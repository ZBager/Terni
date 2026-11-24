// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#include "frame_load.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "rule_compiler.hpp"  // detail::RuleCompiler: the benchmark is built with the compiler

namespace terni::bench {
namespace {

// Each pass mixes every word of the buffer `rounds` times: arithmetic and memory traffic,
// as a frame's passes have. Nothing reads the result.
const char* PASS = R"(#version 460
layout(local_size_x = 256) in;
layout(std430, binding = 0) buffer Data { uint data[]; };
layout(push_constant) uniform Work { uint rounds; uint count; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= count) return;
    uint v = data[i] + i;
    for (uint r = 0u; r < rounds; ++r) {
        v = v * 1664525u + 1013904223u;
        v ^= v >> 13;
    }
    data[i] = v;
}
)";

constexpr uint32_t ROUNDS = 4;              // per word and pass
constexpr uint32_t MAX_WORDS = 8u << 20;    // 32 MiB
constexpr uint32_t MIN_WORDS = 16u << 10;
constexpr uint32_t MAX_PASSES = 4096;

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("frame load: ") + what + " failed (" + std::to_string(int(r)) + ")");
}

}  // namespace

struct FrameLoad::Vk {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;  // null: no timestamps on this queue
    double period_ns = 1;
    uint64_t mask = ~uint64_t(0);
    uint32_t words = MAX_WORDS, passes = 1;  // per frame, with a barrier after each pass

    ~Vk() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (queries) vkDestroyQueryPool(device, queries, nullptr);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commands) vkDestroyCommandPool(device, commands, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
            if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
            if (buffer) vkDestroyBuffer(device, buffer, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    void record() {
        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};  // submitted every frame
        check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        if (queries) {
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const uint32_t work[2] = {ROUNDS, words};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof work, work);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        for (uint32_t p = 0; p < passes; ++p) {
            vkCmdDispatch(cmd, (words + 255) / 256, 1, 1);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &barrier, 0, nullptr, 0, nullptr);
        }
        if (queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    }
};

FrameLoad::FrameLoad(const std::array<uint8_t, 16>& device_uuid, double percent) : vk_(std::make_unique<Vk>()) {
    Vk& v = *vk_;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "terni_bench frame load";
    app.apiVersion = VK_API_VERSION_1_3;  // the shader is SPIR-V for Vulkan 1.3, as Terni needs anyway
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    check(vkCreateInstance(&ici, nullptr, &v.instance), "vkCreateInstance");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(v.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(v.instance, &n, devices.data());
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        vkGetPhysicalDeviceProperties2(d, &props);
        if (std::memcmp(id.deviceUUID, device_uuid.data(), VK_UUID_SIZE) == 0) {
            v.physical = d;
            v.period_ns = props.properties.limits.timestampPeriod;
        }
    }
    if (!v.physical) throw std::runtime_error("frame load: the device is not visible to a plain Vulkan instance");

    // a queue that can draw, as a renderer's is (it can always run compute too)
    vkGetPhysicalDeviceQueueFamilyProperties(v.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(v.physical, &n, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t f = 0; f < n && family == UINT32_MAX; ++f)
        if ((families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (families[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) family = f;
    if (family == UINT32_MAX) throw std::runtime_error("frame load: the device has no graphics queue");
    const uint32_t bits = families[family].timestampValidBits;
    if (bits < 64) v.mask = (uint64_t(1) << bits) - 1;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    check(vkCreateDevice(v.physical, &dci, nullptr, &v.device), "vkCreateDevice");
    vkGetDeviceQueue(v.device, family, 0, &v.queue);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = uint64_t(MAX_WORDS) * 4;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    check(vkCreateBuffer(v.device, &bci, nullptr, &v.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v.device, v.buffer, &req);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(v.physical, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mem.memoryTypeCount && type == UINT32_MAX; ++t)
        if ((req.memoryTypeBits & (1u << t)) && (mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    for (uint32_t t = 0; t < mem.memoryTypeCount && type == UINT32_MAX; ++t)
        if (req.memoryTypeBits & (1u << t)) type = t;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    check(vkAllocateMemory(v.device, &mai, nullptr, &v.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(v.device, v.buffer, v.memory, 0), "vkBindBufferMemory");

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(v.device, &lci, nullptr, &v.set_layout), "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &size;
    check(vkCreateDescriptorPool(v.device, &pci, nullptr, &v.pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = v.pool;
    sai.descriptorSetCount = 1;
    sai.pSetLayouts = &v.set_layout;
    check(vkAllocateDescriptorSets(v.device, &sai, &v.set), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo info{v.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = v.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(v.device, 1, &write, 0, nullptr);

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &v.set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(v.device, &plci, nullptr, &v.layout), "vkCreatePipelineLayout");
    const std::vector<uint32_t> spirv = detail::RuleCompiler().compile("frame_load.comp", PASS, [](const char*, std::string&, std::string&) {
        return false;
    });
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spirv.size() * 4;
    smci.pCode = spirv.data();
    check(vkCreateShaderModule(v.device, &smci, nullptr, &v.shader), "vkCreateShaderModule");
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = v.shader;
    cpci.stage.pName = "main";
    cpci.layout = v.layout;
    check(vkCreateComputePipelines(v.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &v.pipeline), "vkCreateComputePipelines");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = family;
    check(vkCreateCommandPool(v.device, &cpi, nullptr, &v.commands), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = v.commands;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(v.device, &cai, &v.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(v.device, &fci, nullptr, &v.fence), "vkCreateFence");
    if (bits != 0) {
        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 2;
        check(vkCreateQueryPool(v.device, &qpci, nullptr, &v.queries), "vkCreateQueryPool");
    }

    // Size a frame: first the words, so that one pass takes about a sixteenth of the frame's
    // work, then the passes. GPUs change their clocks with the load, so frames run for a
    // while before each measure, and a measure is the median of five frames.
    const double target = FRAME_MS * std::clamp(percent, 1.0, 95.0) / 100.0;
    auto measure = [&] {
        v.record();
        const auto warm = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - warm < std::chrono::milliseconds(100)) frame_gpu_ms();
        std::array<double, 5> ms{};
        for (double& m : ms) m = frame_gpu_ms();
        std::sort(ms.begin(), ms.end());
        return ms[2];
    };
    double ms = measure();
    while (ms > target / 16 && v.words > MIN_WORDS) {
        v.words /= 2;
        ms = measure();
    }
    for (int step = 0; step < 6 && ms > 0 && std::abs(ms - target) > target * 0.03; ++step) {
        v.passes = uint32_t(std::clamp(std::round(double(v.passes) * target / ms), 1.0, double(MAX_PASSES)));
        ms = measure();
    }
    // What a frame takes alone, paced as during the measurement: 60 frames a second.
    start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop();
    calibrated_ms_ = mean_frame_ms();
}

FrameLoad::~FrameLoad() { stop(); }

double FrameLoad::frame_gpu_ms() {
    Vk& v = *vk_;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &v.cmd;
    const auto t0 = std::chrono::steady_clock::now();
    check(vkQueueSubmit(v.queue, 1, &si, v.fence), "vkQueueSubmit");
    check(vkWaitForFences(v.device, 1, &v.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(vkResetFences(v.device, 1, &v.fence), "vkResetFences");
    if (!v.queries) return wall;
    uint64_t ts[2] = {};
    check(vkGetQueryPoolResults(v.device, v.queries, 0, 2, sizeof ts, ts, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "vkGetQueryPoolResults");
    return double((ts[1] - ts[0]) & v.mask) * v.period_ns / 1e6;
}

void FrameLoad::start() {
    if (running_) return;
    frames_ = late_ = 0;
    total_ms_ = 0;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void FrameLoad::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

double FrameLoad::mean_frame_ms() const { return frames_ ? total_ms_ / double(frames_) : 0; }

void FrameLoad::run() {
    using Clock = std::chrono::steady_clock;
    const auto frame = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(FRAME_MS));
    auto slot = Clock::now();
    while (running_) {
        total_ms_ += frame_gpu_ms();
        ++frames_;
        slot += frame;
        if (Clock::now() > slot) {  // the frame did not fit in its slot: the next one starts at once
            ++late_;
            slot = Clock::now();
        } else {
            std::this_thread::sleep_until(slot);
        }
    }
}

}  // namespace terni::bench
