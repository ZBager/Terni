// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Rules a mod may get wrong. A rule that reads or writes past the end of a field, far past
// it or just past it, reads 0 and writes nothing, with plain writes and atomics alike, on
// every device, so a broken mod stays deterministic and harms no other field. A lost
// device is reported as terni::DeviceLost.
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

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

constexpr uint32_t TOWNS = 37;  // the capacity, no multiple of anything

const char* REACH = R"(
void main() {
    FOR_EACH_TOWN(t) {
        town_seen[t] = town_pop[t + 1000000u];                         // terni: order ok
        town_edge[t] = town_stock[TOWN_CAPACITY * 3u + t];             // terni: order ok
        town_pop[t + 2000000u] = 99;                                   // terni: order ok
        town_stock[TOWN_CAPACITY * 3u + t] = 5;                        // terni: order ok
        atomicAdd(town_total[t + TOWN_CAPACITY], 1);                   // terni: order ok
        atomicAdd(town_total[t], 1);
        town_pop[t] += 1;
    }
}
)";

int main() {
    WorldBuilder b;
    const uint32_t town = b.entity("town", TOWNS);
    b.field(town, "pop", FieldType::Int);
    b.field(town, "seen", FieldType::Int, "", 1, -1);
    b.field(town, "edge", FieldType::Int, "", 1, -1);
    b.field(town, "stock", FieldType::Int, "", 3);
    b.field(town, "total", FieldType::Agg);
    b.stage("reach", "town", REACH);
    for (uint32_t t = 0; t < TOWNS; ++t) {
        b.set(town, b.create(town), "pop", 10 * t);
        for (uint32_t k = 0; k < 3; ++k) b.set(town, t, "stock", 100 + t * 3 + k, k);
    }
    const auto game = b.finish();
    for (const auto& e : game->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& w : game->warnings) std::printf("  warning: %s\n", w.text().c_str());
    CHECK(game->ok() && game->warnings.empty());
    if (!game->ok()) return 1;

    std::vector<StateSnapshot> ends;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        if (d.robustness != BufferRobustness::Defined) {  // not on today's drivers; nothing defined to check
            std::printf("%-45.45s no robustBufferAccess2: skipped\n", d.name.c_str());
            continue;
        }
        GameSim sim(*game, d.index);
        for (int t = 0; t < 3; ++t) sim.tick();
        bool ok = true;
        const auto pop = sim.read_field(town, "pop"), seen = sim.read_field(town, "seen");
        const auto edge = sim.read_field(town, "edge"), stock = sim.read_field(town, "stock");
        const auto total = sim.read_field(town, "total");
        for (uint32_t t = 0; t < TOWNS; ++t) {
            ok &= pop[t] == int32_t(10 * t + 3) && seen[t] == 0 && edge[t] == 0 && total[t] == 3;
            for (uint32_t k = 0; k < 3; ++k) ok &= stock[t * 3 + k] == int32_t(100 + t * 3 + k);
        }
        CHECK(ok);
        ends.push_back(sim.snapshot());
        const bool same = first_difference(ends.front(), ends.back()).empty();
        CHECK(same);
        std::printf("%-45.45s past the ends: read 0, wrote nothing %s\n", d.name.c_str(), ok && same ? "ok" : "WRONG");
    }
    CHECK(!ends.empty());

    // a lost device is its own exception; other Vulkan errors are not
    bool lost = false, other = false;
    try {
        VK_CHECK(VK_ERROR_DEVICE_LOST);
    } catch (const DeviceLost&) {
        lost = true;
    }
    try {
        VK_CHECK(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    } catch (const DeviceLost&) {
    } catch (const std::runtime_error&) {
        other = true;
    }
    CHECK(lost && other);

    std::printf("test_broken_rules: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
