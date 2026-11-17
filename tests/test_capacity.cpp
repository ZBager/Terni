// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Capacities chosen at run time: Game::set_capacity() resizes a loaded or built world,
// the rules see the new <ENTITY>_CAPACITY (a specialization constant, also in the
// inherited-modifier accessors), one set of compiled rules serves every capacity, and
// the rows in use behave the same whatever the room around them. Checksums and saves
// tell capacities apart.
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "terni/fixed.hpp"
#include "terni/game.hpp"
#include "terni/vk_context.hpp"

using namespace terni;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

template <class E, class F>
bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    } catch (...) {
    }
    return false;
}

const char* LOOK = R"(
const uint HALF_TOWNS = TOWN_CAPACITY / 2u;   // constant expressions of capacities work too

void main() {
    FOR_EACH_TOWN(t) {
        town_room[t] = int(TOWN_CAPACITY);
        town_half[t] = int(HALF_TOWNS);
        town_realm_room[t] = int(REALM_CAPACITY);
        town_seen[t] = town_mod_tax(t);
        town_level[t] += 1;
    }
}
)";

constexpr uint32_t REALMS = 3, TOWNS = 20;

std::unique_ptr<Game> make_world() {
    WorldBuilder b;
    const uint32_t realm = b.entity("realm", 4), town = b.entity("town", 32);
    b.field(town, "owner", FieldType::Ref, "realm");
    for (const char* f : {"room", "half", "realm_room", "seen"}) b.field(town, f, FieldType::Int);
    b.field(town, "level", FieldType::Int, "", 1, 7);  // default 7
    const uint32_t tax = b.modifier_key("tax", town, FieldType::Rate, "owner");
    b.bundle("levy", {{tax, fx::rate(3, 100)}});
    b.stage("look", "town", LOOK);
    for (uint32_t r = 0; r < REALMS; ++r) b.create(realm);
    for (uint32_t t = 0; t < TOWNS; ++t) b.set(town, b.create(town), "owner", t % REALMS);
    return b.finish();
}

uint64_t state_hash(GameSim& sim) { return combine(hash_fields(sim.snapshot())); }

int main() {
    auto small = make_world();
    for (const auto& e : small->errors) std::printf("  %s\n", e.text().c_str());
    if (!small->ok()) return 1;
    const uint32_t realm = uint32_t(small->entity_index("realm")), town = uint32_t(small->entity_index("town"));
    const uint32_t tax = uint32_t(small->modifier_key_index("tax")), levy = uint32_t(small->bundle_index("levy"));

    // ---- set_capacity on the description
    auto large = make_world();
    large->set_capacity(realm, 1000);
    large->set_capacity(town, 5000);
    {
        const auto& t = large->tables[town];
        const size_t level = size_t(large->entities[town].field_index("level"));
        CHECK(large->entities[town].capacity == 5000 && t.count == TOWNS);
        CHECK(t.columns[level].size() == 5000 && t.columns[level][4999] == 7 && t.columns[level][TOWNS] == 7);
        CHECK(game_checksum(*large) != game_checksum(*small));
        auto back = make_world();  // the same capacities give the same checksum again
        back->set_capacity(town, 5000);
        back->set_capacity(town, 32);
        CHECK(game_checksum(*back) == game_checksum(*small));
        CHECK(back->tables[town].columns[level] == small->tables[town].columns[level]);
        back->set_capacity(town, TOWNS);  // down to the rows in use
        CHECK(back->tables[town].columns[level].size() == TOWNS);
        CHECK(throws<std::out_of_range>([&] { back->set_capacity(town, TOWNS - 1); }));
        CHECK(throws<std::out_of_range>([&] { back->set_capacity(realm, 0); }));
        CHECK(throws<std::out_of_range>([&] { back->set_capacity(7, 10); }));
        CHECK(generate_prelude(*small) == generate_prelude(*large));  // nothing in the rules changes
    }

    // ---- one set of compiled rules for every capacity
    const fs::path cache = fs::path(TERNI_TEST_BINARY_DIR) / "capacity_cache";
    fs::remove_all(cache);
    const CompileStats filled = compile_rules(*small, cache.string());
    CHECK(filled.compiled > 0);
    CHECK(compile_rules(*large, cache.string()).compiled == 0);

    std::vector<uint64_t> hashes;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        std::printf("device %u: %s\n", d.index, d.name.c_str());
        GameSimOptions o;
        o.device = int(d.index);
        o.spirv_cache_dir = cache.string();
        o.cache_only = true;  // the small world's rules, never compiled for the large one
        GameSim a(*small, o), b(*large, o);
        CHECK(b.compile_stats().compiled == 0 && b.compile_stats().cached == a.compile_stats().cached);

        for (int k = 0; k < 3; ++k) a.tick(), b.tick();
        CHECK(b.read_field(town, "room", 0, 1)[0] == 5000 && a.read_field(town, "room", 0, 1)[0] == 32);
        CHECK(b.read_field(town, "half", 0, 1)[0] == 2500 && a.read_field(town, "half", 0, 1)[0] == 16);
        CHECK(b.read_field(town, "realm_room", 0, 1)[0] == 1000 && a.read_field(town, "realm_room", 0, 1)[0] == 4);
        // the rows in use behave the same whatever the room around them
        CHECK(a.read_field(town, "level") == b.read_field(town, "level", 0, 32));
        CHECK(a.read_field(town, "level", 0, TOWNS) == std::vector<int32_t>(TOWNS, 10));

        // the inherited-modifier accessor bounds refs by the capacity in use
        b.add_modifier(levy, realm, 700);
        b.write_field(town, "owner", 0, {700});
        b.tick();
        const int32_t levied = b.modifier_own_total(tax, realm, 700);
        CHECK(levied != 0 && b.read_field(town, "seen", 0, 1)[0] == levied);
        CHECK(b.modifier_total(tax, town, 0) == levied);

        // a save loads only into a world with the same capacities
        const std::vector<uint8_t> save = b.save();
        CHECK(throws<std::runtime_error>([&] { a.load(save); }));
        GameSim c(*large, o);
        c.load(save);
        c.tick(), b.tick();
        CHECK(state_hash(c) == state_hash(b));
        hashes.push_back(state_hash(b));
    }
    CHECK(!hashes.empty());
    for (uint64_t h : hashes) CHECK(h == hashes.front());

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("capacities: ok\n");
    return failures ? 1 : 0;
}
