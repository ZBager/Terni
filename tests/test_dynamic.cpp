// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Entities at run time: provinces ask for armies, armies wear out and destroy themselves,
// the host creates and destroys some too. Every device is compared, tick by tick, with a
// model of the same world on the CPU: which rows are alive, which row each request got
// (the lowest free one), generations, fields, counts and the modifiers of destroyed rows.
// The session also saves and loads, moves device, runs a checked tick and creates a row
// while a tick is in flight.
#include <cstdio>
#include <stdexcept>
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

constexpr uint32_t PROVINCES = 12, PROVINCE_CAPACITY = 16, ARMIES = 3, ARMY_CAPACITY = 24;

const char* RECRUIT = R"(
void main() {
    FOR_EACH_PROVINCE(p) {
        if ((terni.tick + p) % 5u == 0u) province_raise[p] += 1u + p % 3u;
    }
}
)";
// a new army (fresh is 1 by default) takes its strength from the province that asked
const char* MARCH = R"(
void main() {
    FOR_EACH_ARMY(a) {
        if (army_fresh[a] != 0) {
            army_fresh[a] = 0;
            army_strength[a] = 10 + int(army_home[a] % 7u);
        }
        army_age[a] += 1;
        army_seen_generation[a] = int(army_generation[a]);
        army_strength[a] -= 2;
        if (army_strength[a] <= 0) army_destroy(a);
    }
}
)";
const char* COUNT = R"(
void main() {
    FOR_EACH_ARMY(a) {
        if (army_home[a] < PROVINCE_CAPACITY) atomicAdd(province_armies[army_home[a]], 1);
    }
}
)";

struct World {
    std::unique_ptr<Game> game;
    uint32_t province = 0, army = 0, boost = 0;
};

static World build() {
    World w;
    WorldBuilder b;
    w.province = b.entity("province", PROVINCE_CAPACITY);
    b.field(w.province, "raise", FieldType::Count);
    b.field(w.province, "armies", FieldType::Agg);
    w.army = b.entity("army", ARMY_CAPACITY);
    b.entity_dynamic(w.army);
    b.field(w.army, "home", FieldType::Ref, "province");
    b.field(w.army, "strength", FieldType::Int);
    b.field(w.army, "fresh", FieldType::Int, "", 1, 1);
    b.field(w.army, "age", FieldType::Int);
    b.field(w.army, "seen_generation", FieldType::Int);
    b.field(w.army, "purse", FieldType::Money64, "", 2, 5000000000ll);  // a sized, two-word default
    b.spawn(w.army, "province", "raise", "home");
    const uint32_t morale = b.modifier_key("morale", w.army, FieldType::Rate);
    w.boost = b.bundle("boost", {{morale, fx::rate(1, 10)}});
    b.stage("recruit", "province", RECRUIT);
    b.stage("march", "army", MARCH);
    b.stage("count", "army", COUNT, {"province.armies"});
    for (uint32_t p = 0; p < PROVINCES; ++p) b.create(w.province);
    for (uint32_t a = 0; a < ARMIES; ++a) b.set(w.army, b.create(w.army), "home", a + 1);
    w.game = b.finish();
    for (const auto& e : w.game->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& x : w.game->warnings) std::printf("  warning: %s\n", x.text().c_str());
    return w;
}

// The same world on the CPU.
struct Model {
    uint64_t tick = 0;
    std::vector<uint32_t> raise = std::vector<uint32_t>(PROVINCE_CAPACITY, 0);
    std::vector<int32_t> armies = std::vector<int32_t>(PROVINCE_CAPACITY, 0);
    std::vector<bool> alive = std::vector<bool>(ARMY_CAPACITY, false);
    std::vector<uint32_t> generation = std::vector<uint32_t>(ARMY_CAPACITY, 0);
    std::vector<int32_t> home = std::vector<int32_t>(ARMY_CAPACITY, 0), strength = home, fresh = std::vector<int32_t>(ARMY_CAPACITY, 1),
                         age = home, seen_generation = home;
    uint32_t count = 0;
    bool was_full = false, reused = false;  // what the session must have gone through
    // What read_field() gives: the fields as the last tick left them. New rows, and
    // anything else the host wrote since, reach the device with the next tick.
    struct Fields {
        std::vector<uint32_t> raise;
        std::vector<int32_t> armies, home, strength, fresh, age, seen_generation;
    } device;
    void sync() { device = {raise, armies, home, strength, fresh, age, seen_generation}; }

