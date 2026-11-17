// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Modifiers through the public API, with a world built in code (no data folder):
// inheritance, rescale, duration/expiry timing, stale handles, breakdowns,
// inapplicable keys, field read/write, and identical results on every device.
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

namespace {

struct World {
    std::unique_ptr<Game> game;
    uint32_t empire, planet;
    uint32_t pop_growth, slots, research;
    uint32_t farming, war_economy, extra_slots;
};

// The observing rule copies what the GPU sees through the generated accessors.
const char* OBSERVE = R"(
void main() {
    FOR_EACH_PLANET(p) {
        planet_seen_growth[p] = planet_mod_pop_growth(p);
        planet_seen_slots[p] = planet_mod_slots(p);
        planet_pop[p] += fx_mul(planet_pop[p], fx_mul(D.base_growth, FX_ONE + planet_mod_pop_growth(p)));
    }
}
)";

World build() {
    World w;
    WorldBuilder b;
    w.empire = b.entity("empire", 8);
    w.planet = b.entity("planet", 16);
    b.field(w.planet, "owner", FieldType::Ref, "empire");
    b.field(w.planet, "pop", FieldType::Quantity);
    b.field(w.planet, "seen_growth", FieldType::Rate);
    b.field(w.planet, "seen_slots", FieldType::Quantity);
    b.define("base_growth", FieldType::Rate);
    b.set_define("base_growth", fx::rate(1, 100));
    w.pop_growth = b.modifier_key("pop_growth", w.planet, FieldType::Rate, "owner");
    w.slots = b.modifier_key("slots", w.planet, FieldType::Quantity);
    w.research = b.modifier_key("research", w.empire, FieldType::Rate);
    w.farming = b.bundle("farming", {{w.pop_growth, fx::rate(5, 100)}});
    w.war_economy = b.bundle("war_economy", {{w.pop_growth, fx::rate(-2, 100)}, {w.research, fx::rate(10, 100)}});
    w.extra_slots = b.bundle("extra_slots", {{w.slots, fx::qty(2)}});
    b.stage("observe", "planet", OBSERVE);
    for (int e = 0; e < 2; ++e) b.create(w.empire);
    const uint32_t owners[3] = {0, 0, 1};
    for (uint32_t p = 0; p < 3; ++p) {
        const uint32_t row = b.create(w.planet);
        b.set(w.planet, row, "owner", owners[p]);
        b.set(w.planet, row, "pop", fx::qty(1000));
    }
    w.game = b.finish();
    for (const auto& e : w.game->errors) std::printf("  build error: %s\n", e.text().c_str());
    return w;
}

int32_t seen(GameSim& sim, const World& w, const char* field, uint32_t planet) {
    return sim.read_field(w.planet, field, planet, 1).at(0);
}

// The same scripted host session on any device; returns the final snapshot.
StateSnapshot session(const World& w, uint32_t device, bool checks) {
    GameSim sim(*w.game, device);
    const int32_t farm = fx::rate(5, 100), war = fx::rate(-2, 100);

    // 1. own + inherited
    ModifierHandle f = sim.add_modifier(w.farming, w.planet, 0);
    ModifierHandle we = sim.add_modifier(w.war_economy, w.empire, 0);
    sim.tick();
    if (checks) {
        CHECK(seen(sim, w, "seen_growth", 0) == farm + war);  // own farming + owner's war economy
        CHECK(seen(sim, w, "seen_growth", 1) == war);         // inherited only
        CHECK(seen(sim, w, "seen_growth", 2) == 0);           // other owner
        CHECK(sim.modifier_total(w.pop_growth, w.planet, 0) == farm + war);
        CHECK(sim.modifier_own_total(w.research, w.empire, 0) == fx::rate(10, 100));
        const auto br = sim.modifier_breakdown(w.pop_growth, w.planet, 0);
        CHECK(br.size() == 2 && br[0].bundle == w.farming && br[1].bundle == w.war_economy && br[1].entity == w.empire);
    }

    // 2. rescale
    CHECK(sim.set_modifier_scale(f, fx::rate(2)));
    sim.tick();
    if (checks) CHECK(seen(sim, w, "seen_growth", 0) == fx::mul(farm, fx::rate(2)) + war);

    // 3. duration: active for exactly 3 ticks
    sim.add_modifier(w.extra_slots, w.planet, 2, fx::ONE, 3);
    for (int t = 0; t < 5; ++t) {
        sim.tick();
        if (checks) CHECK(seen(sim, w, "seen_slots", 2) == (t < 3 ? fx::qty(2) : 0));
    }

    // 4. remove, stale handles, slot reuse
    CHECK(sim.remove_modifier(f));
    if (checks) {
        CHECK(!sim.remove_modifier(f));
        CHECK(!sim.set_modifier_scale(f, fx::ONE));
    }
    ModifierHandle f2 = sim.add_modifier(w.farming, w.planet, 1);
    if (checks) {
        CHECK(f2.slot == f.slot && f2.version != f.version);  // slot reused, old handle stays stale
        CHECK(!sim.modifier_alive(f) && sim.modifier_alive(f2));
    }
    sim.tick();
    if (checks) {
        CHECK(seen(sim, w, "seen_growth", 0) == war);
        CHECK(seen(sim, w, "seen_growth", 1) == farm + war);
    }

    // 5. a key that doesn't apply to the entity type is ignored
    sim.add_modifier(w.extra_slots, w.empire, 1);
    sim.tick();
    if (checks) CHECK(seen(sim, w, "seen_slots", 2) == 0);

    // 6. host field write takes effect at the next tick
    sim.write_field(w.planet, "pop", 2, {fx::qty(5000)});
    sim.tick();
    if (checks) CHECK(seen(sim, w, "pop", 2) == fx::qty(5000) + fx::mul(fx::qty(5000), fx::rate(1, 100)));

    // 7. removing the inherited source
    CHECK(sim.remove_modifier(we));
    sim.tick();
    if (checks) {
        CHECK(seen(sim, w, "seen_growth", 1) == farm);
        CHECK(sim.modifier_count() == 2);  // f2 + inapplicable extra_slots on the empire
    }
    return sim.snapshot();
}

}  // namespace

int main() {
    const World w = build();
    CHECK(w.game->ok());
    if (!w.game->ok()) return 1;

    std::vector<StateSnapshot> results;
    std::vector<std::string> names;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        results.push_back(session(w, d.index, results.empty()));
        names.push_back(d.name);
    }
    CHECK(!results.empty());
    for (size_t i = 1; i < results.size(); ++i) {
        const std::string diff = first_difference(results[0], results[i]);
        if (!diff.empty()) std::printf("  %s differs from %s in %s\n", names[i].c_str(), names[0].c_str(), diff.c_str());
        CHECK(diff.empty());
    }
    std::printf("test_modifiers: %s (%zu device(s))\n", failures == 0 ? "PASS" : "FAIL", results.size());
    return failures == 0 ? 0 : 1;
}
