// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Partial writes: write_field(entity, field, row, index, words) changes some values of one
// row of a sized field and leaves the rest as the GPU has it. Writes queued for one tick
// apply in the order they were made, even where they overlap (CI runs this test under
// synchronization validation too). money64 values take two words; the host copy of an
// inherit field follows partial writes; every device ends with the same state.
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

// Touches only `ticks`, so the fields under test hold exactly what the host wrote.
const char* COUNT = R"(
void main() {
    FOR_EACH_TOWN(t) {
        town_ticks[t] += 1;
    }
}
)";

constexpr uint32_t REALMS = 3, TOWNS = 40, GOODS = 6, PURSES = 3;

uint64_t state_hash(GameSim& sim) { return combine(hash_fields(sim.snapshot())); }

int main() {
    WorldBuilder b;
    const uint32_t realm = b.entity("realm", 4), town = b.entity("town", 64);
    b.field(town, "stock", FieldType::Int, "", GOODS);
    b.field(town, "purse", FieldType::Money64, "", PURSES);
    b.field(town, "owner", FieldType::Ref, "realm");
    b.field(town, "ticks", FieldType::Int);
    const uint32_t tax = b.modifier_key("tax", town, FieldType::Rate, "owner");
    const uint32_t levy = b.bundle("levy", {{tax, fx::rate(1, 100)}});
    b.stage("count", "town", COUNT);
    for (uint32_t r = 0; r < REALMS; ++r) b.create(realm);
    for (uint32_t t = 0; t < TOWNS; ++t) {
        const uint32_t row = b.create(town);
        for (uint32_t g = 0; g < GOODS; ++g) b.set(town, row, "stock", int64_t(100 * t + g), g);
        b.set(town, row, "owner", t % REALMS);
    }
    auto world = b.finish();
    for (const auto& e : world->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& w : world->warnings) std::printf("  warning: %s\n", w.text().c_str());
    if (!world->ok()) return 1;

    std::vector<uint64_t> hashes;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        std::printf("device %u: %s\n", d.index, d.name.c_str());
        GameSimOptions o;
        o.device = int(d.index);
        GameSim sim(*world, o);

        // host model of `stock`, all capacity rows
        std::vector<int32_t> stock = sim.read_field(town, "stock");
        auto set = [&](uint32_t row, uint32_t index, const std::vector<int32_t>& v) {
            sim.write_field(town, "stock", row, index, v);
            for (size_t k = 0; k < v.size(); ++k) stock[row * GOODS + index + k] = v[k];
        };
        auto set_row = [&](uint32_t row, const std::vector<int32_t>& v) {
            sim.write_field(town, "stock", row, v);
            for (size_t k = 0; k < v.size(); ++k) stock[row * GOODS + k] = v[k];
        };

        // single values, a run of values, and a run that ends at the last value
        set(5, 2, {7});
        set(6, 3, {8, 9});
        set(9, GOODS - 2, {-1, -2});
        // overlapping writes in one tick: the later one wins, whichever kind it is
        set_row(7, {1, 2, 3, 4, 5, 6});
        set(7, 1, {50});
        set(7, 1, {60, 61});
        set(8, 5, {70});
        set_row(8, {11, 12, 13, 14, 15, 16});
        set(8, 0, {80});
        sim.tick();
        CHECK(sim.read_field(town, "stock") == stock);

        // many small, overlapping writes in one tick (copies are batched per buffer)
        uint32_t x = 12345;
        for (int i = 0; i < 3000; ++i) {
            x = x * 1103515245u + 12345u;
            const uint32_t row = (x >> 8) % TOWNS, index = (x >> 16) % GOODS;
            const uint32_t n = 1 + (x >> 24) % (GOODS - index);
            std::vector<int32_t> v(n);
            for (uint32_t k = 0; k < n; ++k) v[k] = int32_t(i * 8 + k);
            set(row, index, v);
            if (i % 500 == 0) set_row(row, {i, i, i, i, i, i});
        }
        sim.tick();
        CHECK(sim.read_field(town, "stock") == stock);
        CHECK(sim.read_field(town, "ticks", 0, 1)[0] == 2);  // the rule ran; nothing else touched

        // money64: two words per value
        const std::vector<int32_t> purse_before = sim.read_field(town, "purse", 3, 1);
        sim.write_field(town, "purse", 3, 1, {int32_t(0x89abcdef), 0x01234567});
        sim.tick();
        std::vector<int32_t> purse = sim.read_field(town, "purse", 3, 1);
        CHECK(purse.size() == 2 * PURSES);
        CHECK(purse[2] == int32_t(0x89abcdef) && purse[3] == 0x01234567);
        CHECK(purse[0] == purse_before[0] && purse[1] == purse_before[1]);
        CHECK(purse[4] == purse_before[4] && purse[5] == purse_before[5]);

        // the host copy of an inherit field follows partial writes, without a GPU read
        sim.add_modifier(levy, realm, 2, fx::rate(5));
        sim.tick();
        CHECK(sim.modifier_total(tax, town, 4) == 0);  // town 4 belongs to realm 1
        const uint64_t reads = sim.sync_reads();
        sim.write_field(town, "owner", 4, 0, {2});
        const int32_t realm_levy = sim.modifier_own_total(tax, realm, 2);
        if (sim.modifier_total(tax, town, 4) != realm_levy)
            std::printf("  total %d, realm's own %d\n", sim.modifier_total(tax, town, 4), realm_levy);
        CHECK(realm_levy != 0 && sim.modifier_total(tax, town, 4) == realm_levy);
        CHECK(sim.sync_reads() == reads);
        sim.tick();
        CHECK(sim.modifier_total(tax, town, 4) == realm_levy);

        // bad calls throw and queue nothing
        CHECK(throws<std::out_of_range>([&] { sim.write_field(town, "stock", 0, GOODS, {1}); }));
        CHECK(throws<std::out_of_range>([&] { sim.write_field(town, "stock", 0, GOODS - 1, {1, 2}); }));
        CHECK(throws<std::out_of_range>([&] { sim.write_field(town, "stock", 64, 0, {1}); }));
        CHECK(throws<std::invalid_argument>([&] { sim.write_field(town, "purse", 0, 0, {1}); }));
        CHECK(throws<std::invalid_argument>([&] { sim.write_field(town, "wealth", 0, 0, {1}); }));
        sim.write_field(town, "stock", 0, GOODS, std::vector<int32_t>{});  // nothing to write: allowed
        sim.tick();
        CHECK(sim.read_field(town, "stock") == stock);

        hashes.push_back(state_hash(sim));
    }
    CHECK(!hashes.empty());
    for (uint64_t h : hashes) CHECK(h == hashes.front());

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("partial writes: ok\n");
    return failures ? 1 : 0;
}
