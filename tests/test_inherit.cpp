// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Inherited modifier totals on the host: modifier_total / modifier_breakdown follow the
// ref field that rules change every tick (owner), host writes and load(), and agree with
// what the GPU accessor sees, without waiting for the GPU after the first query. Inherit
// fields the generated accessor cannot honour are rejected.
#include <cstdio>
#include <string>
#include <vector>

#include "terni/fixed.hpp"
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

// `seen` is what the GPU accessor gives at the start of the tick; then every planet
// changes owner, so the host has to follow the rule's writes.
const char* MOVE = R"(
void main() {
    FOR_EACH_PLANET(p) {
        planet_seen[p] = planet_mod_growth(p);
        planet_owner[p] = (planet_owner[p] + p + terni.tick) % terni.empire_count;
    }
}
)";

int main() {
    constexpr uint32_t EMPIRES = 4, PLANETS = 40;
    WorldBuilder b;
    const uint32_t empire = b.entity("empire", 8), planet = b.entity("planet", 64);
    b.field(planet, "owner", FieldType::Ref, "empire");
    b.field(planet, "seen", FieldType::Rate);
    const uint32_t growth = b.modifier_key("growth", planet, FieldType::Rate, "owner");
    const uint32_t boost = b.bundle("boost", {{growth, fx::rate(1, 100)}});
    b.stage("move", "planet", MOVE);
    for (uint32_t e = 0; e < EMPIRES; ++e) b.create(empire);
    for (uint32_t p = 0; p < PLANETS; ++p) b.set(planet, b.create(planet), "owner", p % EMPIRES);
    auto world = b.finish();
    for (const auto& e : world->errors) std::printf("  %s\n", e.text().c_str());
    if (!world->ok()) return 1;

    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        GameSimOptions o;
        o.device = int(d.index);
        GameSim sim(*world, o);
        for (uint32_t e = 0; e < EMPIRES; ++e) sim.add_modifier(boost, empire, e, fx::rate(int32_t(e) + 1));
        sim.add_modifier(boost, planet, 3, fx::rate(7));
        sim.add_modifier(boost, planet, 7, fx::rate(-2));
        sim.tick();  // the modifiers reach the GPU

        auto expected = [&](const std::vector<int32_t>& owners, uint32_t p) {
            return sim.modifier_own_total(growth, planet, p) + sim.modifier_own_total(growth, empire, uint32_t(owners[p]));
        };
        auto totals = [&] {
            std::vector<int32_t> t(PLANETS);
            for (uint32_t p = 0; p < PLANETS; ++p) t[p] = sim.modifier_total(growth, planet, p);
            return t;
        };
        uint64_t reads = sim.sync_reads();
        std::vector<int32_t> before = totals();
        CHECK(sim.sync_reads() == reads + 1);  // the first query reads the owners once
        bool same = true, quiet = true, agrees = true;
        for (int t = 0; t < 6; ++t) {
            std::vector<int32_t> owners = sim.read_field(planet, "owner");
            for (uint32_t p = 0; p < PLANETS; ++p) same &= before[p] == expected(owners, p);
            reads = sim.sync_reads();
            sim.tick();
            const std::vector<int32_t> after = totals();
            for (uint32_t p = 0; p < PLANETS; ++p) {
                const auto parts = sim.modifier_breakdown(growth, planet, p);
                int32_t sum = 0;
                for (const auto& c : parts) sum += c.value;
                same &= sum == after[p];
            }
            quiet &= sim.sync_reads() == reads;  // no waiting for the GPU after a tick
            const std::vector<int32_t> seen = sim.read_field(planet, "seen");
            for (uint32_t p = 0; p < PLANETS; ++p) agrees &= seen[p] == before[p];  // the GPU saw what the host said
            before = after;
        }
        CHECK(same);
        CHECK(quiet);
        CHECK(agrees);

        // a host write counts at once (it applies before the next tick's stages)
        sim.write_field(planet, "owner", 5, {3});
        const int32_t written = sim.modifier_total(growth, planet, 5);
        CHECK(written == sim.modifier_own_total(growth, planet, 5) + sim.modifier_own_total(growth, empire, 3));
        bool from_three = false;
        for (const auto& c : sim.modifier_breakdown(growth, planet, 5)) from_three |= c.entity == empire && c.row == 3;
        CHECK(from_three);
        sim.tick();
        CHECK(sim.read_field(planet, "seen", 5, 1).at(0) == written);

        // load() brings back the saved owners
        const std::vector<uint8_t> save = sim.save();
        const std::vector<int32_t> saved = sim.read_field(planet, "owner");
        const std::vector<int32_t> saved_totals = totals();
        sim.tick();
        sim.tick();
        sim.load(save);
        CHECK(totals() == saved_totals);
        bool loaded = true;
        for (uint32_t p = 0; p < PLANETS; ++p) loaded &= saved_totals[p] == expected(saved, p);
        CHECK(loaded);
        std::printf("inherited totals on %-45s %s\n", d.name.c_str(), same && quiet && agrees && loaded ? "ok" : "WRONG");
    }

    // inherit fields the generated accessor could not follow are rejected
    {
        WorldBuilder w;
        const uint32_t e = w.entity("empire", 4), p = w.entity("planet", 4);
        w.field(p, "owners", FieldType::Ref, "empire", 2);
        w.field(p, "parent", FieldType::Ref, "planet");
        w.modifier_key("two", p, FieldType::Rate, "owners");
        w.modifier_key("self", p, FieldType::Rate, "parent");
        (void)e;
        auto bad = w.finish();
        bool size_error = false, self_error = false;
        for (const auto& err : bad->errors) {
            size_error |= err.message.find("must hold one ref") != std::string::npos;
            self_error |= err.message.find("must refer to another entity type") != std::string::npos;
        }
        CHECK(size_error && self_error && bad->errors.size() == 2);
    }

    std::printf("test_inherit: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