    Model() {
        for (uint32_t a = 0; a < ARMIES; ++a) alive[a] = true, generation[a] = 1, home[a] = int32_t(a + 1);
        count = ARMIES;
        sync();
    }
    uint32_t alive_count() const {
        uint32_t n = 0;
        for (bool a : alive) n += a;
        return n;
    }
    uint32_t create() {  // lowest free row, every field at its default
        for (uint32_t a = 0; a < ARMY_CAPACITY; ++a) {
            if (alive[a]) continue;
            reused |= generation[a] > 0;
            alive[a] = true;
            ++generation[a];
            home[a] = 0, strength[a] = 0, fresh[a] = 1, age[a] = 0, seen_generation[a] = 0;
            count = std::max(count, a + 1);
            return a;
        }
        return NO_ENTITY;
    }
    void step() {
        for (uint32_t p = 0; p < PROVINCES; ++p)
            if ((tick + p) % 5 == 0) raise[p] += 1 + p % 3;
        for (uint32_t a = 0; a < count; ++a) {
            if (!alive[a]) continue;
            if (fresh[a] != 0) fresh[a] = 0, strength[a] = 10 + home[a] % 7;
            age[a] += 1;
            seen_generation[a] = int32_t(generation[a]);
            strength[a] -= 2;
            if (strength[a] <= 0) alive[a] = false;
        }
        std::fill(armies.begin(), armies.end(), 0);
        for (uint32_t a = 0; a < count; ++a)
            if (alive[a]) ++armies[size_t(home[a])];
        ++tick;
        sync();
        // the rows the provinces asked for, in province order
        for (uint32_t p = 0; p < PROVINCES; ++p)
            while (raise[p] > 0) {
                const uint32_t a = create();
                if (a == NO_ENTITY) {
                    was_full = true;
                    break;
                }
                home[a] = int32_t(p);
                --raise[p];
            }
    }
};

static bool matches(GameSim& sim, const World& w, const Model& m) {
    bool ok = true;
    auto same = [&](uint32_t entity, const char* field, const std::vector<int32_t>& want) {
        const bool eq = sim.read_field(entity, field) == want;
        if (!eq) std::printf("  tick %llu: %s differs\n", (unsigned long long)m.tick, field);
        ok &= eq;
    };
    same(w.province, "raise", std::vector<int32_t>(m.device.raise.begin(), m.device.raise.end()));
    same(w.province, "armies", m.device.armies);
    same(w.army, "home", m.device.home);
    same(w.army, "strength", m.device.strength);
    same(w.army, "fresh", m.device.fresh);
    same(w.army, "age", m.device.age);
    same(w.army, "seen_generation", m.device.seen_generation);
    for (uint32_t a = 0; a < ARMY_CAPACITY; ++a) {
        ok &= sim.entity_alive(w.army, a) == m.alive[a];
        ok &= sim.entity_generation(w.army, a) == m.generation[a];
    }
    ok &= sim.entity_count(w.army) == m.count && sim.entities_alive(w.army) == m.alive_count();
    ok &= !sim.entity_alive(w.army, ARMY_CAPACITY) && sim.entity_generation(w.army, ARMY_CAPACITY + 7) == 0;
    return ok;
}

