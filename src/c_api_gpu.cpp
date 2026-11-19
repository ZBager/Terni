// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The C API's calls for sharing the renderer's GPU, and for a software Vulkan driver
// shipped with the game: the ID of the GPU a host renders with, read through the
// host's own graphics API objects.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#endif

#include "terni/terni.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>

#include "terni/vk_context.hpp"

namespace terni::c_api {
// In c_api.cpp: sets terni_last_error() for this thread.
terni_status fail(terni_status status, const std::string& message);
void clear_error();
}  // namespace terni::c_api

namespace {

using terni::c_api::clear_error;
using terni::c_api::fail;

bool any(const uint8_t* bytes, size_t n) {
    return std::any_of(bytes, bytes + n, [](uint8_t b) { return b != 0; });
}

void set_name(terni_renderer_gpu& gpu, const std::string& name) {
    const size_t n = std::min(name.size(), sizeof gpu.name - 1);
    std::memcpy(gpu.name, name.data(), n);
    gpu.name[n] = '\0';
}

#ifdef _WIN32
bool describe(IDXGIAdapter* adapter, terni_renderer_gpu& gpu) {
    DXGI_ADAPTER_DESC desc;
    if (FAILED(adapter->GetDesc(&desc))) return false;
    static_assert(sizeof(desc.AdapterLuid) == 8, "an adapter LUID is 8 bytes");
    std::memcpy(gpu.luid, &desc.AdapterLuid, 8);  // LowPart then HighPart, as Vulkan's deviceLUID
    gpu.has_luid = any(gpu.luid, 8) ? 1 : 0;
    gpu.vendor_id = desc.VendorId;
    char name[sizeof gpu.name];
    const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, int(sizeof name), nullptr, nullptr);
    set_name(gpu, n > 0 ? std::string(name) : std::string());
    return true;
}
#endif

}  // namespace

extern "C" {

terni_status terni_vulkan_renderer_gpu(void* instance, void* get_instance_proc_addr, void* physical_device,
                                       terni_renderer_gpu* gpu) {
    clear_error();
    if (!gpu || !instance || !physical_device)
        return fail(TERNI_INVALID, "terni_vulkan_renderer_gpu() needs the instance, the physical device and gpu");
    terni_renderer_gpu out{};
    const auto get_proc = get_instance_proc_addr ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr)
                                                 : &vkGetInstanceProcAddr;
    const auto inst = static_cast<VkInstance>(instance);
    const auto phys = static_cast<VkPhysicalDevice>(physical_device);
    const auto properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_proc(inst, "vkGetPhysicalDeviceProperties"));
    if (!properties) return fail(TERNI_ERROR, "the renderer's instance has no vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties p;
    properties(phys, &p);
    out.vendor_id = p.vendorID;
    set_name(out, p.deviceName);
    // The IDs need Vulkan 1.1 (or an extension) on the renderer's instance; without it
    // drivers leave them zero.
    auto properties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_proc(inst, "vkGetPhysicalDeviceProperties2"));
    if (!properties2)
        properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_proc(inst, "vkGetPhysicalDeviceProperties2KHR"));
    if (properties2) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &id;
        properties2(phys, &p2);
        out.has_uuid = any(id.deviceUUID, 16) ? 1 : 0;
        std::memcpy(out.uuid, id.deviceUUID, 16);
        out.has_luid = id.deviceLUIDValid == VK_TRUE && any(id.deviceLUID, 8) ? 1 : 0;
        std::memcpy(out.luid, id.deviceLUID, 8);
    }
    *gpu = out;
    return TERNI_OK;
}

terni_status terni_dxgi_renderer_gpu(uint32_t adapter_index, terni_renderer_gpu* gpu) {
    clear_error();
    if (!gpu) return fail(TERNI_INVALID, "no gpu given (NULL)");
#ifdef _WIN32
    terni_renderer_gpu out{};
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return fail(TERNI_ERROR, "CreateDXGIFactory1 failed");
    IDXGIAdapter1* adapter = nullptr;
    bool ok = false, found = false;
    if (SUCCEEDED(factory->EnumAdapters1(adapter_index, &adapter))) {
        found = true;
        ok = describe(adapter, out);
        adapter->Release();
    }
    factory->Release();
    if (!found) return fail(TERNI_INVALID, "no DXGI adapter " + std::to_string(adapter_index));
    if (!ok) return fail(TERNI_ERROR, "the DXGI adapter does not describe itself");
    *gpu = out;
    return TERNI_OK;
#else
    (void)adapter_index;
    return fail(TERNI_INVALID, "DirectX adapters exist only on Windows");
#endif
}

