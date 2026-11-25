// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// terni_check -- what a modder runs before publishing: the game's folder with the mods on
// top, loaded as a game loads them, compiled, and run on every device.
//
//   terni_check <game folder> [mod folder ...] [--ticks N] [--device I]... [--driver LIBRARY]
//               [--no-run] [--strict]
//
// It reports which files the mods replace and add, files the game never reads, every
// error and warning with file:line (a warning about order means a multiplayer game can
// desync), compile errors, order races found at run time, devices that end in different
// states (with the fields and rows), and ticks slow enough to risk a GPU reset.
// Exit code: 0 ready (warnings allowed unless --strict), 1 not ready, 2 bad arguments.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "terni/game.hpp"
#include "terni/vk_context.hpp"
#include "utf8_args.hpp"

using namespace terni;
namespace fs = std::filesystem;

namespace {

// The subfolders a game's .txt files are read from.
const char* const READ_FOLDERS[] = {"schema", "common", "common/modifiers", "history", "rules"};

std::string folder_name(const std::string& root) {
    fs::path p = terni::detail::utf8_path(root).lexically_normal();
    if (p.filename().empty()) p = p.parent_path();
    return terni::detail::utf8_generic_string(p.filename());
}

void usage() {
    std::puts("usage: terni_check <game folder> [mod folder ...] [--ticks N] [--device I]... [--driver LIBRARY]\n"
              "                   [--no-run] [--strict]\n"
              "Loads the game with the mods on top, compiles its rules and runs it on every device.");
}

}  // namespace

