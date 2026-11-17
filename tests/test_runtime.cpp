// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Terni::runtime: the library without the rule compiler (no shaderc). It loads every
// rule from the SPIR-V cache that test_cache_fill made with Terni::core, reaches the
// same state on every device, and names whatever is missing from the cache.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "terni/game.hpp"
#include "terni/vk_context.hpp"

using namespace terni;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static bool throws_with(const std::function<void()>& f, const std::string& text) {
    try {
        f();
    } catch (const std::exception& e) {
        if (std::string(e.what()).find(text) != std::string::npos) return true;
        std::printf("  unexpected message: %s\n", e.what());
        return false;
    }
    std::printf("  no exception (expected '%s')\n", text.c_str());
    return false;
}

int main() {
    auto game = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    if (!game->ok()) return 1;
    const uint32_t shaders = static_cast<uint32_t>(game->stages.size()) + 1;
    const fs::path dir = fs::path(TERNI_TEST_BINARY_DIR) / "runtime_cache";
    unsigned long long reference = 0;
    if (!(std::ifstream(fs::path(TERNI_TEST_BINARY_DIR) / "runtime_reference.txt") >> std::hex >> reference)) {
        std::printf("no reference: run test_cache_fill first (ctest does)\n");
        return 1;
    }

    CHECK(!can_compile_rules());
    CHECK(throws_with([&] { compile_rules(*game, (fs::path(TERNI_TEST_BINARY_DIR) / "runtime_unused").string()); },
                      "no rule compiler"));
    CHECK(throws_with([&] { GameSim sim(*game); }, "no rule compiler (Terni::runtime): no spirv_cache_dir"));

    // from the cache: the same state as the Terni::core run, on every device
    GameSimOptions o;
    o.spirv_cache_dir = dir.string();  // cache_only is implied: this build cannot compile
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        o.device = int(d.index);
        GameSim sim(*game, o);
        CHECK(sim.compile_stats().cached == shaders && sim.compile_stats().compiled == 0);
        for (int t = 0; t < 50; ++t) sim.tick();
        const uint64_t hash = combine(hash_fields(sim.snapshot()));
        std::printf("  from the cache on %-45s %s\n", d.name.c_str(), hash == reference ? "identical" : "DIFFERS");
        CHECK(hash == reference);
    }

    // an edited rule is not in the cache: the error names it
    auto modded = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    modded->stages[1].source += "\n// a modder's edit\n";
    o.device = -1;
    CHECK(throws_with([&] { GameSim sim(*modded, o); }, "1 of " + std::to_string(shaders) + " shaders are not in the "
                                                        "SPIR-V cache '" + dir.string() + "' (" + modded->stages[1].name + ")"));

    std::printf("test_runtime: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