terni_status terni_dxgi_adapter_gpu(void* adapter, terni_renderer_gpu* gpu) {
    clear_error();
    if (!gpu || !adapter) return fail(TERNI_INVALID, "terni_dxgi_adapter_gpu() needs the adapter and gpu");
#ifdef _WIN32
    terni_renderer_gpu out{};
    if (!describe(static_cast<IDXGIAdapter*>(adapter), out))
        return fail(TERNI_ERROR, "the DXGI adapter does not describe itself");
    *gpu = out;
    return TERNI_OK;
#else
    return fail(TERNI_INVALID, "DirectX adapters exist only on Windows");
#endif
}

terni_status terni_find_device(const terni_renderer_gpu* gpu, int32_t* device) {
    clear_error();
    if (!gpu || !device) return fail(TERNI_INVALID, "terni_find_device() needs gpu and device");
    try {
        const auto devices = terni::VulkanContext::list_devices();
        int32_t found = -1;
        if (gpu->has_uuid)
            for (const terni::DeviceInfo& d : devices)
                if (found < 0 && d.usable && std::equal(d.uuid.begin(), d.uuid.end(), gpu->uuid)) found = int32_t(d.index);
        if (found < 0 && gpu->has_luid)
            for (const terni::DeviceInfo& d : devices)
                if (found < 0 && d.usable && d.luid_valid && std::equal(d.luid.begin(), d.luid.end(), gpu->luid))
                    found = int32_t(d.index);
        if (found < 0 && gpu->vendor_id != 0 && gpu->name[0]) {
            // Some renderers' instances do not give the IDs (they come with Vulkan 1.1).
            // Two identical GPUs cannot be told apart this way, so then there is no match.
            const std::string name(gpu->name, std::find(gpu->name, gpu->name + sizeof gpu->name, '\0'));
            int matches = 0;
            for (const terni::DeviceInfo& d : devices)
                if (d.usable && d.vendor_id == gpu->vendor_id && d.name == name) found = int32_t(d.index), ++matches;
            if (matches != 1) found = -1;
        }
        *device = found;
        return TERNI_OK;
    } catch (const std::exception& e) {
        return fail(TERNI_ERROR, e.what());
    } catch (...) {
        return fail(TERNI_ERROR, "unknown error");
    }
}

const char* terni_shipped_driver_name(void) {
#ifdef _WIN32
    return "vulkan_lvp.dll";
#else
    return "libvulkan_lvp.so";
#endif
}

terni_status terni_add_shipped_driver(const char* folder, int32_t* result) {
    clear_error();
    if (!folder || !result) return fail(TERNI_INVALID, "terni_add_shipped_driver() needs the folder and result");
    static std::mutex mutex;
    static bool done = false;
    static terni_status first_status = TERNI_OK;
    static int32_t first_result = TERNI_DRIVER_NOT_SHIPPED;
    static std::string first_error;
    std::lock_guard<std::mutex> lock(mutex);
    if (!done) {
        done = true;
        try {
            std::error_code ec;
            const std::filesystem::path file = std::filesystem::path(folder) / terni_shipped_driver_name();
            bool system_has_one = false;
            for (const terni::DeviceInfo& d : terni::VulkanContext::list_devices())
                system_has_one = system_has_one || (d.usable && d.cpu());
            if (!std::filesystem::exists(file, ec)) {
                first_result = TERNI_DRIVER_NOT_SHIPPED;
            } else if (system_has_one) {
                first_result = TERNI_DRIVER_SYSTEM_HAS_ONE;
            } else {
                terni::add_vulkan_driver(file.string());
                first_result = TERNI_DRIVER_ADDED;
            }
        } catch (const std::exception& e) {
            first_status = TERNI_ERROR;
            first_error = e.what();
        } catch (...) {
            first_status = TERNI_ERROR;
            first_error = "unknown error";
        }
    }
    if (first_status != TERNI_OK) return fail(first_status, first_error);
    *result = first_result;
    return TERNI_OK;
}

}  // extern "C"
