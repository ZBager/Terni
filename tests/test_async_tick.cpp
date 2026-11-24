// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// A tick that does not block: begin_tick() / tick_ready() / end_tick(). A host that
// issues its commands while the previous tick runs (writes, partial writes, inherit-field
// writes, timed modifiers) gets exactly the state of a host that calls tick() and issues
// them afterwards: per tick, in tick_result(), modifier totals, saves and the final hash.
// During a tick, tick_result() keeps the last collected tick's results. CI also runs this
// test under synchronization validation.
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

const char* GROW = R"(
void main() {
    FOR_EACH_TOWN(t) {
        town_pop[t] += 1 + town_stock[t * TOWN_STOCK_SIZE] + town_mod_tax(t) / 64;
        town_seen[t] = town_mod_tax(t);
    }
}
)";

constexpr uint32_t REALMS = 3, TOWNS = 24, TICKS = 40;

struct World {
    std::unique_ptr<Game> game;
    uint32_t realm, town, tax, levy;
};

World make_world() {
    WorldBuilder b;
    World w;
    w.realm = b.entity("realm", 4);
    w.town = b.entity("town", 32);
    b.field(w.town, "owner", FieldType::Ref, "realm");
    b.field(w.town, "pop", FieldType::Int);
    b.field(w.town, "seen", FieldType::Rate);
    b.field(w.town, "stock", FieldType::Int, "", 4);
    w.tax = b.modifier_key("tax", w.town, FieldType::Rate, "owner");
    w.levy = b.bundle("levy", {{w.tax, fx::rate(1, 10)}});
    b.stage("grow", "town", GROW);
    for (uint32_t r = 0; r < REALMS; ++r) b.create(w.realm);
    for (uint32_t t = 0; t < TOWNS; ++t) b.set(w.town, b.create(w.town), "owner", t % REALMS);
    w.game = b.finish();
    return w;
}

// The host's commands before tick k: the same on both sides.
void commands(GameSim& sim, const World& w, uint32_t k) {
    if (k % 3 == 0) sim.write_field(w.town, "stock", k % TOWNS, 0, {int32_t(k)});
    if (k % 4 == 1) sim.write_field(w.town, "owner", (k * 5) % TOWNS, {int32_t((k / 4) % REALMS)});
    if (k % 5 == 2) sim.add_modifier(w.levy, w.realm, k % REALMS, fx::rate(int32_t(k % 3) + 1), 3);  // timed
    if (k % 7 == 3) sim.add_modifier(w.levy, w.town, (k * 3) % TOWNS);
    if (k % 6 == 5) sim.write_field(w.town, "stock", k % TOWNS, 0, {0});
}

std::vector<int32_t> totals(GameSim& sim, const World& w) {
    std::vector<int32_t> out;
    for (uint32_t t = 0; t < TOWNS; ++t) out.push_back(sim.modifier_total(w.tax, w.town, t));
    return out;
}

std::vector<int32_t> result(GameSim& sim, uint32_t id) {
    const auto r = sim.tick_result(id);
    return {r.begin(), r.end()};
}

uint64_t state_hash(GameSim& sim) { return combine(hash_fields(sim.snapshot())); }