// One session on device `from`, moving to device `to` on the way. Returns the final state.
static StateSnapshot session(const World& w, uint32_t from, uint32_t to) {
    GameSim sim(*w.game, from);
    Model m;
    bool ok = matches(sim, w, m);
    struct Held {
        ModifierHandle handle;
        uint32_t row, generation;
    };
    std::vector<Held> held;  // modifiers on armies: gone as soon as their army is
    auto modifiers_follow = [&] {
        bool good = true;
        for (const auto& h : held)
            good &= sim.modifier_alive(h.handle) == (m.alive[h.row] && m.generation[h.row] == h.generation);
        return good;
    };
    std::vector<uint8_t> save;
    Model saved;
    const uint32_t tick_reads_stay = sim.read_each_tick(w.army, "strength");  // follows the rows in use as they grow

    for (int t = 0; t < 60; ++t) {
        // ---- the host's turn before tick t
        if (t == 4 || t == 30) {
            const uint32_t row = sim.create_entity(w.army);
            const uint32_t want = m.create();
            ok &= row == want;
            if (row != NO_ENTITY) {
                sim.write_field(w.army, "home", row, {2});
                m.home[row] = 2;
                ok &= sim.entity_alive(w.army, row);  // at once on the host; rules see it from the next tick
            }
        }
        if (t == 6 || t == 26)
            for (uint32_t a = 0; a < ARMY_CAPACITY; ++a)
                if (m.alive[a]) held.push_back({sim.add_modifier(w.boost, w.army, a), a, m.generation[a]});
        if (t == 9 || t == 33) {
            uint32_t a = 0;
            while (a < ARMY_CAPACITY && !m.alive[a]) ++a;
            if (a < ARMY_CAPACITY) {
                ok &= sim.destroy_entity(w.army, a) && !sim.destroy_entity(w.army, a);
                m.alive[a] = false;
                ok &= !sim.entity_alive(w.army, a) && modifiers_follow();
            }
        }
        if (t == 13) sim.move_to_device(to);
        if (t == 15) save = sim.save(), saved = m;
        if (t == 22) {  // back to tick 15, and on from there
            sim.load(save);
            m = saved;
            m.sync();  // a save holds the writes that were waiting for the next tick
            ok &= matches(sim, w, m);
        }

        // ---- tick t
        if (t == 17) {
            const auto issues = sim.tick_checked();
            for (const auto& i : issues) std::printf("  order issue: %s in %s\n", i.field.c_str(), i.stage.c_str());
            ok &= issues.empty();
            m.step();
        } else if (t == 19 || t == 40) {
            sim.begin_tick();
            const uint32_t row = sim.create_entity(w.army);  // collects the tick: its requests come first
            m.step();
            ok &= !sim.tick_in_flight() && row == m.create();
        } else {
            sim.tick();
            m.step();
        }
        ok &= matches(sim, w, m) && modifiers_follow();
        ok &= sim.tick_result(tick_reads_stay).size() == m.count || t == 22;  // (stale just after load)
    }
    CHECK(ok);
    CHECK(m.was_full && m.reused);  // the session met a full entity type and reused rows
    CHECK(sim.entities_alive(w.army) > 0 && sim.entity_count(w.army) == ARMY_CAPACITY);
    return sim.snapshot();
}

static bool has(const Game& g, const std::string& text) {
    for (const auto& e : g.errors)
        if (e.message.find(text) != std::string::npos) return true;
    return false;
}

