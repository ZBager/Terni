// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#pragma once
// Minimal compute-only Vulkan context.
//
// A graphics app needs a window, swapchain, render passes, ... A compute-only
// app needs just five things, all created here:
//   1. VkInstance        — connection to the Vulkan loader
//   2. VkPhysicalDevice  — the GPU we picked (NVIDIA dGPU, AMD iGPU, ...)
//   3. VkDevice          — our logical "session" on that GPU, with the features we enable
//   4. VkQueue           — where we submit work (from a queue family that supports compute)
//   5. VkCommandPool     — allocator for command buffers (recorded lists of GPU commands)

#include <vulkan/vulkan.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#define VK_CHECK(expr)                                                                         \
    do {                                                                                       \
        VkResult vk_check_result_ = (expr);                                                    \
        if (vk_check_result_ != VK_SUCCESS)                                                    \
            throw std::runtime_error(std::string("Vulkan error ") +                            \
                                     std::to_string(static_cast<int>(vk_check_result_)) +      \
                                     " in " #expr " at " __FILE__ ":" + std::to_string(__LINE__)); \
    } while (0)

namespace terni {

// A VkBuffer is only a handle describing size/usage; the actual memory is a
// separate VkDeviceMemory allocation bound to it. We keep them together.
struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // non-null for host-visible buffers (staging)
};

struct DeviceInfo {
    uint32_t index;
    std::string name;
    std::string type;  // "discrete", "integrated", "virtual", "cpu (software)", "other"
    bool usable;       // supports everything we need
    std::string why_not;
    std::array<uint8_t, 16> uuid{};  // VkPhysicalDeviceIDProperties::deviceUUID
    uint32_t vendor_id = 0;
};

enum class DevicePreference {
    Discrete,    // dedicated GPU, then integrated, then anything, then CPU (lavapipe)
    Integrated,  // integrated GPU first (e.g. to leave a dedicated GPU to the renderer)
    Cpu,         // software Vulkan (lavapipe) first: headless servers, reference runs
};

// Choose a usable device. A non-null `uuid` that matches a usable device wins —
// pass the physical device UUID of the host's own renderer to share its GPU.
// Returns -1 if no usable device exists.
int pick_device(DevicePreference preference = DevicePreference::Discrete, const std::array<uint8_t, 16>* uuid = nullptr);

class VulkanContext {
public:
    static std::vector<DeviceInfo> list_devices();

    VulkanContext(uint32_t device_index, bool enable_validation);
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // `required` memory flags must be present; `preferred` ones are used if a
    // memory type has them (e.g. HOST_CACHED for fast CPU reads of readbacks).
    GpuBuffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred = 0);
    void destroy_buffer(GpuBuffer& b);

    VkCommandBuffer allocate_command_buffer();
    void free_command_buffer(VkCommandBuffer cmd);
    // Submit and block until the GPU has finished it.
    void submit_and_wait(VkCommandBuffer cmd);
    void submit_and_wait(const std::vector<VkCommandBuffer>& cmds);  // one submission, in order

    VkShaderModule load_shader(const std::string& spirv_path);
    VkShaderModule create_shader(const std::vector<uint32_t>& spirv);

    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_; }
    const VkPhysicalDeviceProperties& properties() const { return props_; }
    uint32_t timestamp_valid_bits() const { return timestamp_valid_bits_; }
    std::string device_name() const { return props_.deviceName; }

private:
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required,
                              VkMemoryPropertyFlags preferred) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    uint32_t timestamp_valid_bits_ = 0;
};

}  // namespace terni