int main() {
    const World w = make_world();
    for (const auto& e : w.game->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& x : w.game->warnings) std::printf("  warning: %s\n", x.text().c_str());
    if (!w.game->ok()) return 1;

    std::vector<uint64_t> hashes;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        std::printf("device %u: %s\n", d.index, d.name.c_str());
        GameSimOptions o;
        o.device = int(d.index);

        // ---- reference: tick() and commands between ticks
        GameSim a(*w.game, o);
        const uint32_t a_pop = a.read_each_tick(w.town, "pop"), a_seen = a.read_each_tick(w.town, "seen");
        std::vector<std::vector<int32_t>> pop(TICKS), seen(TICKS), mods(TICKS);
        std::vector<uint8_t> a_save;
        for (uint32_t k = 0; k < TICKS; ++k) {
            commands(a, w, k);
            mods[k] = totals(a, w);
            if (k == TICKS / 2) a_save = a.save();
            a.tick();
            pop[k] = result(a, a_pop);
            seen[k] = result(a, a_seen);
        }

        // ---- the same game with each tick's commands issued while the previous tick runs
        GameSim b(*w.game, o);
        const uint32_t b_pop = b.read_each_tick(w.town, "pop"), b_seen = b.read_each_tick(w.town, "seen");
        uint64_t polls = 0, reads = 0;
        bool same_results = true, same_mods = true, same_during = true;
        commands(b, w, 0);
        same_mods &= totals(b, w) == mods[0];
        reads = b.sync_reads();  // the first inherited total read the owners once; no reads after this
        b.begin_tick();
        for (uint32_t k = 1; k < TICKS; ++k) {
            CHECK(b.tick_in_flight() && b.ticks() == k - 1);
            if (k >= 2) same_during &= result(b, b_pop) == pop[k - 2];  // the last collected tick
            commands(b, w, k);  // for tick k, made while tick k-1 runs
            same_mods &= totals(b, w) == mods[k];
            if (k == TICKS / 2) {
                CHECK(b.save() == a_save);  // collects tick k-1 first
                CHECK(!b.tick_in_flight() && b.ticks() == k);
                reads = b.sync_reads();  // a save reads the whole state
            }
            while (!b.tick_ready()) ++polls;
            // done on the GPU but not collected: still the last collected tick's results
            // (at the save, which collected tick k-1 already, those are tick k-1's)
            if (k >= 2) same_during &= result(b, b_pop) == pop[k == TICKS / 2 ? k - 1 : k - 2];
            b.end_tick();
            CHECK(!b.tick_in_flight() && b.ticks() == k);
            same_results &= result(b, b_pop) == pop[k - 1] && result(b, b_seen) == seen[k - 1];
            b.begin_tick();
        }
        b.end_tick();
        same_results &= result(b, b_pop) == pop[TICKS - 1] && result(b, b_seen) == seen[TICKS - 1];
        CHECK(same_results);
        CHECK(same_mods);
        CHECK(same_during);
        CHECK(b.sync_reads() == reads);
        CHECK(b.ticks() == TICKS && a.ticks() == TICKS);
        CHECK(state_hash(a) == state_hash(b));
        std::printf("  %llu polls while ticks ran\n", (unsigned long long)polls);

        // ---- calls that need the GPU state collect the tick first; one tick in flight at a time
        b.begin_tick();
        CHECK(b.read_field(w.town, "pop", 0, 1).size() == 1 && b.ticks() == TICKS + 1 && !b.tick_in_flight());
        b.begin_tick();
        b.begin_tick();  // collects the one before
        CHECK(b.ticks() == TICKS + 2 && b.tick_in_flight());
        b.end_tick();
        b.end_tick();  // nothing in flight: nothing to do
        CHECK(b.ticks() == TICKS + 3 && b.tick_ready());
        a.tick(), a.tick(), a.tick();
        CHECK(state_hash(a) == state_hash(b));
        hashes.push_back(state_hash(b));

        // ---- readbacks added and resized between ticks: each tick copies what is registered then
        {
            GameSim c(*w.game, o);
            std::vector<std::pair<uint32_t, const char*>> reads;
            auto matches = [&] {
                for (const auto& [id, field] : reads)
                    if (result(c, id) != c.read_field(w.town, field, 0, TOWNS)) return false;
                return true;
            };
            bool same = true;
            for (uint32_t k = 0; k < 14; ++k) {
                commands(c, w, k);
                if (k == 2) reads.push_back({c.read_each_tick(w.town, "pop"), "pop"});  // after ticks without any
                if (k == 5) reads.push_back({c.read_each_tick(w.town, "seen"), "seen"});  // one destination written
                c.begin_tick();
                if (k == 8) reads.push_back({c.read_each_tick(w.town, "owner"), "owner"});  // collects it first
                c.end_tick();
                if (k == 11) c.load(c.save());  // sizes the readbacks again
                if (k != 8 && k != 11) same &= matches();  // a new one has no results before its first tick
            }
            CHECK(same && reads.size() == 3);
        }

        // ---- waits that spin first: the same ticks, whether the spin sees them done or gives up
        for (uint32_t us : {1u, 1000000u}) {
            o.spin_us = us;
            GameSim c(*w.game, o);
            for (uint32_t k = 0; k < TICKS; ++k) {
                commands(c, w, k);
                if (k % 2 == 0) c.tick();
                else c.begin_tick(), c.end_tick();
            }
            c.begin_tick();
            CHECK(c.read_field(w.town, "pop", 0, 1).size() == 1);  // collects it
            c.move_to_device(d.index);
            c.tick(), c.tick();
            CHECK(c.ticks() == TICKS + 3 && state_hash(c) == state_hash(b));
        }
    }
    CHECK(!hashes.empty());
    for (uint64_t h : hashes) CHECK(h == hashes.front());

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ticks that do not block: ok\n");
    return failures ? 1 : 0;
}
