// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Small fields (int8 ... ref16): a world with small fields and its twin with 32-bit ones run
// the same rules and must agree after every tick, on every device, down to the state hashes.
// Every device runs them packed and widened (as a device without 8 and 16-bit storage does).
// Then what only small fields have: values that do not fit, accesses past the last row,
// neighbours that share a word, packed readbacks, saves between widths, and what the
// builder, the loader and the order lint say about them.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "terni/game.hpp"
#include "terni/rng.hpp"
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

template <class E, class F>
bool throws(F&& f, const std::string& text = "") {
    try {
        f();
    } catch (const E& e) {
        if (std::string(e.what()).find(text) != std::string::npos) return true;
        std::printf("  unexpected message: %s\n", e.what());
    } catch (...) {
    }
    return false;
}

static bool has_error(const Game& g, const std::string& text) {
    for (const auto& e : g.errors)
        if (e.message.find(text) != std::string::npos) return true;
    std::printf("  no error '%s'; got:\n", text.c_str());
    for (const auto& e : g.errors) std::printf("    %s\n", e.text().c_str());
    return false;
}

// Compiled rules are shared between the simulations below.
static const char* const CACHE = "test_small_fields_cache";
// A device and how it stores small fields.
struct Target {
    uint32_t index;
    std::string name;
    bool can_pack, widen;
    bool widened() const { return widen || !can_pack; }
};
static GameSimOptions options(const Target& d) {
    GameSimOptions o;
    o.device = int(d.index);
    o.spirv_cache_dir = CACHE;
    o.widen_small_fields = d.widen;
    return o;
}

// ---- the twin worlds: the same rules over 32-bit and small fields ----

constexpr uint32_t REALMS = 7, REGIONS = 60, POPS = 5003, UNITS = 997;


// GET(entity_field, index) and SET(entity_field, index, value) are the accessors of the twin at hand.
static std::string rule(bool small, const std::string& body) {
    return (small ? std::string("#define GET(f, i) f(i)\n#define SET(f, i, v) f##_set(i, v)\n")
                  : std::string("#define GET(f, i) f[i]\n#define SET(f, i, v) f[i] = (v)\n")) +
           body;
}

