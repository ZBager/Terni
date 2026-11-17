// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Fixture for test_runtime (Terni::core): fills a SPIR-V cache with compile_rules(),
// which needs no Vulkan device, and writes the state hash a run of this build reaches,
// for the Terni::runtime build to match. Also checks compile_rules() itself.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "terni/game.hpp"

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
    const uint32_t shaders = static_cast<uint32_t>(game->stages.size()) + 1;  // + built-in tick stage
    const fs::path dir = fs::path(TERNI_TEST_BINARY_DIR) / "runtime_cache";
    fs::remove_all(dir);

    CHECK(can_compile_rules());
    const CompileStats first = compile_rules(*game, dir.string());
    CHECK(first.compiled == shaders && first.cached == 0);
    const CompileStats again = compile_rules(*game, dir.string());  // already there: nothing compiled
    CHECK(again.compiled == 0 && again.cached == shaders);
    std::printf("compile_rules: %u shaders compiled in %.1f ms\n", first.compiled, first.ms);

    // a cache that cannot be written is an error (the cache is what compile_rules makes)
    const fs::path blocker = fs::path(TERNI_TEST_BINARY_DIR) / "runtime_cache_blocker";
    std::ofstream(blocker) << "a file, not a folder";
    CHECK(throws_with([&] { compile_rules(*game, (blocker / "cache").string()); }, "could not write"));
    fs::remove(blocker);
    CHECK(throws_with([&] { compile_rules(*game, ""); }, "no cache folder"));

    // the reference for test_runtime: a cache-only run of this build
    GameSimOptions o;
    o.spirv_cache_dir = dir.string();
    o.cache_only = true;
    GameSim sim(*game, o);
    CHECK(sim.compile_stats().cached == shaders && sim.compile_stats().compiled == 0);
    for (int t = 0; t < 50; ++t) sim.tick();
    const uint64_t hash = combine(hash_fields(sim.snapshot()));
    std::ofstream(fs::path(TERNI_TEST_BINARY_DIR) / "runtime_reference.txt") << std::hex << hash << "\n";
    std::printf("reference after 50 ticks on %s: %016llx\n", sim.device_name().c_str(),
                static_cast<unsigned long long>(hash));

    std::printf("test_cache_fill: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
