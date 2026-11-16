// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#include "terni/vk_context.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

namespace terni {

namespace {

const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

VkInstance create_instance(bool enable_validation) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Terni";
    app.apiVersion = VK_API_VERSION_1_3;  // 1.3 gives us synchronization2 in core

    std::vector<const char*> layers;
    if (enable_validation) {
        // The validation layer checks every API call for misuse and prints
        // errors. Indispensable while learning; costs performance.
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        bool found = false;
        for (auto& p : props) found |= std::strcmp(p.layerName, VALIDATION_LAYER) == 0;
        if (found)
            layers.push_back(VALIDATION_LAYER);
        else
            std::cerr << "warning: " << VALIDATION_LAYER << " not installed (package vulkan-validation-layers)\n";
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    VkInstance instance;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
    return instance;
}

std::vector<VkPhysicalDevice> enumerate(VkInstance instance) {
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devs(n);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &n, devs.data()));
    return devs;
}

// Returns an empty string if the device supports everything we need.
std::string check_support(VkPhysicalDevice pd) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return "needs Vulkan 1.3";

    // Feature queries use a pNext chain: each struct points to the next one,
    // and the driver fills them all in.
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(pd, &f2);

    if (!f13.synchronization2) return "no synchronization2";
    return {};
}

const char* type_name(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu (software)";
        default: return "other";
    }
}

}  // namespace

std::vector<DeviceInfo> VulkanContext::list_devices() {
    VkInstance instance = create_instance(false);
    std::vector<DeviceInfo> out;
    auto devs = enumerate(instance);
    for (uint32_t i = 0; i < devs.size(); ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        std::string why = check_support(devs[i]);
        DeviceInfo info{i, props.deviceName, type_name(props.deviceType), why.empty(), why};
        info.vendor_id = props.vendorID;
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &id;
        vkGetPhysicalDeviceProperties2(devs[i], &p2);
        std::copy(std::begin(id.deviceUUID), std::end(id.deviceUUID), info.uuid.begin());
        out.push_back(std::move(info));
    }
    vkDestroyInstance(instance, nullptr);
    return out;
}

int pick_device(DevicePreference preference, const std::array<uint8_t, 16>* uuid) {
    const auto devices = VulkanContext::list_devices();
    if (uuid)
        for (const auto& d : devices)
            if (d.usable && d.uuid == *uuid) return static_cast<int>(d.index);
    auto rank = [&](const DeviceInfo& d) {
        // lower is better
        const bool cpu = d.type == "cpu (software)";
        switch (preference) {
            case DevicePreference::Discrete:
                return d.type == "discrete" ? 0 : d.type == "integrated" ? 1 : cpu ? 3 : 2;
            case DevicePreference::Integrated:
                return d.type == "integrated" ? 0 : d.type == "discrete" ? 1 : cpu ? 3 : 2;
            case DevicePreference::Cpu:
                return cpu ? 0 : d.type == "discrete" ? 1 : d.type == "integrated" ? 2 : 3;
        }
        return 4;
    };
    int best = -1, best_rank = 99;
    for (const auto& d : devices)
        if (d.usable && rank(d) < best_rank) best = static_cast<int>(d.index), best_rank = rank(d);
    return best;
}

VulkanContext::VulkanContext(uint32_t device_index, bool enable_validation) {
    instance_ = create_instance(enable_validation);

    // ---- 1. pick the physical device
    auto devs = enumerate(instance_);
    if (device_index >= devs.size()) throw std::runtime_error("no Vulkan device with index " + std::to_string(device_index));
    physical_ = devs[device_index];
    if (auto why = check_support(physical_); !why.empty())
        throw std::runtime_error("device " + std::to_string(device_index) + " unusable: " + why);
    vkGetPhysicalDeviceProperties(physical_, &props_);
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);

    // ---- 2. pick a queue family with compute support.
    // Prefer a compute-only family (no graphics bit): that's the "async compute"
    // queue, which in a real game would run alongside rendering.
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, qprops.data());
    int best = -1;
    for (uint32_t i = 0; i < nq; ++i) {
        if (!(qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        const bool compute_only = !(qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
        if (best < 0 || (compute_only && (qprops[best].queueFlags & VK_QUEUE_GRAPHICS_BIT))) best = int(i);
    }
    if (best < 0) throw std::runtime_error("no compute queue");
    queue_family_ = static_cast<uint32_t>(best);
    timestamp_valid_bits_ = qprops[queue_family_].timestampValidBits;

    // ---- 3. create the logical device, enabling exactly the features we use
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;  // nicer barrier API (vkCmdPipelineBarrier2)
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    // Terni is pure 32-bit: no shaderInt64 / 64-bit atomics needed.
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;  // features go via pNext when using VkPhysicalDeviceFeatures2
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // ---- 4. command pool (RESET_COMMAND_BUFFER lets us re-record individual buffers)
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &pool_));

    // ---- 5. a fence: the GPU signals it when a submission finishes, the CPU waits on it
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device_, &fci, nullptr, &fence_));
}

VulkanContext::~VulkanContext() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyFence(device_, fence_, nullptr);
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

uint32_t VulkanContext::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required,
                                         VkMemoryPropertyFlags preferred) const {
    // GPUs expose several memory types (VRAM, system RAM visible to the GPU,
    // cached/uncached, ...). `type_bits` says which ones this buffer may use.
    int fallback = -1;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if (!(type_bits & (1u << i))) continue;
        const auto flags = mem_props_.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (fallback < 0) fallback = int(i);
    }
    if (fallback < 0) throw std::runtime_error("no suitable memory type");
    return static_cast<uint32_t>(fallback);
}

GpuBuffer VulkanContext::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                       VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    GpuBuffer b;
    b.size = size;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &b.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);

    // Note: real engines sub-allocate many buffers from a few big allocations
    // (that's what the VMA library does). One allocation per buffer is fine for
    // a few dozen buffers.
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, required, preferred);
    VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device_, b.buffer, b.memory, 0));

    if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VK_CHECK(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    return b;
}

void VulkanContext::destroy_buffer(GpuBuffer& b) {
    if (b.mapped) vkUnmapMemory(device_, b.memory);
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

VkCommandBuffer VulkanContext::allocate_command_buffer() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device_, &ai, &cmd));
    return cmd;
}

void VulkanContext::free_command_buffer(VkCommandBuffer cmd) { vkFreeCommandBuffers(device_, pool_, 1, &cmd); }

void VulkanContext::submit_and_wait(VkCommandBuffer cmd) { submit_and_wait(std::vector<VkCommandBuffer>{cmd}); }

void VulkanContext::submit_and_wait(const std::vector<VkCommandBuffer>& cmds) {
    std::vector<VkCommandBufferSubmitInfo> cbi(cmds.size(), {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO});
    for (size_t i = 0; i < cmds.size(); ++i) cbi[i].commandBuffer = cmds[i];
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = static_cast<uint32_t>(cbi.size());
    si.pCommandBufferInfos = cbi.data();
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, fence_));
    VK_CHECK(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device_, 1, &fence_));
}

VkShaderModule VulkanContext::create_shader(const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
    return m;
}

VkShaderModule VulkanContext::load_shader(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open shader " + path);
    const size_t size = static_cast<size_t>(f.tellg());
    std::vector<uint32_t> code((size + 3) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = code.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(device_, &ci, nullptr, &m));
    return m;
}

}  // namespace terni