static std::unique_ptr<Game> twin(bool small, bool narrow_mood = true) {
    auto t = [&](FieldType s, FieldType w) { return small ? s : w; };
    WorldBuilder b;
    const uint32_t realm = b.entity("realm", REALMS), region = b.entity("region", REGIONS), pop = b.entity("pop", POPS),
                   unit = b.entity("unit", UNITS);
    b.field(realm, "people", FieldType::Agg);
    b.field(region, "owner", t(FieldType::Ref8, FieldType::Ref), "realm");
    b.field(region, "people", FieldType::Agg);
    b.field(region, "raise", t(FieldType::Count8, FieldType::Count));
    b.field(region, "stock", t(FieldType::Count16, FieldType::Count), "", 5);
    b.field(region, "tally", t(FieldType::Count16, FieldType::Count));
    b.field(pop, "home", t(FieldType::Ref16, FieldType::Ref), "region");
    b.field(pop, "job", t(FieldType::Count8, FieldType::Count));
    b.field(pop, "mood", t(narrow_mood ? FieldType::Int8 : FieldType::Int16, FieldType::Int), "", 1, -3);
    b.field(pop, "years", t(FieldType::Int16, FieldType::Int));
    b.field(pop, "habits", t(FieldType::Int8, FieldType::Int), "", 3);
    b.field(pop, "wealth", FieldType::Quantity);
    b.entity_dynamic(unit);
    b.field(unit, "home", t(FieldType::Ref8, FieldType::Ref), "region");
    b.field(unit, "number", t(FieldType::Int8, FieldType::Int));
    b.field(unit, "age", t(FieldType::Count8, FieldType::Count), "", 1, 4);
    b.spawn(unit, "region", "raise", SpawnOptions{"home", "number"});
    const uint32_t zeal = b.modifier_key("zeal", pop, FieldType::Int, "home.owner");
    const uint32_t fervour = b.bundle("fervour", {{zeal, 2}});

    b.stage("pops", "pop", rule(small, R"(
void main() {
    FOR_EACH_POP(i) {
        const uint r = rng_roll(i, terni.tick, 1u);
        if ((r & 1u) != 0u) SET(pop_job, i, (GET(pop_job, i) * 3u + (r >> 8)) & 255u);
        if ((r & 6u) == 0u) {
            SET(pop_home, i, (GET(pop_home, i) + (r >> 16)) % REGION_CAPACITY);
        } else if ((r & 0xFFF0u) == 0u) {
            SET(pop_home, i, 0xFFFFFFFFu);  // no row
        } else if (GET(pop_home, i) == 0xFFFFFFFFu) {
            SET(pop_home, i, i % REGION_CAPACITY);
        }
        int mood = GET(pop_mood, i) + int(r >> 4 & 3u) - 1 - (pop_mod_zeal(i) & 1);
        SET(pop_mood, i, mood > 127 ? -128 : mood < -128 ? 127 : mood);
        SET(pop_years, i, GET(pop_years, i) > 32000 ? -32768 : GET(pop_years, i) + int(r & 255u));
        for (uint k = 0u; k < 3u; ++k) SET(pop_habits, i * 3u + k, (int(r >> (8u * k)) & 255) - 128);
        pop_wealth[i] += GET(pop_mood, i) * 7 + GET(pop_habits, i * 3u + 1u) + pop_mod_zeal(i);
    }
}
)"));
    // scattered: every row written by a thread far away from its own
    b.stage("shuffle", "pop", rule(small, R"(
void main() {
    FOR_EACH_POP(i) {
        const uint j = (i * 2609u + 11u) % POP_CAPACITY;
        if ((rng_roll(i, terni.tick, 2u) & 3u) == 0u) SET(pop_job, j, i & 255u);  // terni: order ok (5003 is prime)
    }
}
)"));
    b.stage("count", "pop", rule(small, R"(
void main() {
    FOR_EACH_POP(i) {
        const uint h = GET(pop_home, i);
        if (h >= REGION_CAPACITY) continue;
        atomicAdd(region_people[h], int(GET(pop_job, i)) + 1);
        const uint o = GET(region_owner, h);
        if (o < REALM_CAPACITY) atomicAdd(realm_people[o], 1);
    }
}
)"), {"region.people", "realm.people"});
    b.stage("regions", "region", rule(small, R"(
void main() {
    FOR_EACH_REGION(p) {
        const uint r = rng_roll(p, terni.tick, 3u);
        for (uint k = 0u; k < 5u; ++k)
            SET(region_stock, p * 5u + k, (GET(region_stock, p * 5u + k) + uint(region_people[p]) + k) & 65535u);
        SET(region_raise, p, (r & 7u) < 3u ? r >> 4 & 3u : 0u);
        SET(region_tally, p, GET(region_tally, p) + 5u);
        if ((r & 0xF00u) == 0u) SET(region_owner, p, (r >> 12) % REALM_CAPACITY);
    }
}
)"));
    b.stage("units", "unit", rule(small, R"(
void main() {
    FOR_EACH_UNIT(u) {
        SET(unit_age, u, GET(unit_age, u) + 1u);
        if (GET(unit_age, u) > 12u + (u & 7u) + uint(GET(unit_number, u))) unit_destroy(u);
    }
}
)"));
    b.clear({"region.tally"}, 3);

    b.create_rows(realm, REALMS);
    b.create_rows(region, REGIONS);
    b.create_rows(pop, POPS);
    for (uint32_t p = 0; p < REGIONS; ++p) {
        b.set(region, p, "owner", p == 9 ? int64_t(NO_ROW) : int64_t(p % REALMS));
        for (uint32_t k = 0; k < 5; ++k) b.set(region, p, "stock", 65535 - 17 * p - k, k);
    }
    std::vector<int32_t> home(POPS), job(POPS);
    for (uint32_t i = 0; i < POPS; ++i) home[i] = int32_t(i % REGIONS), job[i] = int32_t(i & 255);
    b.set_rows(pop, 0, "home", home);
    b.set_rows(pop, 0, "job", job);
    b.add_modifier(fervour, realm, 3);
    b.add_modifier(fervour, region, 5);
    auto g = b.finish();
    for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& w : g->warnings) std::printf("  warning: %s\n", w.text().c_str());
    CHECK(g->ok() && g->warnings.empty());
    return g;
}

// What the host does between ticks, the same to each twin.
static void host_events(GameSim& sim, int tick) {
    if (tick % 4 == 1) sim.write_field(2, "job", uint32_t(tick * 31), {255, 0, 17});
    if (tick % 5 == 2) sim.write_field(2, "habits", 77, 1, {-128, 127});
    if (tick % 6 == 3) sim.write_field(1, "owner", 11, {tick % 12 == 3 ? -1 : 2});  // -1: no row
    if (tick % 7 == 4) sim.create_entity(3);
    if (tick % 9 == 5) sim.destroy_entity(3, 1);
    if (tick == 8) sim.add_modifier(0, 0, 2, 3 << 16, 6);
}

static void twins(const std::vector<Target>& devices) {
    auto small = twin(true), wide = twin(false);
    CHECK(small->small_fields() && !wide->small_fields());
    CHECK(game_checksum(*small) != game_checksum(*wide));
    std::vector<std::vector<FieldHash>> first;
    std::vector<uint8_t> first_save;
    for (const auto& d : devices) {
        std::printf("device %u: %s, %s\n", d.index, d.name.c_str(), d.widened() ? "widened" : "packed");
        const GameSimOptions o = options(d);
        GameSim a(*small, o), w(*wide, o);
        CHECK(a.widened() == d.widened() && !w.widened());
        // moves from one form to the other on the way, with host writes waiting and results to keep
        GameSim v(*small, o);
        const uint32_t rv = v.read_each_tick(2, "mood"), hv = v.read_each_tick(2, "home", 3, 1001);
        const uint32_t ra = a.read_each_tick(2, "mood"), rw = w.read_each_tick(2, "mood");
        const uint32_t ha = a.read_each_tick(2, "home", 3, 1001), hw = w.read_each_tick(2, "home", 3, 1001);
        std::vector<std::vector<FieldHash>> hashes;
        CHECK(a.field_hashes() == w.field_hashes());
        std::span<const int32_t> before;
        std::vector<int32_t> before_copy;
        std::span<const uint8_t> bytes_before;
        std::vector<uint8_t> bytes_before_copy;
        for (int t = 0; t < 40; ++t) {
            host_events(a, t), host_events(w, t), host_events(v, t);
            a.begin_tick(), w.begin_tick();
            if (t % 5 == 2) {
                const auto mood = v.tick_result(rv);
                const std::vector<int32_t> last(mood.begin(), mood.end());
                const auto packed = v.tick_result_bytes(hv);
                const std::vector<uint8_t> last_bytes(packed.begin(), packed.end());
                v.move_to_device(d.index, !v.widened());
                CHECK(v.widened() == ((t / 5 % 2 == 0) != d.widened() || !d.can_pack));
                const auto after = v.tick_result(rv);
                CHECK(last.size() == POPS && std::equal(after.begin(), after.end(), last.begin(), last.end()));
                const auto after_bytes = v.tick_result_bytes(hv);
                CHECK(std::equal(after_bytes.begin(), after_bytes.end(), last_bytes.begin(), last_bytes.end()));
            }
            v.begin_tick();
            if (t % 5 == 4) v.move_to_device(d.index);  // collects the tick
            v.end_tick();
            // the last tick's results stay valid while this one runs, and until the next begins
            if (t) CHECK(std::equal(before.begin(), before.end(), before_copy.begin(), before_copy.end()));
            a.end_tick(), w.end_tick();
            if (t) CHECK(std::equal(before.begin(), before.end(), before_copy.begin(), before_copy.end()));
            const auto x = a.tick_result(ra), y = w.tick_result(rw);
            CHECK(std::equal(x.begin(), x.end(), y.begin(), y.end()));
            if (t) CHECK(std::equal(before.begin(), before.end(), before_copy.begin(), before_copy.end()));
            const auto p = a.tick_result(ha), q = w.tick_result(hw);
            CHECK(p.size() == 1001 && std::equal(p.begin(), p.end(), q.begin(), q.end()));
            const auto bytes = a.tick_result_bytes(ha);
            if (t) CHECK(std::equal(bytes_before.begin(), bytes_before.end(), bytes_before_copy.begin(), bytes_before_copy.end()));
            CHECK(bytes.size() == 2002);
            for (size_t i = 0; i < 1001 && bytes.size() == 2002; ++i) {
                const uint32_t v = uint32_t(bytes[2 * i]) | uint32_t(bytes[2 * i + 1]) << 8;
                if (v != (uint32_t(p[i]) & 0xFFFFu)) {
                    CHECK(!"packed readback differs");
                    break;
                }
            }
            CHECK(w.tick_result_bytes(hw).size() == 1001 * 4);
            before = x, before_copy.assign(x.begin(), x.end());
            bytes_before = bytes, bytes_before_copy.assign(bytes.begin(), bytes.end());
            hashes.push_back(a.field_hashes());
            CHECK(hashes.back() == w.field_hashes());
            CHECK(hashes.back() == v.field_hashes());
            CHECK(v.modifier_total(0, 2, 5) == w.modifier_total(0, 2, 5));
            const auto moved = v.tick_result(rv);
            CHECK(std::equal(moved.begin(), moved.end(), x.begin(), x.end()));
            const auto moved_bytes = v.tick_result_bytes(hv), home_bytes = a.tick_result_bytes(ha);
            CHECK(std::equal(moved_bytes.begin(), moved_bytes.end(), home_bytes.begin(), home_bytes.end()));
            CHECK(a.modifier_total(0, 2, 5) == w.modifier_total(0, 2, 5));
            CHECK(a.modifier_total(0, 1, 11) == w.modifier_total(0, 1, 11));
        }
        const StateSnapshot sa = a.snapshot(), sw = w.snapshot();
        CHECK(differences(sa, sw).empty());
        CHECK(field_hashes(sa) == a.field_hashes());  // the host's hashes of a snapshot are the device's
        CHECK(a.read_field(2, "mood", 100, 33) == w.read_field(2, "mood", 100, 33));
        CHECK(a.read_field(1, "stock", 7, 2) == w.read_field(1, "stock", 7, 2));
        CHECK(a.entities_alive(3) > 0 && a.entities_alive(3) == w.entities_alive(3));
        bool no_row = false;
        for (int32_t v : a.read_field(2, "home")) no_row |= v == -1;
        CHECK(no_row);  // the rules do make some
        if (first.empty()) first = hashes;
        else CHECK(hashes == first);

        const auto save = a.save();
        CHECK(v.save() == save);
        if (first_save.empty()) first_save = save;
        else CHECK(save == first_save);
        CHECK(save.size() + POPS * 12 < w.save().size());  // the columns are packed
        CHECK(a.tick_checked().empty());                   // neither order problems nor values that do not fit
        w.tick();
        CHECK(a.field_hashes() == w.field_hashes());
        GameSim c(*small, o);
        c.load(save);
        CHECK(c.save() == save);
        // what tick_result() made before a load must not come back after it
        const uint32_t rc = c.read_each_tick(2, "mood");
        c.tick();
        const auto at41 = c.tick_result(rc);
        const std::vector<int32_t> kept(at41.begin(), at41.end());
        c.load(save);
        c.write_field(2, "mood", 0, std::vector<int32_t>(POPS, 100));
        c.tick();
        const auto again = c.tick_result(rc);
        CHECK(!std::equal(again.begin(), again.end(), kept.begin(), kept.end()));
        CHECK(std::equal(again.begin(), again.end(), c.read_field(2, "mood").begin()) || again.size() != POPS);
        for (const auto& other : devices)
            if (other.index != d.index) {
                c.load(save);
                const auto h = c.field_hashes();
                c.move_to_device(other.index);
                CHECK(c.field_hashes() == h);
                break;
            }

        // a reload between widths: the twins load each other's saves by name
        GameSim n(*small, o);
        const auto notes = n.load(w.save(), SaveMatch::ByName);
        CHECK(n.field_hashes() == w.field_hashes());
        CHECK(std::find(notes.begin(), notes.end(), "field 'pop.job' was count and is count8 now") != notes.end());
        GameSim m(*wide, o);
        m.load(save, SaveMatch::ByName);
        GameSim s(*small, o);
        s.load(save);
        CHECK(m.field_hashes() == s.field_hashes());
        CHECK(differences(save_snapshot(save), save_snapshot(m.save())).empty());
        w.write_field(2, "job", 4321, {256});
        const auto too_wide = w.save();
        const auto untouched = n.field_hashes();
        CHECK(throws<std::runtime_error>([&] { n.load(too_wide, SaveMatch::ByName); }, "row 4321: 256 does not fit count8"));
        CHECK(n.field_hashes() == untouched);
    }
    // another width of one field
    auto other = twin(true, false);
    GameSim x(*other, devices[0].index);
    const auto notes = x.load(first_save, SaveMatch::ByName);
    CHECK(std::find(notes.begin(), notes.end(), "field 'pop.mood' was int8 and is int16 now") != notes.end());
}

// ---- values that do not fit, and accesses past the last row ----

struct Edge {
    FieldType type;
    uint32_t rows;
};

static void edges(const std::vector<Target>& devices) {
    for (const Edge edge : {Edge{FieldType::Count8, 5}, Edge{FieldType::Int8, 6}, Edge{FieldType::Ref8, 7}, Edge{FieldType::Count16, 3},
                            Edge{FieldType::Int16, 1}, Edge{FieldType::Ref16, 5}}) {
        const FieldType base = base_type(edge.type);
        const std::string type = base == FieldType::Int ? "int" : "uint";
        const uint32_t rows = edge.rows;
        WorldBuilder b;
        const uint32_t thing = b.entity("thing", rows), probe = b.entity("probe", 64);
        b.field(thing, "v", edge.type, base == FieldType::Ref ? "thing" : "");
        b.field(probe, "in", FieldType::Int);
        b.field(probe, "at", FieldType::Count);
        b.field(probe, "writes", FieldType::Int);
        b.field(probe, "got", FieldType::Int);
        b.stage("write", "probe", "void main() { FOR_EACH_PROBE(p) { if (probe_writes[p] != 0) thing_v_set(probe_at[p], " + type +
                                      "(probe_in[p])); } }  // terni: order ok");
        b.stage("read", "probe", "void main() { FOR_EACH_PROBE(p) { probe_got[p] = int(thing_v(probe_writes[p] != 0 ? probe_at[p] : p)); } }");
        b.create_rows(thing, rows);
        b.create_rows(probe, 64);
        auto g = b.finish();
        for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
        CHECK(g->ok());

        const int64_t values = int64_t(1) << field_bits(edge.type);
        const std::vector<int64_t> tries = {0, 1, values / 2 - 1, values / 2, values - 2, values - 1, values, values + 1, -1, -2,
                                            -(values / 2), -(values / 2) - 1, INT32_MAX, INT32_MIN};
        for (const auto& d : devices) {
            GameSim sim(*g, options(d));
            for (const int64_t in : tries) {
                // probe 0 writes the value to the last row, 1 to 8 write 9 past the end, where padding may be
                std::vector<int32_t> ins(64, 9), at(64);
                for (uint32_t p = 0; p < 64; ++p) at[p] = int32_t(p <= 8 ? rows - 1 + p : p == 9 ? 0xFFFFFFFFu : 1000000 + p);
                ins[0] = int32_t(uint32_t(in));
                sim.write_field(probe, "in", 0, ins);
                sim.write_field(probe, "at", 0, at);
                sim.write_field(probe, "writes", 0, std::vector<int32_t>(64, 1));
                const auto issues = sim.tick_checked();
                int64_t want = int64_t(int32_t(uint32_t(in)));
                if (base == FieldType::Int) want = std::clamp<int64_t>(want, -(values / 2), values / 2 - 1);
                else {
                    const int64_t unsigned_in = int64_t(uint32_t(in));
                    want = base == FieldType::Count ? std::min<int64_t>(unsigned_in, values - 1)
                           : unsigned_in >= values - 1 ? int64_t(-1) : unsigned_in;
                }
                const auto stored = sim.read_field(thing, "v", rows - 1, 1);
                if (int64_t(stored[0]) != want)
                    std::printf("  %s <- %lld: %d, not %lld\n", type_name(edge.type), (long long)in, stored[0], (long long)want);
                CHECK(int64_t(stored[0]) == want);
                auto got = sim.read_field(probe, "got");
                CHECK(int64_t(got[0]) == want);
                bool past_end_zero = true;
                for (uint32_t p = 1; p < 64; ++p) past_end_zero &= got[p] == 0;
                CHECK(past_end_zero);  // nothing written there either
                const bool misfit = !fits_word(edge.type, int32_t(uint32_t(in)));
                CHECK(issues.size() == (misfit ? 1u : 0u));
                if (misfit && !issues.empty()) {
                    CHECK(issues[0].misfit && issues[0].stage == "write" && issues[0].field == "thing.v");
                    CHECK(issues[0].first_row == rows - 1 && issues[0].rows == 1);
                }
                sim.write_field(probe, "writes", 0, std::vector<int32_t>(64, 0));
                CHECK(sim.tick_checked().empty());  // the counts start anew in every tick
                got = sim.read_field(probe, "got");
                for (uint32_t p = rows; p < 64; ++p) past_end_zero &= got[p] == 0;
                CHECK(past_end_zero);
            }
            // what a save holds after all that: no trace of the writes past the end
            const auto snap = sim.snapshot();
            for (const auto& [name, column] : snap.fields)
                if (name == "thing.v") CHECK(column.size() == rows);
        }
    }
}

// Neighbours: each row written by its own thread, the next rows by others, against the CPU.
static void neighbours(const std::vector<Target>& devices) {
    constexpr uint32_t N = 100003;
    WorldBuilder b;
    const uint32_t cell = b.entity("cell", N);
    b.field(cell, "a", FieldType::Count8);
    b.field(cell, "b", FieldType::Int16);
    b.stage("own", "cell", R"(
void main() {
    FOR_EACH_CELL(i) {
        const uint r = rng_roll(i, terni.tick, 1u);
        if ((r & 1u) != 0u) cell_a_set(i, r >> 8 & 255u);
        if ((r & 2u) != 0u) cell_b_set(i, int(r >> 16) - 32768);
    }
}
)");
    b.create_rows(cell, N);
    auto g = b.finish();
    CHECK(g->ok() && g->warnings.empty());
    std::vector<int32_t> a(N), c(N);
    for (uint32_t t = 0; t < 60; ++t)
        for (uint32_t i = 0; i < N; ++i) {
            const uint32_t r = rng::roll(i, t, 1);
            if (r & 1) a[i] = int32_t(r >> 8 & 255);
            if (r & 2) c[i] = int32_t(r >> 16) - 32768;
        }
    for (const auto& d : devices) {
        GameSim sim(*g, options(d));
        for (int t = 0; t < 60; ++t) sim.tick();
        CHECK(sim.read_field(cell, "a") == a);
        CHECK(sim.read_field(cell, "b") == c);
    }
}

// An order problem in a small field is reported with its rows, however the field is stored.
static void order_problems(const std::vector<Target>& devices) {
    WorldBuilder b;
    const uint32_t cell = b.entity("cell", 40);
    b.field(cell, "a", FieldType::Int16, "", 2);
    b.stage("shift", "cell", R"(
void main() {
    FOR_EACH_CELL(i) {
        if (i >= 10u && i < 39u) cell_a_set(i * 2u, cell_a(i * 2u + 2u));  // terni: order ok (wrong on purpose)
    }
}
)");
    b.create_rows(cell, 40);
    for (uint32_t i = 0; i < 40; ++i) b.set(cell, i, "a", int64_t(i) * 100);
    auto g = b.finish();
    CHECK(g->ok());
    for (const auto& d : devices) {
        GameSim sim(*g, options(d));
        const auto issues = sim.tick_checked();
        const auto it = std::find_if(issues.begin(), issues.end(), [](const OrderIssue& i) { return !i.parallel && !i.misfit; });
        CHECK(it != issues.end());
        if (it == issues.end()) continue;
        // forwards row i gets its neighbour's old value, backwards that of row 39; row 38 gets it either way
        CHECK(it->field == "cell.a" && it->first_row == 10 && it->rows == 28);
    }
}

// The shader in a cache file.
static std::vector<uint32_t> spirv(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() > 24 + 20);
    std::vector<uint32_t> words(bytes.size() < 24 ? 0 : (bytes.size() - 24) / 4);  // after the file's header
    if (!words.empty()) std::memcpy(words.data(), bytes.data() + 24, words.size() * 4);
    return words;
}

// Constants of every type stored through the setters: no 8 or 16-bit constant may end up in
// the SPIR-V, which needs features Terni does not ask for (CI runs this under the validation
// layer, and the capabilities are checked here).
static void constants(const std::vector<Target>& devices) {
    WorldBuilder b;
    const uint32_t cell = b.entity("cell", 300), other = b.entity("other", 100);
    const std::vector<std::pair<FieldType, int32_t>> fields = {{FieldType::Int8, -7},    {FieldType::Int16, -700},
                                                               {FieldType::Count8, 200}, {FieldType::Count16, 60000},
                                                               {FieldType::Ref8, -1},    {FieldType::Ref16, -1}};
    std::string body;
    for (const auto& [t, v] : fields) {
        b.field(cell, type_name(t), t, base_type(t) == FieldType::Ref ? "other" : "");
        const std::string value = base_type(t) == FieldType::Int ? std::to_string(v) : std::to_string(uint32_t(v)) + "u";
        const std::string set = std::string("cell_") + type_name(t) + "_set(i, ";
        body += set + value + "); if ((i & 1u) != 0u) " + set + "0" + (base_type(t) == FieldType::Int ? "" : "u") + "); else if (i == 298u) " +
                set + "(i & 4u) != 0u ? " + value + " : 1" + (base_type(t) == FieldType::Int ? "" : "u") + ");\n";
    }
    b.stage("rule", "cell", "void main() { FOR_EACH_CELL(i) {\n" + body + "} }");
    b.create_rows(cell, 300);
    b.create_rows(other, 1);
    auto g = b.finish();
    for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
    CHECK(g->ok());
    for (const auto& d : devices) {
        GameSim sim(*g, options(d));
        CHECK(sim.tick_checked().empty());
        for (const auto& [t, v] : fields) {
            const auto got = sim.read_field(cell, type_name(t));
            CHECK(got[0] == v && got[1] == 0 && got[2] == v && got[299] == 0 && got[298] == 1);
        }
    }
    // every shader compiled so far, the built-in ones too
    size_t files = 0;
    for (const auto& file : std::filesystem::directory_iterator(CACHE)) {
        if (!file.is_regular_file()) continue;
        const std::vector<uint32_t> words = spirv(file.path());
        std::vector<uint32_t> narrow;  // the ids of the 8 and 16-bit types
        for (size_t at = 5; at < words.size() && (words[at] >> 16) != 0; at += words[at] >> 16) {
            const uint32_t op = words[at] & 0xFFFFu;
            if (op == 17) CHECK(words[at + 1] != 39 && words[at + 1] != 22);  // OpCapability Int8, Int16
            if (op == 21 && words[at + 2] < 32) narrow.push_back(words[at + 1]);  // OpTypeInt
            const bool constant = op == 43 || op == 46 || op == 50 || op == 52;   // OpConstant, Null, SpecConstant, SpecConstantOp
            if (constant) CHECK(std::find(narrow.begin(), narrow.end(), words[at + 1]) == narrow.end());
        }
        ++files;
    }
    CHECK(files > 5);

    // widened: nothing that needs 8 or 16-bit storage
    const std::string only = std::string(CACHE) + "/widened";
    GameSimOptions o = options(devices[0]);
    o.spirv_cache_dir = only;
    o.widen_small_fields = true;
    GameSim sim(*g, o);
    files = 0;
    for (const auto& file : std::filesystem::directory_iterator(only)) {
        const std::vector<uint32_t> words = spirv(file.path());
        for (size_t at = 5; at < words.size() && (words[at] >> 16) != 0; at += words[at] >> 16) {
            const uint32_t op = words[at] & 0xFFFFu;
            if (op == 17) CHECK(words[at + 1] != 4433 && words[at + 1] != 4448);  // StorageBuffer16BitAccess, ...8BitAccess
            if (op == 21) CHECK(words[at + 2] == 32);
        }
        ++files;
    }
    CHECK(files == 3);  // the rule, terni_tick, terni_hash

    // compile_rules() gives both, so a cache-only start works on either kind of device
    const std::string both = std::string(CACHE) + "/both";
    CHECK(compile_rules(*g, both).compiled == 6);
    o.spirv_cache_dir = both;
    o.cache_only = true;
    GameSim shipped(*g, o);
    CHECK(shipped.compile_stats().compiled == 0);
    shipped.move_to_device(devices[0].index, false);
    CHECK(shipped.widened() == !devices[0].can_pack);
    // a move that lacks its SPIR-V changes nothing
    o.spirv_cache_dir = only;
    GameSim stuck(*g, o);
    stuck.tick();
    const auto before = stuck.field_hashes();
    if (devices[0].can_pack) {
        CHECK(throws<std::runtime_error>([&] { stuck.move_to_device(devices[0].index, false); }, "not in the SPIR-V cache"));
        CHECK(stuck.widened() && stuck.field_hashes() == before);
        stuck.move_to_device(devices[0].index);
        CHECK(stuck.widened());
    }
}

// ---- the host's side: refused values, the builder, the loader, the order lint ----

static void host_side(uint32_t device) {
    for (const FieldType t : {FieldType::Int8, FieldType::Int16, FieldType::Count8, FieldType::Count16, FieldType::Ref8, FieldType::Ref16})
        CHECK(is_small(t) && !is_small(base_type(t)));
    CHECK(fits(FieldType::Int8, -128) && !fits(FieldType::Int8, 128) && fits(FieldType::Count16, 65535) && !fits(FieldType::Count16, -1));
    CHECK(fits(FieldType::Ref8, 254) && !fits(FieldType::Ref8, 255) && fits(FieldType::Ref8, -1) && fits(FieldType::Ref8, NO_ROW));
    int64_t v = 0;
    CHECK(parse_value("-128", FieldType::Int8, v) && v == -128 && !parse_value("-129", FieldType::Int8, v));
    CHECK(parse_value("255", FieldType::Count8, v) && !parse_value("256", FieldType::Count8, v));
    CHECK(!parse_value("1.5", FieldType::Count8, v) && parse_value("yes", FieldType::Count8, v) && v == 1);
    CHECK(!parse_value("3", FieldType::Ref16, v));

    auto g = twin(true);
    GameSim sim(*g, device);
    const auto before = sim.field_hashes();
    CHECK(throws<std::out_of_range>([&] { sim.write_field(2, "job", 5, {1, 256}); }, "'pop.job', row 6: 256 does not fit count8 (0 to 255)"));
    CHECK(throws<std::out_of_range>([&] { sim.write_field(2, "habits", 5, 2, {-129}); }, "row 5: -129 does not fit int8"));
    CHECK(throws<std::out_of_range>([&] { sim.write_field(3, "home", 0, {255}); }, "rows 0 to 254, or none"));
    sim.write_field(3, "home", 0, {-1});
    CHECK(sim.save().size() > 0);
    CHECK(sim.read_field(3, "home", 0, 1) == std::vector<int32_t>{-1});
    CHECK(sim.field_hashes() != before);

    CHECK(throws<std::out_of_range>([&] { g->set_capacity(0, 256); }, "at most 255 rows of 'realm'"));
    g->set_capacity(0, 255);
    CHECK(throws<std::out_of_range>([&] { g->set_capacity(1, 256); }));  // unit.home is a ref8
    g->tables[2].columns[1][3] = 999;                                   // pop.job, past the builder's checks
    CHECK(throws<std::out_of_range>([&] { GameSim bad(*g, device); }, "'pop.job', row 3: 999 does not fit"));

    WorldBuilder b;
    const uint32_t town = b.entity("town", 300), road = b.entity("road", 70000);
    b.field(town, "size", FieldType::Count8, "", 1, 256);
    b.field(town, "level", FieldType::Int8);
    b.field(town, "level_set", FieldType::Int);
    b.field(town, "next", FieldType::Ref8, "town");
    b.field(road, "next", FieldType::Ref16, "road");
    b.define("rate", FieldType::Int16);
    b.create_rows(town, 2);
    b.set(town, 0, "level", 200);
    const std::vector<int32_t> levels{1, -200};
    b.set_rows(town, 0, "level", levels);
    auto bad = b.finish();
    CHECK(has_error(*bad, "default 256 does not fit count8 (0 to 255)"));
    CHECK(has_error(*bad, "rules already have town_level_set() for the small field 'level'"));
    CHECK(has_error(*bad, "a ref8 can refer to at most 255 rows, and 'town' has room for 300"));
    CHECK(has_error(*bad, "a ref16 can refer to at most 65535 rows, and 'road' has room for 70000"));
    CHECK(has_error(*bad, "defines can't be small fields: use int"));
    CHECK(has_error(*bad, "field 'level': 200 does not fit int8 (-128 to 127)"));
    CHECK(has_error(*bad, "field 'level', row 1: -200 does not fit int8"));

    // the order lint reads the accessor functions like array accesses
    auto warnings = [](const std::string& body) {
        WorldBuilder w;
        w.entity("cell", 8);
        w.field(0, "a", FieldType::Count8);
        w.field(0, "s", FieldType::Int16, "", 4);
        w.stage("rule", "cell", "void main() { FOR_EACH_CELL(i) { " + body + " } }");
        return w.finish()->warnings.size();
    };
    CHECK(warnings("cell_a_set(i, cell_a(i) + 1u);") == 0);
    CHECK(warnings("uint base = i * 4u; cell_s_set(base + 1u, cell_s(i * 4u + 2u));") == 0);
    CHECK(warnings("cell_a_set(i, cell_a(i + 1u));") == 1);
    CHECK(warnings("cell_a_set((i + 1u) % 8u, 3u);") == 1);
    CHECK(warnings("cell_a_set(min(i, 3u), 3u);") == 1);
    CHECK(warnings("cell_s_set(i * 4u, int(cell_a(i ^ 1u)));") == 0);  // reads another field's other row

    // no atomics, no raw access: a compile error rather than a wrong value
    for (const std::string body : {"atomicAdd(cell_a(i), 1u);", "cell_a[i] = 1u;"}) {
        WorldBuilder w;
        w.entity("cell", 8);
        w.field(0, "a", FieldType::Count8);
        w.stage("rule", "cell", "void main() { FOR_EACH_CELL(i) { " + body + " } }");
        auto game = w.finish();
        CHECK(game->ok());
        CHECK(throws<std::runtime_error>([&] { GameSim s(*game, device); }, "failed to compile"));
    }
}

static void data_folder(uint32_t device) {
    auto g = load_game(TERNI_SOURCE_DIR "/tests/data/small_fields");
    for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
    CHECK(g->ok() && g->warnings.empty());
    if (!g->ok()) return;
    const EntityDef& town = *g->entity("town");
    CHECK(town.field("owner")->type == FieldType::Ref8 && town.field("owner")->ref_entity == "realm");
    CHECK(town.field("level")->type == FieldType::Count8 && town.field("level")->default_raw == 3);
    CHECK(town.field("stock")->type == FieldType::Int16 && town.field("stock")->size == 2);
    CHECK(town.field("mood")->type == FieldType::Int8 && town.field("people")->type == FieldType::Count16);
    CHECK(town.field("twin")->type == FieldType::Ref16);
    const uint32_t t = uint32_t(g->entity_index("town"));
    GameSim sim(*g, device);
    for (int k = 0; k < 3; ++k) sim.tick();
    CHECK(sim.read_field(t, "level", 0, 3) == (std::vector<int32_t>{7, 6, 3}));
    CHECK(sim.read_field(t, "stock", 0, 2) == (std::vector<int32_t>{-30000, 12, 0, 0}));
    CHECK(sim.read_field(t, "owner", 0, 2) == (std::vector<int32_t>{1, 0}));
    CHECK(sim.read_field(t, "mood", 0, 2) == (std::vector<int32_t>{-5, 0}));

    auto bad = load_game(TERNI_SOURCE_DIR "/tests/data/small_fields_bad");
    CHECK(has_error(*bad, "unknown field type 'ref32:realm'"));
    CHECK(has_error(*bad, "unknown field type 'count4'"));
    CHECK(has_error(*bad, "invalid default '300' for count8"));
    CHECK(has_error(*bad, "invalid value '70000' for field 'people': 70000 does not fit count16 (0 to 65535)"));
}

int main() {
    std::vector<Target> devices;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        if (d.small_fields) devices.push_back({d.index, d.name, true, false});
        devices.push_back({d.index, d.name, d.small_fields, true});
    }
    CHECK(!devices.empty());
    if (devices.empty()) return 1;
    std::filesystem::remove_all(CACHE);
    host_side(devices[0].index);
    data_folder(devices[0].index);
    neighbours(devices);
    edges(devices);
    order_problems(devices);
    twins(devices);
    constants(devices);
    std::filesystem::remove_all(CACHE);
    std::printf(failures ? "%d FAILURES\n" : "all small field checks passed\n", failures);
    return failures ? 1 : 0;
}
