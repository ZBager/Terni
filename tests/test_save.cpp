// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Save / load: replay equivalence across devices, byte-identical round trips,
// surviving modifier handles, and rejection of foreign / corrupt saves. A patched game
// loads an old save by name (SaveMatch::ByName): fields, entity types and bundles that
// were added, removed or resized, on every device alike; what cannot be fitted is
// refused. Saves of format 1 still load into their own game.
#include <cstdio>
#include <functional>
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

static uint64_t state_hash(GameSim& sim) { return combine(hash_fields(sim.snapshot())); }

// ---- a world and versions of it, as a game's patches would change it
// 0: the original. 1: the patch: town capacity 8 -> 12, town.stock 2 -> 3 values,
//    town.fertility new (history values), town.old gone, cart capacity 16 -> 32, cart.fresh
//    new, entity type road new, relic gone, bundle doomed gone, bundles reordered, the grow
//    rule changed; glow now raises growth, but its instance sat on a relic. 2: as 1, but
//    town.pop became a quantity. 3: as 1, but carts fit only 2.
//    4: as 1, but carts are not dynamic.
static std::unique_ptr<Game> town_world(int version) {
    const bool patched = version > 0;
    WorldBuilder b;
    const uint32_t town = b.entity("town", patched ? 12 : 8);
    b.field(town, "pop", version == 2 ? FieldType::Quantity : FieldType::Int);
    b.field(town, "stock", FieldType::Quantity, "", patched ? 3 : 2, fx::qty(9));
    if (patched) b.field(town, "fertility", FieldType::Int);
    else b.field(town, "old", FieldType::Int);
    const uint32_t cart = b.entity("cart", version == 3 ? 2 : patched ? 32 : 16);
    if (version != 4) b.entity_dynamic(cart);
    b.field(cart, "load", FieldType::Int);
    if (patched) b.field(cart, "fresh", FieldType::Int, "", 1, 7);
    uint32_t relic = 0, road = 0;
    if (patched) {
        road = b.entity("road", 4);
        b.field(road, "length", FieldType::Int);
    } else {
        relic = b.entity("relic", 2);
        b.field(relic, "age", FieldType::Int);
    }
    const uint32_t growth = b.modifier_key("growth", town, FieldType::Rate);
    if (patched) {
        b.bundle("festival", {{growth, fx::rate(1, 50)}});
        b.bundle("boom", {{growth, fx::rate(1, 10)}});
        b.bundle("glow", {{growth, fx::rate(1, 4)}});  // kept, but its instance sat on a relic
    } else {
        b.bundle("boom", {{growth, fx::rate(1, 10)}});
        b.bundle("doomed", {{growth, fx::rate(-1, 20)}});
        const uint32_t shine = b.modifier_key("shine", relic, FieldType::Rate);
        b.bundle("glow", {{shine, fx::rate(1, 4)}});
    }
    b.stage("grow", "town", patched ? "void main() { FOR_EACH_TOWN(t) { town_pop[t] += 2; } }"
                                    : "void main() { FOR_EACH_TOWN(t) { town_pop[t] += 1; } }");
    b.stage("haul", "cart", "void main() { FOR_EACH_CART(c) { cart_load[c] += 2; } }");
    for (uint32_t t = 0; t < 5; ++t) {
        b.set(town, b.create(town), "pop", 100 + t);
        b.set(town, t, "stock", fx::qty(int32_t(t)), 1);
        if (patched) b.set(town, t, "fertility", 10 * t);
    }
    for (uint32_t c = 0; c < (version == 3 ? 2u : 3u); ++c) b.set(cart, b.create(cart), "load", 50 + c);
    if (patched) {
        for (uint32_t r = 0; r < 2; ++r) b.set(road, b.create(road), "length", 3 + r);
    } else {
        b.create(relic);
    }
    auto g = b.finish();
    for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
    return g;
}

static bool has_note(const std::vector<std::string>& notes, const std::string& text) {
    for (const auto& n : notes)
        if (n.find(text) != std::string::npos) return true;
    std::printf("  no note with '%s'\n", text.c_str());
    return false;
}

