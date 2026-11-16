// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Entities at run time: provinces ask for armies, armies wear out and destroy themselves,
// the host creates and destroys some too. Every device is compared, tick by tick, with a
// model of the same world on the CPU: which rows are alive, which row each request got
// (the lowest free one; a destroyed row rests for a tick), generations, fields, counts and
// the modifiers of destroyed rows.
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
    // prev: alive when the last tick ended, or created by the host since. cool: died in the last tick.
    std::vector<bool> prev = alive, cool = alive;
    std::vector<uint32_t> generation = std::vector<uint32_t>(ARMY_CAPACITY, 0);
    std::vector<int32_t> home = std::vector<int32_t>(ARMY_CAPACITY, 0), strength = home, fresh = std::vector<int32_t>(ARMY_CAPACITY, 1),
                         age = home, seen_generation = home;
    uint32_t count = 0;
    bool was_full = false, reused = false;  // what the session must have gone through
    // What read_field() gives: the fields as the last tick left them, the rows it created
    // included. What the host wrote since reaches the device with the next tick.
    struct Fields {
        std::vector<uint32_t> raise;
        std::vector<int32_t> armies, home, strength, fresh, age, seen_generation;
    } device;
    void sync() { device = {raise, armies, home, strength, fresh, age, seen_generation}; }

    Model() {
        for (uint32_t a = 0; a < ARMIES; ++a) alive[a] = prev[a] = true, generation[a] = 1, home[a] = int32_t(a + 1);
        count = ARMIES;
        sync();
    }
    uint32_t alive_count() const {
        uint32_t n = 0;
        for (bool a : alive) n += a;
        return n;
    }
    void born(uint32_t a) {  // every field at its default
        reused |= generation[a] > 0;
        alive[a] = prev[a] = true;
        ++generation[a];
        home[a] = 0, strength[a] = 0, fresh[a] = 1, age[a] = 0, seen_generation[a] = 0;
        count = std::max(count, a + 1);
    }
    uint32_t create() {  // the host: the lowest row that is free between ticks
        for (uint32_t a = 0; a < ARMY_CAPACITY; ++a)
            if (!alive[a] && !prev[a] && !cool[a]) {
                born(a);
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
        // the end of the tick: the rows the provinces asked for, in province order, each the
        // lowest row that is dead now and was dead when the last tick ended
        std::vector<uint32_t> eligible;
        for (uint32_t a = 0; a < ARMY_CAPACITY; ++a) {
            if (!alive[a] && !prev[a]) eligible.push_back(a);
            cool[a] = prev[a] && !alive[a];
            prev[a] = alive[a];
        }
        size_t next = 0;
        for (uint32_t p = 0; p < PROVINCES; ++p)
            for (; raise[p] > 0; --raise[p]) {
                if (next == eligible.size()) {
                    was_full = true;
                    break;
                }
                born(eligible[next]);
                home[eligible[next++]] = int32_t(p);
            }
        sync();
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
            m.sync();  // a save holds the host's writes that were waiting for the next tick
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

// ---- a crowd: thousands of rows asking at once ------------------------------------
// Pops split (a dynamic entity type asking for rows of its own kind) and provinces settle
// new ones, 180,000 pops in 200,003 rows: requests and free rows span many blocks of the
// spawn stages, there are more requests than rows in the first ticks, and the capacity
// is no multiple of anything.
constexpr uint32_t CROWD_PROVINCES = 700, POPS = 180000, POP_CAPACITY = 200003;

const char* SETTLE = R"(
void main() {
    FOR_EACH_PROVINCE(p) {
        if ((p * 7u + terni.tick) % 11u == 0u) province_settle[p] += 1u + (p & 3u);
    }
}
)";
const char* LIVE = R"(
void main() {
    FOR_EACH_POP(i) {
        pop_age[i] += 1;
        uint h = (i * 2654435761u + terni.tick * 40503u) >> 20;
        if (h < 300u) pop_split[i] += 1u + (h & 1u);
        else if (h > 3600u || pop_age[i] > 30) pop_destroy(i);
    }
}
)";

struct Crowd {
    std::unique_ptr<Game> game;
    uint32_t province = 0, pop = 0;
};
static Crowd build_crowd() {
    Crowd w;
    WorldBuilder b;
    w.province = b.entity("province", 1024);
    b.field(w.province, "settle", FieldType::Count);
    w.pop = b.entity("pop", POP_CAPACITY);
    b.entity_dynamic(w.pop);
    b.field(w.pop, "home", FieldType::Ref, "province");
    b.field(w.pop, "mother", FieldType::Ref, "pop");
    b.field(w.pop, "age", FieldType::Int);
    b.field(w.pop, "split", FieldType::Count);
    b.spawn(w.pop, "province", "settle", "home");
    b.spawn(w.pop, "pop", "split", "mother");
    b.stage("settle", "province", SETTLE);
    b.stage("live", "pop", LIVE);
    for (uint32_t p = 0; p < CROWD_PROVINCES; ++p) b.create(w.province);
    for (uint32_t i = 0; i < POPS; ++i) b.set(w.pop, b.create(w.pop), "age", int64_t(i % 29));
    w.game = b.finish();
    for (const auto& e : w.game->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& x : w.game->warnings) std::printf("  warning: %s\n", x.text().c_str());
    return w;
}

