// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// The smallest complete Terni program: a world described in code, one rule, ten ticks.
// Walked through in docs/getting-started.md.
#include <cstdio>

#include "terni/fixed.hpp"
#include "terni/game.hpp"

int main() {
    terni::WorldBuilder b;
    const uint32_t town = b.entity("town", 1024);  // room for 1024 towns
    b.field(town, "pop", terni::FieldType::Quantity);
    b.field(town, "food", terni::FieldType::Quantity);
    b.define("growth", terni::FieldType::Rate);
    b.set_define("growth", terni::fx::rate(1, 100));  // 1 % per tick

    // A rule is the GLSL body of one compute stage. Terni generates the names it
    // uses from the description above: town_pop[], town_food[], D.growth, FOR_EACH_TOWN.
    b.stage("grow", "town", R"(
void main() {
    FOR_EACH_TOWN(t) {
        int pop = town_pop[t];
        if (town_food[t] >= pop) pop += fx_mul(pop, D.growth);  // fed towns grow
        town_pop[t] = pop;
    }
}
)");

    for (int i = 0; i < 3; ++i) {  // the starting state: three towns
        const uint32_t row = b.create(town);
        b.set(town, row, "pop", terni::fx::qty(1000 * (i + 1)));
        b.set(town, row, "food", terni::fx::qty(2500));
    }
    auto game = b.finish();
    for (const auto& e : game->errors) std::printf("error: %s\n", e.text().c_str());
    for (const auto& w : game->warnings) std::printf("warning: %s\n", w.text().c_str());
    if (!game->ok()) return 1;

    terni::GameSim sim(*game);  // picks a GPU, or a CPU Vulkan driver such as lavapipe
    for (int t = 0; t < 10; ++t) sim.tick();

    std::printf("after %llu ticks on %s:\n", static_cast<unsigned long long>(sim.ticks()), sim.device_name().c_str());
    const auto pops = sim.read_field(town, "pop", 0, 3);
    for (size_t i = 0; i < pops.size(); ++i) std::printf("  town %zu: %.2f people\n", i, terni::fx::qty_to_double(pops[i]));
    return 0;
}
