// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// A machine without a Vulkan driver: no devices and a clear error, until the game adds
// the software driver it ships with add_vulkan_driver(); then the rules run on the CPU.
// CTest runs this with the system's drivers hidden (VK_DRIVER_FILES), and passes the
// driver library. That the added driver agrees with every other device is the
// software_driver_parity test (terni_run --driver).
#include <cstdio>
#include <stdexcept>
#include <string>

#include "terni/game.hpp"
#include "terni/vk_context.hpp"

using namespace terni;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

template <class F>
static std::string error_of(F&& f) {
    try {
        f();
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return "";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: test_software_driver <driver library>");
        return 2;
    }
    auto game = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    CHECK(game->ok());

    // nothing to run on, and every call says so instead of failing inside Vulkan
    CHECK(VulkanContext::list_devices().empty());
    CHECK(pick_device() == -1);
    CHECK(error_of([&] { GameSim sim(*game); }).find("no usable Vulkan device") != std::string::npos);
    CHECK(error_of([&] { GameSim sim(*game, 0); }).find("no Vulkan driver") != std::string::npos);

    // files that are not drivers are refused, and change nothing
    CHECK(error_of([] { add_vulkan_driver(TERNI_SOURCE_DIR "/no_such_driver.so"); }).find("no file") != std::string::npos);
    CHECK(error_of([] { add_vulkan_driver(TERNI_SOURCE_DIR "/README.md"); }).find("not a Vulkan driver") != std::string::npos);
    CHECK(VulkanContext::list_devices().empty());

    add_vulkan_driver(argv[1]);
    add_vulkan_driver(argv[1]);  // twice is once
    const auto devices = VulkanContext::list_devices();
    CHECK(devices.size() == 1 && devices[0].usable && devices[0].cpu());
    CHECK(pick_device() == 0);
    CHECK(pick_device(DevicePreference::Integrated) == 0);
    if (failures == 0) {
        GameSim sim(*game);  // the default options find it
        for (int t = 0; t < 50; ++t) sim.tick();
        CHECK(sim.ticks() == 50);
        std::printf("ran on %s\n", sim.device_name().c_str());
    }

    std::printf("test_software_driver: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