struct CrowdModel {
    uint32_t tick = 0, count = POPS;
    std::vector<uint32_t> settle = std::vector<uint32_t>(1024, 0);
    std::vector<bool> alive = std::vector<bool>(POP_CAPACITY, false), prev = alive;
    std::vector<uint32_t> generation = std::vector<uint32_t>(POP_CAPACITY, 0), split = generation;
    std::vector<int32_t> home = std::vector<int32_t>(POP_CAPACITY, 0), mother = home, age = home;
    uint64_t born = 0, turned_away = 0;

    CrowdModel() {
        for (uint32_t i = 0; i < POPS; ++i) alive[i] = prev[i] = true, generation[i] = 1, age[i] = int32_t(i % 29);
    }
    void step() {
        for (uint32_t p = 0; p < CROWD_PROVINCES; ++p)
            if ((p * 7u + tick) % 11u == 0u) settle[p] += 1u + (p & 3u);
        for (uint32_t i = 0; i < count; ++i) {
            if (!alive[i]) continue;
            age[i] += 1;
            const uint32_t h = (i * 2654435761u + tick * 40503u) >> 20;
            if (h < 300u) split[i] += 1u + (h & 1u);
            else if (h > 3600u || age[i] > 30) alive[i] = false;
        }
        ++tick;
        std::vector<uint32_t> eligible;
        for (uint32_t i = 0; i < POP_CAPACITY; ++i) {
            if (!alive[i] && !prev[i]) eligible.push_back(i);
            prev[i] = alive[i];
        }
        const std::vector<bool> asking = alive;  // rows created below do not ask in the tick they are created
        const uint32_t asking_count = count;
        size_t next = 0;
        auto serve = [&](uint32_t& want, std::vector<int32_t>& parent, uint32_t by) {
            for (; want > 0; --want) {
                if (next == eligible.size()) {
                    turned_away += want;
                    return;
                }
                const uint32_t row = eligible[next++];
                alive[row] = prev[row] = true;
                ++generation[row];
                home[row] = mother[row] = age[row] = 0;
                split[row] = 0;
                parent[row] = int32_t(by);
                count = std::max(count, row + 1);
                ++born;
            }
        };
        for (uint32_t p = 0; p < CROWD_PROVINCES; ++p) serve(settle[p], home, p);
        for (uint32_t i = 0; i < asking_count; ++i)
            if (asking[i]) serve(split[i], mother, i);
    }
};

static void test_crowd(const std::vector<DeviceInfo>& devices) {
    const Crowd w = build_crowd();
    CHECK(w.game->ok() && w.game->warnings.empty());
    if (!w.game->ok()) return;
    std::vector<StateSnapshot> ends;
    for (const auto& d : devices) {
        GameSim sim(*w.game, d.index);
        CrowdModel m;
        bool ok = true;
        const uint64_t reads = sim.sync_reads();
        for (int t = 0; t < 30; ++t) {
            sim.tick();
            m.step();
            bool same = sim.entity_count(w.pop) == m.count;
            uint32_t alive = 0;
            for (uint32_t i = 0; i < POP_CAPACITY; ++i) {
                same &= sim.entity_alive(w.pop, i) == m.alive[i] && sim.entity_generation(w.pop, i) == m.generation[i];
                alive += m.alive[i];
            }
            same &= sim.entities_alive(w.pop) == alive;
            ok &= same;
            if (!same) std::printf("  tick %d: rows differ from the model\n", t);
        }
        CHECK(sim.sync_reads() == reads);  // the host followed all of it without one read that waits
        ok &= sim.read_field(w.pop, "home") == m.home && sim.read_field(w.pop, "mother") == m.mother;
        ok &= sim.read_field(w.pop, "age") == m.age;
        ok &= sim.read_field(w.pop, "split") == std::vector<int32_t>(m.split.begin(), m.split.end());
        ok &= sim.read_field(w.province, "settle") == std::vector<int32_t>(m.settle.begin(), m.settle.end());
        ok &= sim.read_field(w.pop, "home") == m.home;
        CHECK(ok);
        CHECK(m.born > 200000 && m.turned_away > 0 && m.count > POPS);
        ends.push_back(sim.snapshot());
        const bool agree = first_difference(ends[0], ends.back()).empty();
        CHECK(agree);
        std::printf("a crowd of pops on %-45.45s %s (%llu born)\n", d.name.c_str(), ok && agree ? "ok" : "DIFFERS",
                    (unsigned long long)m.born);
    }
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

    test_crowd(devices);

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
        b.entity_dynamic(b.entity("crowd", 1u << 24));  // more rows than the spawn stages count
        const auto bad = b.finish();
        CHECK(has(*bad, "field 'alive' of dynamic entity 'cart'"));
        CHECK(has(*bad, "'want' is not a single count field of 'town'"));
        CHECK(has(*bad, "'carts' is not a single count field of 'town'"));
        CHECK(has(*bad, "unknown entity 'village'"));
        CHECK(has(*bad, "'missing' is not a single count field of 'mill'"));
        CHECK(has(*bad, "'owner' is not a single ref:town field of 'cart'"));
        CHECK(has(*bad, "'mill' is not a dynamic entity type"));
        CHECK(has(*bad, "dynamic entity 'crowd': capacity must be under 16,777,216"));
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
