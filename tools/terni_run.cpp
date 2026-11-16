// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// terni_run — load a data-driven game folder, build its rules, run it on every
// Vulkan device (GPUs and lavapipe) and check that all produce identical state.
//
//   terni_run <game folder> [--ticks N] [--device I]... [--driver LIBRARY] [--prelude] [--vk-validation]
//
// --driver adds a Vulkan driver library (e.g. lavapipe) to the system's, as a game that
// ships its own software driver would.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "terni/game.hpp"
#include "terni/vk_context.hpp"

using namespace terni;

int main(int argc, char** argv) {
    std::string root;
    int ticks = 100;
    bool show_prelude = false, validation = false, dump = false;
    std::vector<uint32_t> devices;
    std::vector<std::string> drivers;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ticks" && i + 1 < argc) ticks = std::stoi(argv[++i]);
        else if (a == "--device" && i + 1 < argc) devices.push_back(static_cast<uint32_t>(std::stoul(argv[++i])));
        else if (a == "--driver" && i + 1 < argc) drivers.push_back(argv[++i]);
        else if (a == "--prelude") show_prelude = true;
        else if (a == "--dump") dump = true;
        else if (a == "--vk-validation") validation = true;
        else root = a;
    }
    if (root.empty()) {
        std::puts("usage: terni_run <game folder> [--ticks N] [--device I]... [--driver LIBRARY] [--prelude] [--dump] "
                  "[--vk-validation]");
        return 2;
    }

    auto game = load_game(root);
    for (const auto& w : game->warnings) std::printf("warning: %s\n", w.text().c_str());
    for (const auto& e : game->errors) std::printf("error: %s\n", e.text().c_str());
    if (!game->ok()) {
        std::printf("%zu error(s) -- not running\n", game->errors.size());
        return 1;
    }
    std::printf("loaded %s: %zu entity types, %zu defines, %zu stages\n", game->root.c_str(), game->entities.size(),
                game->defines.size(), game->stages.size());
    for (const auto& t : game->tables)
        std::printf("  %-12s %u / %u\n", t.def->name.c_str(), t.count, t.def->capacity);
    if (show_prelude) std::printf("\n%s\n", generate_prelude(*game).c_str());

    try {
        for (const auto& d : drivers) add_vulkan_driver(d);
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    if (devices.empty())
        for (const auto& d : VulkanContext::list_devices())
            if (d.usable) devices.push_back(d.index);
    if (devices.empty()) {
        std::puts("error: no usable Vulkan device (try --driver with a software driver such as lavapipe)");
        return 1;
    }

    struct Run {
        std::string name;
        double ms;
        StateSnapshot state;
    };
    std::vector<Run> runs;
    try {
        for (uint32_t d : devices) {
            GameSim sim(*game, d, "", validation);
            const auto t0 = std::chrono::steady_clock::now();
            for (int t = 0; t < ticks; ++t) sim.tick();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            runs.push_back({sim.device_name(), ms / ticks, sim.snapshot()});
        }
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }

    bool identical = true;
    std::printf("\n%d ticks:\n", ticks);
    for (const auto& r : runs) {
        const std::string diff = first_difference(runs[0].state, r.state);
        identical &= diff.empty();
        std::printf("  %-45s %8.3f ms/tick  hash %016llx  %s\n", r.name.c_str(), r.ms,
                    static_cast<unsigned long long>(combine(hash_fields(r.state))),
                    diff.empty() ? "identical" : ("DIFFERS in " + diff).c_str());
    }
    if (dump && !runs.empty()) {
        // print every live entity of the first device's final state, formatted by field type
        const StateSnapshot& s = runs[0].state;
        auto column = [&](const std::string& name) -> const std::vector<int64_t>* {
            for (const auto& f : s.fields)
                if (f.first == name) return &f.second;
            return nullptr;
        };
        const std::vector<int64_t>& state = *column("terni.state");  // tick, then the rows in use per entity type
        for (size_t e = 0; e < game->tables.size(); ++e) {
            const auto& t = game->tables[e];
            std::printf("\n%s:\n", t.def->name.c_str());
            const uint32_t count = uint32_t(state[1 + e]);
            const std::vector<int64_t>* alive = column(t.def->name + ".alive_bits");  // dynamic entity types
            std::vector<std::string> names(count);
            for (uint32_t r = 0; r < count; ++r) names[r] = "#" + std::to_string(r);  // created during the run
            if (!alive)
                for (const auto& [id, row] : t.ids) names[row] = id;
            for (uint32_t r = 0; r < count; ++r) {
                if (alive && ((*alive)[r / 32] >> (r % 32) & 1) == 0) continue;
                std::printf("  %-12s", names[r].c_str());
                for (const auto& f : t.def->fields) {
                    const auto& v = *column(t.def->name + "." + f.name);
                    std::printf(" %s=", f.name.c_str());
                    for (uint32_t k = 0; k < f.size; ++k) {
                        const size_t i = size_t(r) * f.size + k;
                        if (k) std::printf(",");
                        switch (f.type) {
                            case FieldType::Quantity: std::printf("%.2f", double(v[i]) / 256.0); break;
                            case FieldType::Rate: std::printf("%.3f", double(v[i]) / 65536.0); break;
                            case FieldType::Count:
                            case FieldType::Ref: std::printf("%u", uint32_t(v[i])); break;
                            case FieldType::Money64:
                                std::printf("%lld", static_cast<long long>((uint64_t(uint32_t(v[2 * i + 1])) << 32) | uint32_t(v[2 * i])));
                                break;
                            default: std::printf("%lld", static_cast<long long>(v[i]));
                        }
                    }
                }
                std::printf("\n");
            }
        }
    }
    return identical ? 0 : 3;
}
