// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// A long run: the benchmark world for thousands of ticks, with the host's events, armies
// that come and go, and saves loaded on another device along the way. Every run starts on
// a different device and moves through the others; the device-side hashes taken along the
// way must be the same whichever device ran each part, equal the host's hashes of a
// snapshot, and the end state must equal the CPU reference's.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bench_world.hpp"
#include "cpu_reference.hpp"
#include "terni/vk_context.hpp"

using namespace terni;
using namespace terni::bench;

int main() {
    BenchConfig config;
    find_size("tiny", config);
    auto game = build_world(config);
    for (const auto& e : game->errors) std::printf("error: %s\n", e.text().c_str());
    if (!game->ok()) return 1;
    std::vector<uint32_t> devices;
    for (const auto& d : VulkanContext::list_devices())
        if (d.usable) devices.push_back(d.index);
    if (devices.empty()) return 1;

    constexpr uint64_t TICKS = 3000, EVERY = 100, MOVE = 1000;
    int failures = 0;
    std::vector<uint64_t> reference;  // per checkpoint, from the first run
    StateHashes end;
    for (size_t start = 0; start < devices.size(); ++start) {
        size_t on = start;
        auto sim = std::make_unique<GameSim>(*game, devices[on]);
        std::vector<HostEvent> events;
        std::vector<uint64_t> seen;
        uint32_t highest_generation = 0;
        bool host_matches = true;
        for (uint64_t t = 0; t < TICKS; ++t) {
            host_events(*game, config, sim->ticks(), events);
            for (const auto& ev : events) sim->add_modifier(ev.bundle, ev.entity, ev.row, ev.scale, ev.duration);
            sim->tick();
            if ((t + 1) % EVERY == 0) seen.push_back(combine(sim->field_hashes()));
            if ((t + 1) % MOVE == 0 && t + 1 < TICKS) {  // on to the next device, through a save
                host_matches &= sim->field_hashes() == field_hashes(sim->snapshot());
                const std::vector<uint8_t> save = sim->save();
                on = (on + 1) % devices.size();
                sim = std::make_unique<GameSim>(*game, devices[on]);
                sim->load(save);
            }
            for (uint32_t a = 0; a < sim->entity_count(ARMY); ++a)
                highest_generation = std::max(highest_generation, sim->entity_generation(ARMY, a));
        }
        const bool same = start == 0 || seen == reference;
        if (start == 0) reference = seen;
        const StateHashes mine = state_hashes(*sim, *game);
        if (start == 0) end = mine;
        const bool agree = first_difference(mine, end).empty();
        failures += !same + !agree + !host_matches + (highest_generation < 3);
        std::printf("from device %u: %s, %zu checkpoints, armies reused up to generation %u\n", devices[start],
                    same && agree && host_matches ? "identical" : "DIFFERS", seen.size(), highest_generation);
    }

    CpuWorld cpu(*game, config, 2);  // it takes the host's events itself
    for (uint64_t t = 0; t < TICKS; ++t) cpu.tick();
    const std::string diff = first_difference(end, cpu.state_hashes());
    if (!diff.empty()) {
        std::printf("the CPU reference differs after %llu ticks: %s\n", static_cast<unsigned long long>(TICKS), diff.c_str());
        ++failures;
    }
    std::printf("test_soak: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