int main(int argc, char** argv) {
    terni::tools::Utf8Args utf8(argc, argv);
    argv = utf8.argv();
    std::vector<std::string> roots, drivers;
    std::vector<uint32_t> devices;
    int ticks = 100;
    bool run = true, strict = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ticks" && i + 1 < argc) ticks = std::max(1, std::stoi(argv[++i]));
        else if (a == "--device" && i + 1 < argc) devices.push_back(static_cast<uint32_t>(std::stoul(argv[++i])));
        else if (a == "--driver" && i + 1 < argc) drivers.push_back(argv[++i]);
        else if (a == "--no-run") run = false;
        else if (a == "--strict") strict = true;
        else if (a == "--help" || a == "-h") return usage(), 0;
        else if (!a.empty() && a[0] == '-') return usage(), 2;
        else roots.push_back(a);
    }
    if (roots.empty()) return usage(), 2;
    for (const auto& r : roots)
        if (!fs::is_directory(terni::detail::utf8_path(r))) {
            std::printf("error: '%s' is not a folder\n", r.c_str());
            return 2;
        }
    int errors = 0, warnings = 0;
    auto warn = [&](const std::string& text) {
        std::printf("  warning: %s\n", text.c_str());
        ++warnings;
    };
    auto fail = [&](const std::string& text) {
        std::printf("  error: %s\n", text.c_str());
        ++errors;
    };

    std::printf("terni_check: %s", folder_name(roots[0]).c_str());
    for (size_t i = 1; i < roots.size(); ++i) std::printf(" + %s", folder_name(roots[i]).c_str());
    std::puts("");

    // ---- files: what each mod replaces and adds; what the game never reads
    std::puts("\nfiles");
    std::map<std::string, size_t> owner;  // path in the game -> the folder it comes from
    for (size_t r = 0; r < roots.size(); ++r) {
        std::vector<std::string> replaces, adds;
        const fs::path root = terni::detail::utf8_path(roots[r]);
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file()) continue;
            const fs::path rel_path = fs::relative(e.path(), root);
            const std::string rel = terni::detail::utf8_generic_string(rel_path);
            const std::string ext = terni::detail::utf8_string(e.path().extension());
            const std::string dir = terni::detail::utf8_generic_string(rel_path.parent_path());
            const bool read_txt = ext == ".txt" && std::find(std::begin(READ_FOLDERS), std::end(READ_FOLDERS), dir) !=
                                                       std::end(READ_FOLDERS);
            const bool rule = ext == ".glsl" && dir == "rules";
            if (ext == ".txt" && !read_txt)
                warn(folder_name(roots[r]) + "/" + rel + " is never read: the game reads .txt files in schema/, "
                                                         "common/, common/modifiers/, history/ and rules/");
            if (ext == ".glsl" && !rule) warn(folder_name(roots[r]) + "/" + rel + " is never read: rule sources go in rules/");
            if (!read_txt && !rule) continue;
            if (owner.count(rel)) replaces.push_back(rel);
            else if (r > 0) adds.push_back(rel);
            owner[rel] = r;
        }
        std::sort(replaces.begin(), replaces.end());
        std::sort(adds.begin(), adds.end());
        if (r == 0) continue;
        for (const auto& f : replaces) std::printf("  %s replaces %s\n", folder_name(roots[r]).c_str(), f.c_str());
        for (const auto& f : adds) std::printf("  %s adds %s\n", folder_name(roots[r]).c_str(), f.c_str());
        if (replaces.empty() && adds.empty()) warn(folder_name(roots[r]) + " changes no file the game reads");
    }

    // ---- loading: errors and warnings with file:line
    std::puts("\nloading");
    const auto game = load_game(roots);
    for (const auto& e : game->errors) fail(e.text());
    for (const auto& w : game->warnings) warn(w.text());
    std::set<std::string> stages;
    for (const auto& s : game->stages) stages.insert(s.name);
    for (const auto& [rel, r] : owner)
        if (const fs::path p = terni::detail::utf8_path(rel); p.parent_path() == "rules" && p.extension() == ".glsl" &&
                                                     !stages.count(terni::detail::utf8_string(p.stem())))
            warn(folder_name(roots[r]) + "/" + rel + " is not used: rules/stages.txt has no stage '" +
                 terni::detail::utf8_string(p.stem()) + "'");
    if (game->ok())
        std::printf("  %zu entity types, %zu stages, %zu modifier bundles\n", game->entities.size(), game->stages.size(),
                    game->bundles.size());

    // ---- compiling every rule
    const fs::path cache = fs::temp_directory_path() /
                           ("terni_check_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool compiled = false;
    if (game->ok()) {
        std::puts("\ncompiling");
        try {
            const CompileStats stats = compile_rules(*game, terni::detail::utf8_string(cache));
            std::printf("  %u shaders in %.0f ms\n", stats.compiled, stats.ms);
            compiled = true;
        } catch (const std::exception& e) {
            fail(e.what());
            // compiler messages name rules/<stage>.glsl; say which folder each came from
            for (const auto& st : game->stages)
                if (std::string(e.what()).find("rules/" + st.name + ".glsl") != std::string::npos && !st.source_file.empty())
                    std::printf("  (rules/%s.glsl is %s)\n", st.name.c_str(), st.source_file.c_str());
        }
    }

    // ---- running on every device: order races, the same state everywhere, tick times
    if (compiled && run) {
        try {
            for (const auto& d : drivers) add_vulkan_driver(d);
        } catch (const std::exception& e) {
            fail(e.what());
        }
        std::vector<DeviceInfo> list;
        for (const auto& d : VulkanContext::list_devices())
            if (d.usable && (devices.empty() || std::find(devices.begin(), devices.end(), d.index) != devices.end()))
                list.push_back(d);
        std::printf("\nrunning %d ticks on %zu device%s\n", ticks, list.size(), list.size() == 1 ? "" : "s");
        if (list.empty()) warn("no usable Vulkan device: the rules were not run (add lavapipe with --driver)");
        constexpr int CHECKED = 3, EVERY = 10;
        constexpr double SLOW_MS = 250;  // drivers reset the GPU after about 2 s (Windows)
        std::vector<std::vector<FieldHash>> first_hashes;  // per checkpoint, from the first device
        StateSnapshot first_end;
        std::set<std::string> races;
        for (size_t di = 0; di < list.size(); ++di) {
            const DeviceInfo& d = list[di];
            try {
                GameSimOptions o;
                o.device = int(d.index);
                o.spirv_cache_dir = terni::detail::utf8_string(cache);
                o.cache_only = true;
                GameSim sim(*game, o);
                double slowest = 0;
                int first_diff = -1;
                size_t checkpoint = 0;
                std::vector<FieldHash> diff_fields;
                for (int t = 0; t < ticks; ++t) {
                    const auto t0 = std::chrono::steady_clock::now();
                    if (t < CHECKED) {
                        for (const auto& issue : sim.tick_checked()) {
                            if (!races.insert(issue.stage + "/" + issue.field + (issue.misfit ? "/fit" : "")).second) continue;
                            const std::string rows = std::to_string(issue.rows), first = std::to_string(issue.first_row);
                            if (issue.misfit)
                                warn("stage '" + issue.stage + "' writes values that do not fit the small field " + issue.field +
                                     " (" + rows + (issue.rows == 1 ? " write" : " writes") + ", the first row is " + first +
                                     "); the nearest ones that fit are stored");
                            else
                                fail("stage '" + issue.stage + "' gives a different " + issue.field + " (" + rows +
                                     " rows from row " + first +
                                     ") depending on the order its threads run in; a multiplayer game would desync");
                        }
                    } else {
                        sim.tick();
                        slowest = std::max(slowest, std::chrono::duration<double, std::milli>(
                                                        std::chrono::steady_clock::now() - t0).count());
                    }
                    if ((t + 1) % EVERY == 0 || t + 1 == ticks) {
                        auto hashes = sim.field_hashes();
                        const size_t k = checkpoint++;
                        if (di == 0) first_hashes.push_back(std::move(hashes));
                        else if (first_diff < 0 && k < first_hashes.size() && hashes != first_hashes[k]) {
                            first_diff = t + 1;
                            for (size_t f = 0; f < hashes.size() && f < first_hashes[k].size(); ++f)
                                if (hashes[f].hash != first_hashes[k][f].hash) diff_fields.push_back(hashes[f]);
                        }
                    }
                }
                const StateSnapshot end = sim.snapshot();
                if (di == 0) first_end = end;
                std::printf("  %-44.44s slowest tick %.2f ms", d.name.c_str(), slowest);
                if (first_diff < 0) {
                    std::puts(di == 0 ? "" : ", the same state as the first device");
                } else {
                    std::puts("");
                    std::string fields;
                    for (const auto& f : diff_fields) fields += (fields.empty() ? "" : ", ") + f.name;
                    fail(d.name + " differs from " + list[0].name + " from tick " + std::to_string(first_diff) +
                         " on: " + fields);
                    for (const auto& diff : differences(first_end, end))
                        std::printf("    %s: %zu row%s differ at the end, the first is row %zu\n", diff.field.c_str(),
                                    diff.rows, diff.rows == 1 ? "" : "s", diff.first_row);
                }
                if (slowest > SLOW_MS)
                    warn(d.name + ": a tick took " + std::to_string(int(slowest)) +
                         " ms; a rule that runs for seconds makes the driver reset the GPU");
            } catch (const DeviceLost& e) {
                fail(d.name + ": the device was lost (a rule ran too long?): " + e.what());
            } catch (const std::exception& e) {
                fail(d.name + ": " + e.what());
            }
        }
    }
    std::error_code ignored;
    fs::remove_all(cache, ignored);

    // ---- verdict
    std::puts("");
    if (game->ok()) {
        std::printf("game checksum %016llx", static_cast<unsigned long long>(game_checksum(*game)));
        if (roots.size() > 1) {
            const auto base = load_game(roots[0]);
            if (base->ok())
                std::printf(" (the game alone: %016llx; players need the same mods to play together)",
                            static_cast<unsigned long long>(game_checksum(*base)));
        }
        std::puts("");
    }
    const bool ready = errors == 0 && (!strict || warnings == 0);
    std::printf("%s: %d error%s, %d warning%s\n", ready ? "ready" : "NOT READY", errors, errors == 1 ? "" : "s", warnings,
                warnings == 1 ? "" : "s");
    return ready ? 0 : 1;
}
