// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The benchmark world at its smallest size: on every device, Terni and the CPU reference
// (on one thread and on several) must have the same state after every tick. This keeps
// the CPU reference in step with the rules; terni_bench checks the same at every size.
// Timings are never checked here.
#include <algorithm>
#include <cstdio>
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
    for (const auto& w : game->warnings) std::printf("warning: %s\n", w.text().c_str());
    if (!game->ok() || !game->warnings.empty()) return 1;

    constexpr int TICKS = 70;  // past two monthly stages
    int failures = 0, tested = 0;
    for (const auto& dev : VulkanContext::list_devices()) {
        if (!dev.usable) continue;
        ++tested;
        GameSim sim(*game, dev.index);
        CpuWorld one(*game, config, 1), several(*game, config, 3);
        std::vector<HostEvent> events;
        std::string problem;
        uint32_t most_alive = 0, born = 0;
        for (int t = 0; t < TICKS && problem.empty(); ++t) {
            host_events(*game, config, sim.ticks(), events);
            for (const auto& ev : events) sim.add_modifier(ev.bundle, ev.entity, ev.row, ev.scale, ev.duration);
            if (t < 2) {  // no stage may depend on the order its threads run in
                for (const auto& issue : sim.tick_checked())
                    problem = "stage " + issue.stage + " depends on thread order (" + issue.field + ")";
            } else {
                sim.tick();
            }
            one.tick();
            several.tick();
            const StateHashes gpu = state_hashes(sim, *game);
            for (const CpuWorld* cpu : {&one, &several}) {
                const std::string diff = first_difference(gpu, cpu->state_hashes());
                if (!diff.empty() && problem.empty())
                    problem = "tick " + std::to_string(t + 1) + ": " + diff + " differs from the CPU reference on " +
                              std::to_string(cpu->threads()) + " thread(s)";
            }
            most_alive = std::max(most_alive, sim.entities_alive(ARMY));
            for (uint32_t a = 0; a < sim.entity_count(ARMY); ++a) born = std::max(born, sim.entity_generation(ARMY, a));
        }
        // the test is only worth something if armies came and went
        if (problem.empty() && (born < 2 || sim.entities_alive(ARMY) == sim.entity_count(ARMY)))
            problem = "no army row was destroyed and reused; the world needs more churn";
        std::printf("[%u] %-45s %s (armies alive at most %u, highest generation %u)\n", dev.index, dev.name.c_str(),
                    problem.empty() ? "identical" : problem.c_str(), most_alive, born);
        failures += !problem.empty();
    }
    std::printf("test_bench: %s (%d device(s))\n", tested && !failures ? "PASS" : "FAIL", tested);
    return tested && !failures ? 0 : 1;
}
