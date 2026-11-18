// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Minimal compute-only Vulkan context.
//
// A graphics app needs a window, swapchain, render passes, ... A compute-only
// app needs just five things, all created here:
//   1. VkInstance        -- connection to the Vulkan loader
//   2. VkPhysicalDevice  -- the GPU we picked (NVIDIA dGPU, AMD iGPU, ...)
//   3. VkDevice          -- our logical "session" on that GPU, with the features we enable
//   4. VkQueue           -- where we submit work (from a queue family that supports compute)
//   5. VkCommandPool     -- allocator for command buffers (recorded lists of GPU commands)

#include <vulkan/vulkan.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace terni {
// The device was lost (VK_ERROR_DEVICE_LOST): the driver reset the GPU, often because a
// rule ran too long (an endless loop in a mod, say), or the GPU went away. The state on
// the device is gone, and every call that ticks or reads the device throws it from then
// on: create a new GameSim, on this device or another, and load the last save.
class DeviceLost : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
}  // namespace terni

#define VK_CHECK(expr)                                                                         \
    do {                                                                                       \
        VkResult vk_check_result_ = (expr);                                                    \
        if (vk_check_result_ == VK_ERROR_DEVICE_LOST)                                          \
            throw ::terni::DeviceLost(std::string("Vulkan device lost in " #expr " at " __FILE__ ":") + \
                                      std::to_string(__LINE__));                               \
        if (vk_check_result_ != VK_SUCCESS)                                                    \
            throw std::runtime_error(std::string("Vulkan error ") +                            \
                                     std::to_string(static_cast<int>(vk_check_result_)) +      \
                                     " in " #expr " at " __FILE__ ":" + std::to_string(__LINE__)); \
    } while (0)

namespace terni {

// What a device does when a rule reads or writes past the end of a field.
enum class BufferRobustness {
    None,     // undefined: the device may crash or corrupt other memory
    Bounded,  // robustBufferAccess: the access stays inside the buffer, in a way each driver chooses
    Defined,  // robustBufferAccess2: reads give 0 and writes are dropped, the same on every device
};

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
    bool cpu() const { return type == "cpu (software)"; }  // a software driver: the rules run on the CPU
    bool usable;       // supports everything we need
    std::string why_not;
    std::array<uint8_t, 16> uuid{};  // VkPhysicalDeviceIDProperties::deviceUUID
    uint32_t vendor_id = 0;
    // VkPhysicalDeviceIDProperties::deviceLUID: the adapter LUID that DirectX (DXGI) also
    // reports, so a host that renders with DirectX can find its GPU. Only on Windows, and
    // only if luid_valid.
    std::array<uint8_t, 8> luid{};
    bool luid_valid = false;
    // What a rule that reads or writes past the end of a field does here. Terni turns on the
    // most the device offers; Defined (all current desktop drivers) keeps a broken rule
    // deterministic across devices.
    BufferRobustness robustness = BufferRobustness::None;
    // The driver's name and version as it reports them ("NVIDIA 580.95.05", "radv Mesa 25.2.4");
    // empty if the device predates Vulkan 1.2. For logs and benchmark results.
    std::string driver;
};

enum class DevicePreference {
    Discrete,    // dedicated GPU, then integrated, then anything, then CPU (lavapipe)
    Integrated,  // integrated GPU first (e.g. to leave a dedicated GPU to the renderer)
    Cpu,         // software Vulkan (lavapipe) first: headless servers, reference runs
};

// A Vulkan driver shipped with the game, for machines whose own drivers give no usable
// device: a software driver such as Mesa's lavapipe runs the same rules on the CPU, with
// the same results. Loads the driver library (vulkan_lvp.dll, libvulkan_lvp.so) and adds
// its devices to the system's for every later list_devices(), pick_device() and GameSim
// in this process. Call it before those: device indices change. Throws
// std::runtime_error if the library cannot be loaded or is not a Vulkan driver, or if the
// Vulkan loader cannot take drivers from the application (before 1.3.234).
void add_vulkan_driver(const std::string& library_path);

// Choose a usable device. A usable device whose UUID matches a non-null `uuid` wins, then
// one whose LUID matches a non-null `luid`: pass the ID of the host's own renderer to
// share its GPU (the physical device UUID from a Vulkan renderer, the adapter LUID from a
// DirectX one). Without a match, `preference` decides. Returns -1 if no usable device exists.
int pick_device(DevicePreference preference = DevicePreference::Discrete, const std::array<uint8_t, 16>* uuid = nullptr,
                const std::array<uint8_t, 8>* luid = nullptr);

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
    // One submission that is not waited for: at most one at a time. submitted_done() asks
    // without blocking; wait_submitted() blocks until it is done (both true/return at once
    // if nothing is pending).
    void submit(const std::vector<VkCommandBuffer>& cmds);
    bool submitted_done();
    void wait_submitted();

    VkShaderModule load_shader(const std::string& spirv_path);
    VkShaderModule create_shader(const std::vector<uint32_t>& spirv);

    VkDevice device() const { return device_; }
    VkPhysicalDevice physical_device() const { return physical_; }
    const VkPhysicalDeviceProperties& properties() const { return props_; }
    uint32_t timestamp_valid_bits() const { return timestamp_valid_bits_; }
    BufferRobustness buffer_robustness() const { return robustness_; }
    // The device's timestamp clock now, in the units vkCmdWriteTimestamp2 writes (through
    // VK_KHR/EXT_calibrated_timestamps). False where the device cannot tell it.
    bool device_clock(uint64_t& ticks) const;
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
    VkFence async_fence_ = VK_NULL_HANDLE;  // submit()
    bool async_pending_ = false;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    uint32_t timestamp_valid_bits_ = 0;
    BufferRobustness robustness_ = BufferRobustness::None;
    PFN_vkGetCalibratedTimestampsEXT get_calibrated_ = nullptr;  // null: no device clock
};

}  // namespace terni