int main() {
    const World w = build();
    if (!w.game->ok()) return 1;
    CHECK(w.game->warnings.empty());
    std::vector<DeviceInfo> devices;
    for (const auto& d : VulkanContext::list_devices())
        if (d.usable) devices.push_back(d);
    if (devices.empty()) {
        std::puts("no usable Vulkan device");
        return 1;
    }

    std::vector<StateSnapshot> ends;
    for (size_t i = 0; i < devices.size(); ++i) {
        const DeviceInfo& to = devices[(i + 1) % devices.size()];
        ends.push_back(session(w, devices[i].index, to.index));
        const bool same = first_difference(ends[0], ends.back()).empty();
        CHECK(same);
        std::printf("armies come and go on %-40.40s (then %.30s) %s\n", devices[i].name.c_str(), to.name.c_str(),
                    same ? "ok" : "DIFFERS");
    }

    // entity types that are not dynamic: the rows in use are alive, and there is nothing to create
    {
        GameSim sim(*w.game, devices[0].index);
        CHECK(sim.entity_alive(w.province, PROVINCES - 1) && !sim.entity_alive(w.province, PROVINCES));
        CHECK(sim.entity_count(w.province) == PROVINCES && sim.entities_alive(w.province) == PROVINCES);
        CHECK(sim.entity_generation(w.province, 0) == 1 && sim.entity_generation(w.province, PROVINCES) == 0);
        bool thrown = false;
        try {
            sim.create_entity(w.province);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        CHECK(thrown);
        // a new row has its defaults, whatever the row held before
        const uint32_t row = sim.create_entity(w.army);
        CHECK(row == ARMIES);
        sim.write_field(w.army, "purse", row, {1, 2, 3, 4});
        sim.write_field(w.army, "fresh", row, {0});
        sim.write_field(w.army, "strength", row, {1});  // worn out in the first tick
        sim.tick();
        CHECK(sim.read_field(w.army, "purse", row, 1) == std::vector<int32_t>({1, 2, 3, 4}));
        while (sim.entity_generation(w.army, row) < 2) sim.tick();  // a province's request takes the row
        sim.tick();                                                 // and the new army reaches the device
        const int32_t lo = int32_t(uint32_t(5000000000ull)), hi = int32_t(5000000000ull >> 32);
        CHECK(sim.read_field(w.army, "purse", row, 1) == std::vector<int32_t>({lo, hi, lo, hi}));
    }

    // what cannot work is refused when the world is built
    {
        WorldBuilder b;
        const uint32_t town = b.entity("town", 4), cart = b.entity("cart", 4), mill = b.entity("mill", 4);
        b.field(town, "want", FieldType::Int);
        b.field(town, "carts", FieldType::Count, "", 2);
        b.field(cart, "alive", FieldType::Int);
        b.field(cart, "owner", FieldType::Ref, "mill");
        b.entity_dynamic(cart);
        b.spawn(cart, "town", "want");            // not a count field
        b.spawn(cart, "town", "carts");           // two values per row
        b.spawn(cart, "village", "want");         // no such entity type
        b.spawn(cart, "mill", "missing", "owner");
        b.field(mill, "need", FieldType::Count);
        b.spawn(cart, "town", "want", "owner");   // owner refers to mill, not town
        b.spawn(mill, "town", "want");            // mill is not dynamic
        const auto bad = b.finish();
        CHECK(has(*bad, "field 'alive' of dynamic entity 'cart'"));
        CHECK(has(*bad, "'want' is not a single count field of 'town'"));
        CHECK(has(*bad, "'carts' is not a single count field of 'town'"));
        CHECK(has(*bad, "unknown entity 'village'"));
        CHECK(has(*bad, "'missing' is not a single count field of 'mill'"));
        CHECK(has(*bad, "'owner' is not a single ref:town field of 'cart'"));
        CHECK(has(*bad, "'mill' is not a dynamic entity type"));
    }

    // a data folder: `dynamic = yes` and `spawn = { ... }`; every device agrees
    {
        const auto g = load_game(std::string(TERNI_TEST_DATA) + "/dynamic_game");
        for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
        CHECK(g->ok() && g->warnings.empty());
        const EntityDef* army = g->entity("army");
        CHECK(army && army->dynamic && army->spawns.size() == 1 && army->spawns[0].from == "province" &&
              army->spawns[0].parent_field == "home" && !g->entity("province")->dynamic);
        if (g->ok()) {
            std::vector<StateSnapshot> states;
            for (const auto& d : devices) {
                GameSim sim(*g, d.index);
                for (int t = 0; t < 30; ++t) sim.tick();
                CHECK(sim.entity_generation(uint32_t(g->entity_index("army")), 0) > 1);  // rows were reused
                states.push_back(sim.snapshot());
                CHECK(first_difference(states[0], states.back()).empty());
            }
        }
        // the checksum covers it: the same world without `dynamic` is another game
        const uint64_t with = game_checksum(*w.game);
        WorldBuilder b;
        b.entity("province", PROVINCE_CAPACITY);
        CHECK(with != game_checksum(*b.finish()));
    }

    std::printf("test_dynamic: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
