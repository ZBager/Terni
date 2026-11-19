// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Paths are UTF-8 on every platform: a game, a mod, a SPIR-V cache and a GLSL library in
// folders named with letters no single Windows code page has (Polish and Japanese), and
// file names in messages as they are on disk. Leaves the folder behind for the tool tests
// (utf8_folder.txt names it).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "terni/game.hpp"
#include "terni/state_hash.hpp"
#include "utf8_path.hpp"

using namespace terni;
namespace fs = std::filesystem;
using detail::utf8_path;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

// "Zajac lodz" with Polish letters, then "nihon" in Japanese; and a mod "deja" with French ones.
static const std::string kFolder = "paths Zaj\xC4\x85""c \xC5\x81\xC3\xB3""d\xC5\xBA \xE6\x97\xA5\xE6\x9C\xAC";
static const std::string kMod = "mod d\xC3\xA9j\xC3\xA0";
static const std::string kBadFile = "defines_\xC5\x82\xC3\xB3\xC5\xBA.txt";

static uint64_t run(const Game& game, const GameSimOptions& o, CompileStats* stats = nullptr) {
    GameSim sim(game, o);
    for (int t = 0; t < 30; ++t) sim.tick();
    if (stats) *stats = sim.compile_stats();
    return combine(hash_fields(sim.snapshot()));
}

int main() {
    const std::string base = std::string(TERNI_TEST_BINARY_DIR) + "/" + kFolder;
    std::error_code ec;
    fs::remove_all(utf8_path(base), ec);
    fs::create_directories(utf8_path(base));
    const auto copy = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
    fs::copy(utf8_path(TERNI_SOURCE_DIR "/examples/mini_galaxy"), utf8_path(base + "/game"), copy);
    fs::copy(utf8_path(TERNI_TEST_DATA "/galaxy_mods/good_mod"), utf8_path(base + "/" + kMod), copy);
    fs::copy(utf8_path(TERNI_SOURCE_DIR "/shaders"), utf8_path(base + "/glsl \xC3\xBC"), copy);
    std::ofstream(utf8_path(TERNI_TEST_BINARY_DIR "/utf8_folder.txt"), std::ios::binary) << base;

    // the game with a mod on top, from these folders: the same as from the originals
    auto game = load_game({base + "/game", base + "/" + kMod});
    for (const auto& e : game->errors) std::printf("  %s\n", e.text().c_str());
    CHECK(game->ok());
    auto reference = load_game({TERNI_SOURCE_DIR "/examples/mini_galaxy", TERNI_TEST_DATA "/galaxy_mods/good_mod"});
    CHECK(reference->ok());
    CHECK(game_checksum(*game) == game_checksum(*reference));
    bool named = false;  // a stage's source file is named after the mod folder, as on disk
    for (const auto& st : game->stages) named = named || st.source_file == kMod + "/rules/planet_growth.glsl";
    CHECK(named);

    // a SPIR-V cache in such a folder: filled, then used alone
    GameSimOptions o;
    o.spirv_cache_dir = base + "/cache \xE2\x82\xAC";
    CompileStats stats;
    const uint64_t expected = run(*reference, GameSimOptions{});
    CHECK(run(*game, o, &stats) == expected);
    size_t files = 0;
    for (const auto& e : fs::directory_iterator(utf8_path(o.spirv_cache_dir), ec)) files += e.path().extension() == ".spv";
    CHECK(files == stats.compiled + stats.cached && files > 0);
    o.cache_only = true;
    CHECK(run(*game, o, &stats) == expected);
    CHECK(stats.compiled == 0);

    // the GLSL library from such a folder
    GameSimOptions library;
    library.shader_library_dir = base + "/glsl \xC3\xBC";
    CHECK(run(*game, library) == expected);

    // a broken file: its name in the message, as on disk
    fs::create_directories(utf8_path(base + "/broken/common"));
    std::ofstream(utf8_path(base + "/broken/common/" + kBadFile), std::ios::binary) << "growth_rate = { 0.01\n";
    auto broken = load_game({base + "/game", base + "/broken"});
    bool reported = false;
    for (const auto& e : broken->errors) reported = reported || e.text().find("broken/common/" + kBadFile + ":") == 0;
    for (const auto& e : broken->errors) std::printf("  expected error: %s\n", e.text().c_str());
    CHECK(!broken->ok() && reported);
    fs::remove_all(utf8_path(base + "/broken"), ec);

    std::printf("test_paths: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