// The same save in format 1, as Terni wrote it before format 2: tick and counts, then the
// columns by name alone, then the modifier store.
static std::vector<uint8_t> as_format_1(const std::vector<uint8_t>& v2) {
    size_t at = 8 + 4;
    auto u8 = [&] { return v2[at++]; };
    auto u32 = [&] {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(v2[at++]) << (8 * i);
        return v;
    };
    auto bytes = [&](size_t n) {
        std::vector<uint8_t> b(v2.begin() + std::ptrdiff_t(at), v2.begin() + std::ptrdiff_t(at + n));
        at += n;
        return b;
    };
    std::vector<uint8_t> out(v2.begin(), v2.begin() + 8);
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    auto put = [&](const std::vector<uint8_t>& b) { out.insert(out.end(), b.begin(), b.end()); };
    put32(1);
    put(bytes(16));  // checksum, completed ticks
    std::vector<uint32_t> state{u32()};
    const uint32_t entities = u32();
    for (uint32_t e = 0; e < entities; ++e) {
        bytes(u32());  // name
        u32(), u8();   // capacity, dynamic
        state.push_back(u32());
    }
    put32(uint32_t(state.size()));
    for (uint32_t v : state) put32(v);
    const uint32_t columns = u32();
    put32(columns);
    for (uint32_t c = 0; c < columns; ++c) {
        const uint32_t name = u32();
        put32(name);
        put(bytes(name));
        u8(), u32(), bytes(u32());  // type, size, ref
        const uint32_t words = u32();
        put32(words);
        put(bytes(size_t(words) * 4));
    }
    for (uint32_t b = 0, n = u32(); b < n; ++b) bytes(u32());  // bundle names
    put(bytes(v2.size() - 8 - at));  // the modifier store
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t c : out) h = (h ^ c) * 0x100000001b3ull;
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(h >> (8 * i)));
    return out;
}

static bool throws_with(const std::function<void()>& f, const std::string& text) {
    try {
        f();
    } catch (const std::exception& e) {
        if (std::string(e.what()).find(text) != std::string::npos) return true;
        std::printf("  unexpected message: %s\n", e.what());
        return false;
    }
    std::printf("  no exception (expected '%s')\n", text.c_str());
    return false;
}

int main() {
    auto game = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    for (const auto& e : game->errors) std::printf("  %s\n", e.text().c_str());
    if (!game->ok()) return 1;
    const uint32_t empire = uint32_t(game->entity_index("empire")), planet = uint32_t(game->entity_index("planet"));
    const uint32_t war = uint32_t(game->bundle_index("war_economy"));

    std::vector<uint32_t> devices;
    std::vector<std::string> names;
    for (const auto& d : VulkanContext::list_devices())
        if (d.usable) devices.push_back(d.index), names.push_back(d.name);
    CHECK(!devices.empty());

    // ---- reference run on the first device: 10 ticks, host changes, save, 10 more ticks
    GameSim a(*game, devices[0]);
    for (int t = 0; t < 10; ++t) a.tick();
    const ModifierHandle h = a.add_modifier(war, empire, 0, fx::rate(3, 2), 15);  // expires after the save point
    a.write_field(planet, "pop", 3, {fx::qty(3333)});                           // queued, not yet applied
    const std::vector<uint8_t> save = a.save();
    for (int t = 0; t < 10; ++t) a.tick();
    const uint64_t reference = state_hash(a);
    std::printf("save: %zu bytes, checksum %016llx\n", save.size(), static_cast<unsigned long long>(a.checksum()));

    // ---- replay from the save on every device (including the CPU via lavapipe)
    for (size_t i = 0; i < devices.size(); ++i) {
        GameSim b(*game, devices[i]);
        b.load(save);
        CHECK(b.ticks() == 10);
        CHECK(b.save() == save);  // round trip is byte-identical
        CHECK(b.modifier_alive(h));  // handles survive a load
        for (int t = 0; t < 10; ++t) b.tick();
        const uint64_t replay = state_hash(b);
        std::printf("  replay on %-45s %s\n", names[i].c_str(), replay == reference ? "identical" : "DIFFERS");
        CHECK(replay == reference);
        CHECK(b.modifier_alive(h) && a.modifier_alive(h));  // due at tick 25: alive at 20 in both runs
    }

    // ---- state hashes on the device: the host's hashes of a snapshot, the same on every device;
    // and two saves that differ in one value are told apart down to the row
    {
        std::vector<std::vector<FieldHash>> per_device;
        for (size_t i = 0; i < devices.size(); ++i) {
            GameSim b(*game, devices[i]);
            b.load(save);
            for (int t = 0; t < 3; ++t) b.tick();
            per_device.push_back(b.field_hashes());
            CHECK(per_device.back() == field_hashes(b.snapshot()));
            CHECK(per_device.back() == per_device.front());
        }
        const auto groups = entity_hashes(per_device.front());
        CHECK(groups.size() == 3 && groups[0].name == "terni" && groups[1].name == "empire" && groups[2].name == "planet");

        GameSim b(*game, devices[0]);
        b.load(save);
        b.write_field(planet, "pop", 7, {fx::qty(1)});
        const std::vector<uint8_t> other = b.save();
        const auto diffs = differences(save_snapshot(save), save_snapshot(other));
        CHECK(diffs.size() == 1 && diffs[0].field == "planet.pop" && diffs[0].first_row == 7 && diffs[0].rows == 1);
        CHECK(differences(save_snapshot(save), save_snapshot(save)).empty());
        CHECK(field_hashes(save_snapshot(save)) != field_hashes(save_snapshot(other)));
    }

    // ---- the handle from before the save behaves the same as in the original run
    {
        GameSim b(*game, devices.back());
        b.load(save);
        CHECK(b.set_modifier_scale(h, fx::ONE));
        CHECK(b.remove_modifier(h));
        CHECK(!b.modifier_alive(h));
    }

    // ---- rejected saves leave the simulation untouched
    {
        GameSim c(*game, devices[0]);
        for (int t = 0; t < 3; ++t) c.tick();
        const uint64_t before = state_hash(c);

        std::vector<uint8_t> corrupt = save;
        corrupt[corrupt.size() / 2] ^= 0x40;
        CHECK(throws_with([&] { c.load(corrupt); }, "corrupt"));

        std::vector<uint8_t> truncated(save.begin(), save.begin() + std::ptrdiff_t(save.size() / 3));
        CHECK(throws_with([&] { c.load(truncated); }, "save:"));

        CHECK(throws_with([&] { c.load(std::vector<uint8_t>{1, 2, 3}); }, "not a Terni save"));
        CHECK(state_hash(c) == before && c.ticks() == 3);
    }

    // ---- a different game (one define changed, like a mod would) can't load it
    {
        auto modded = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
        modded->define_values[0] += 1;
        CHECK(game_checksum(*modded) != game_checksum(*game));
        GameSim m(*modded, devices[0]);
        CHECK(throws_with([&] { m.load(save); }, "different game"));
    }

    // ---- format 1: loads into its own game, and saves again as the same format 2 bytes
    {
        const std::vector<uint8_t> old = as_format_1(save);
        CHECK(save_checksum(old) == game_checksum(*game) && save_checksum(save) == game_checksum(*game));
        GameSim b(*game, devices[0]);
        CHECK(b.load(old).empty());
        CHECK(b.save() == save);
        auto modded = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
        modded->define_values[0] += 1;
        GameSim m(*modded, devices[0]);
        CHECK(throws_with([&] { m.load(old, SaveMatch::ByName); }, "format version 1"));
    }

    // ---- a patched game loads the old save by name
    {
        const auto original = town_world(0), patched = town_world(1);
        CHECK(original->ok() && patched->ok() && original->warnings.empty() && patched->warnings.empty());
        const uint32_t town = 0, cart = 1;
        GameSim a(*original, devices[0]);
        for (int t = 0; t < 3; ++t) a.tick();
        CHECK(a.create_entity(cart) == 3 && a.create_entity(cart) == 4);
        CHECK(a.destroy_entity(cart, 1));
        const ModifierHandle boom = a.add_modifier(uint32_t(original->bundle_index("boom")), town, 1);
        const ModifierHandle doomed = a.add_modifier(uint32_t(original->bundle_index("doomed")), town, 2, fx::ONE, 50);
        const ModifierHandle glow = a.add_modifier(uint32_t(original->bundle_index("glow")), 2, 0);
        a.write_field(town, "stock", 3, {fx::qty(77), fx::qty(78)});
        for (int t = 0; t < 2; ++t) a.tick();
        const std::vector<uint8_t> old = a.save();
        const auto pop = a.read_field(town, "pop"), stock = a.read_field(town, "stock"), load = a.read_field(cart, "load");
        CHECK(save_checksum(old) == game_checksum(*original));

        std::vector<uint64_t> after;  // per device: the state some ticks after the load
        for (size_t i = 0; i < devices.size(); ++i) {
            GameSim b(*patched, devices[i]);
            CHECK(throws_with([&] { b.load(old); }, "different game"));
            const std::vector<std::string> notes = b.load(old, SaveMatch::ByName);
            if (i == 0)
                for (const auto& n : notes) std::printf("  note: %s\n", n.c_str());
            CHECK(has_note(notes, "another version of the game") && has_note(notes, "entity type 'road' is new") &&
                  has_note(notes, "entity type 'relic' is gone") && has_note(notes, "'town': capacity 8, now 12") &&
                  has_note(notes, "'cart': capacity 16, now 32") && has_note(notes, "field 'town.fertility' is new") &&
                  has_note(notes, "field 'cart.fresh' is new: it starts at its default") &&
                  has_note(notes, "'town.stock': 2 values per row, now 3") && has_note(notes, "field 'town.old' is gone") &&
                  has_note(notes, "1 modifier instance dropped: bundle 'doomed' is gone") &&
                  has_note(notes, "1 modifier instance dropped: entity type 'relic' is gone"));
            CHECK(notes.size() == 11);
            CHECK(b.ticks() == 5);

            bool same = true;
            const auto pop2 = b.read_field(town, "pop"), stock2 = b.read_field(town, "stock");
            const auto fertility = b.read_field(town, "fertility");
            for (uint32_t t = 0; t < 12; ++t) {
                same &= pop2[t] == (t < 8 ? pop[t] : 0);
                for (uint32_t k = 0; k < 3; ++k)
                    same &= stock2[t * 3 + k] == (t < 8 && k < 2 ? stock[t * 2 + k] : fx::qty(9));
                same &= fertility[t] == int32_t(t < 5 ? 10 * t : 0);
            }
            const auto load2 = b.read_field(cart, "load"), fresh = b.read_field(cart, "fresh");
            for (uint32_t c = 0; c < 32; ++c) {
                same &= load2[c] == (c < 16 ? load[c] : 0) && fresh[c] == 7;
                same &= b.entity_alive(cart, c) == (c < 16 && a.entity_alive(cart, c));
                same &= b.entity_generation(cart, c) == (c < 16 ? a.entity_generation(cart, c) : 0);
            }
            same &= b.entity_count(cart) == a.entity_count(cart) && b.entities_alive(cart) == a.entities_alive(cart);
            same &= b.entity_count(2) == 2 && b.read_field(2, "length") == std::vector<int32_t>({3, 4, 0, 0});
            CHECK(same);
            CHECK(b.modifier_alive(boom) && !b.modifier_alive(doomed) && !b.modifier_alive(glow) && b.modifier_count() == 1);
            const uint32_t growth = uint32_t(patched->modifier_key_index("growth"));
            CHECK(b.modifier_total(growth, town, 1) == fx::rate(1, 10) && b.modifier_total(growth, town, 2) == 0);

            const std::vector<uint8_t> resaved = b.save();  // now a save of the patched game
            GameSim c(*patched, devices[i]);
            CHECK(c.load(resaved).empty() && c.save() == resaved);
            CHECK(b.create_entity(cart) == 1);  // the destroyed row rested long ago
            for (int t = 0; t < 4; ++t) b.tick();
            CHECK(b.read_field(town, "pop")[0] == pop[0] + 8);  // the patched rule goes on from there
            after.push_back(state_hash(b));
            std::printf("  patched game on %-45s %s\n", names[i].c_str(), after.back() == after.front() ? "identical" : "DIFFERS");
        }
        for (uint64_t h2 : after) CHECK(h2 == after.front());

        // what cannot be fitted is refused, and the simulation stays as it was
        const char* why[] = {"", "", "must convert", "capacity", "dynamic"};
        for (int version = 2; version <= 4; ++version) {
            const auto other = town_world(version);
            CHECK(other->ok());
            GameSim d(*other, devices[0]);
            d.tick();
            const uint64_t before = state_hash(d);
            CHECK(throws_with([&] { d.load(old, SaveMatch::ByName); }, why[version]));
            CHECK(state_hash(d) == before && d.ticks() == 1);
        }
    }

    std::printf("test_save: %s (%zu device(s))\n", failures == 0 ? "PASS" : "FAIL", devices.size());
    return failures == 0 ? 0 : 1;
}
