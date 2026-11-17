// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Inherited modifier totals on the host: modifier_total / modifier_breakdown follow the
// ref field that rules change every tick (owner), host writes and load(), and agree with
// what the GPU accessor sees, without waiting for the GPU after the first query. The same
// for a key that inherits in several steps (province, district, region, realm). Inherit
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

// Three steps: every level records what its accessor gives, then moves to another parent.
const char* PROVINCES = R"(
void main() {
    FOR_EACH_PROVINCE(p) {
        province_seen[p] = province_mod_tax(p);
        province_district[p] = (province_district[p] + p + terni.tick) % terni.district_count;
    }
}
)";
const char* DISTRICTS = R"(
void main() {
    FOR_EACH_DISTRICT(d) {
        district_seen[d] = district_mod_tax(d);
        district_region[d] = (district_region[d] + d + terni.tick) % terni.region_count;
    }
}
)";
const char* REGIONS = R"(
void main() {
    FOR_EACH_REGION(r) {
        region_seen[r] = region_mod_tax(r);
        region_realm[r] = (region_realm[r] + r + terni.tick) % terni.realm_count;
    }
}
)";

// A province's total is its own plus its district's, that district's region's and that
// region's realm's; a district's, region's or realm's total starts at its own level.
static void test_chain() {
    constexpr uint32_t REALMS = 3, REGIONS_N = 5, DISTRICTS_N = 11, PROVINCES_N = 50;
    constexpr uint32_t NONE = 0xFFFFFFFFu;
    WorldBuilder b;
    const uint32_t realm = b.entity("realm", 4), region = b.entity("region", 8);
    const uint32_t district = b.entity("district", 16), province = b.entity("province", 64);
    b.field(region, "realm", FieldType::Ref, "realm");
    b.field(region, "seen", FieldType::Rate);
    b.field(district, "region", FieldType::Ref, "region");
    b.field(district, "seen", FieldType::Rate);
    b.field(province, "district", FieldType::Ref, "district");
    b.field(province, "seen", FieldType::Rate);
    const uint32_t tax = b.modifier_key("tax", province, FieldType::Rate, "district.region.realm");
    const uint32_t levy = b.modifier_key("levy", district, FieldType::Rate, "region");  // shares district.region
    const uint32_t boost = b.bundle("boost", {{tax, fx::rate(1, 100)}, {levy, fx::rate(3, 100)}});
    b.stage("provinces", "province", PROVINCES);
    b.stage("districts", "district", DISTRICTS);
    b.stage("regions", "region", REGIONS);
    for (uint32_t i = 0; i < REALMS; ++i) b.create(realm);
    for (uint32_t i = 0; i < REGIONS_N; ++i) b.set(region, b.create(region), "realm", i % REALMS);
    for (uint32_t i = 0; i < DISTRICTS_N; ++i) b.set(district, b.create(district), "region", i % REGIONS_N);
    for (uint32_t i = 0; i < PROVINCES_N; ++i) b.set(province, b.create(province), "district", i % DISTRICTS_N);
    auto world = b.finish();
    for (const auto& e : world->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& w : world->warnings) std::printf("  warning: %s\n", w.text().c_str());
    CHECK(world->ok() && world->warnings.empty());
    if (!world->ok()) return;
    const auto& steps = world->modifier_keys[tax].inherit_steps;
    CHECK(steps.size() == 3 && steps[0].entity == district && steps[1].entity == region && steps[2].entity == realm);

    std::vector<std::vector<int32_t>> first;  // every device ends with the same values
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        GameSimOptions o;
        o.device = int(d.index);
        GameSim sim(*world, o);
        // a different power of ten per level, so a total shows which rows it took
        for (uint32_t i = 0; i < REALMS; ++i) sim.add_modifier(boost, realm, i, fx::rate(int32_t(i) + 1));
        for (uint32_t i = 0; i < REGIONS_N; ++i) sim.add_modifier(boost, region, i, fx::rate(10 * (int32_t(i) + 1)));
        for (uint32_t i = 0; i < DISTRICTS_N; ++i) sim.add_modifier(boost, district, i, fx::rate(100 * (int32_t(i) + 1)));
        sim.add_modifier(boost, province, 3, fx::rate(7000));
        sim.add_modifier(boost, province, 8, fx::rate(-2000));
        sim.tick();  // the modifiers reach the GPU

        // the chain walked by hand, from the fields as the GPU has them
        auto by_hand = [&](uint32_t key, uint32_t entity, uint32_t row) {
            const std::vector<int32_t> pd = sim.read_field(province, "district"), dr = sim.read_field(district, "region"),
                                       rr = sim.read_field(region, "realm");
            int32_t total = 0;
            const bool is_tax = key == tax;
            if (entity == province) {
                total += sim.modifier_own_total(key, province, row);
                entity = district, row = uint32_t(pd[row]);
            }
            if (entity == district && row < 16) {
                total += sim.modifier_own_total(key, district, row);
                entity = region, row = uint32_t(dr[row]);
            }
            if (entity == region && row < 8) {
                total += sim.modifier_own_total(key, region, row);
                entity = realm, row = uint32_t(rr[row]);
                if (!is_tax) return total;  // levy stops at the region
            }
            if (entity == realm && row < 4) total += sim.modifier_own_total(key, realm, row);
            return total;
        };
        auto all_agree = [&] {
            bool ok = true;
            for (uint32_t p = 0; p < PROVINCES_N; ++p) ok &= sim.modifier_total(tax, province, p) == by_hand(tax, province, p);
            for (uint32_t i = 0; i < DISTRICTS_N; ++i) {
                ok &= sim.modifier_total(tax, district, i) == by_hand(tax, district, i);
                ok &= sim.modifier_total(levy, district, i) == by_hand(levy, district, i);
            }
            for (uint32_t i = 0; i < REGIONS_N; ++i) ok &= sim.modifier_total(tax, region, i) == by_hand(tax, region, i);
            for (uint32_t i = 0; i < REALMS; ++i)
                ok &= sim.modifier_total(tax, realm, i) == sim.modifier_own_total(tax, realm, i);
            return ok;
        };
        struct Totals {
            std::vector<int32_t> province, district, region;
        };
        auto totals = [&] {
            Totals t;
            for (uint32_t p = 0; p < PROVINCES_N; ++p) t.province.push_back(sim.modifier_total(tax, province, p));
            for (uint32_t i = 0; i < DISTRICTS_N; ++i) t.district.push_back(sim.modifier_total(tax, district, i));
            for (uint32_t i = 0; i < REGIONS_N; ++i) t.region.push_back(sim.modifier_total(tax, region, i));
            return t;
        };

        uint64_t reads = sim.sync_reads();
        Totals before = totals();
        CHECK(sim.sync_reads() == reads + 3);  // each of the three inherit fields is read once
        (void)sim.modifier_total(levy, district, 0);
        CHECK(sim.sync_reads() == reads + 3);  // levy goes through a field tax already follows
        bool same = all_agree(), quiet = true, agrees = true, parts = true;
        for (int t = 0; t < 6; ++t) {
            reads = sim.sync_reads();
            sim.tick();
            const Totals after = totals();
            for (uint32_t p = 0; p < PROVINCES_N; ++p) {
                int32_t sum = 0;
                uint32_t last = province;  // nearest first: province, district, region, realm
                for (const auto& c : sim.modifier_breakdown(tax, province, p)) {
                    sum += c.value;
                    parts &= c.entity <= last;
                    last = c.entity;
                }
                parts &= sum == after.province[p];
            }
            quiet &= sim.sync_reads() == reads;  // no waiting for the GPU after a tick
            same &= all_agree();
            // the GPU saw what the host said before the tick, at every level
            agrees &= sim.read_field(province, "seen", 0, PROVINCES_N) == before.province;
            agrees &= sim.read_field(district, "seen", 0, DISTRICTS_N) == before.district;
            agrees &= sim.read_field(region, "seen", 0, REGIONS_N) == before.region;
            before = after;
        }
        CHECK(same);
        CHECK(quiet);
        CHECK(agrees);
        CHECK(parts);

        // a ref that points at no row ends the chain there, on the host and on the GPU
        const uint32_t d5 = uint32_t(sim.read_field(province, "district", 5, 1).at(0));
        sim.write_field(district, "region", d5, {int32_t(NONE)});
        const int32_t cut = sim.modifier_total(tax, province, 5);
        CHECK(cut == sim.modifier_own_total(tax, province, 5) + sim.modifier_own_total(tax, district, d5));
        CHECK(sim.modifier_total(tax, district, d5) == sim.modifier_own_total(tax, district, d5));
        // a host write in the middle of the chain counts at once
        const uint32_t d6 = uint32_t(sim.read_field(province, "district", 6, 1).at(0));
        sim.write_field(district, "region", d6, {4});
        sim.write_field(region, "realm", 4, {2});
        const int32_t moved = sim.modifier_total(tax, province, 6);
        CHECK(moved == sim.modifier_own_total(tax, province, 6) + sim.modifier_own_total(tax, district, d6) +
                           sim.modifier_own_total(tax, region, 4) + sim.modifier_own_total(tax, realm, 2));
        sim.tick();
        CHECK(sim.read_field(province, "seen", 5, 1).at(0) == cut);
        CHECK(sim.read_field(province, "seen", 6, 1).at(0) == moved);
        CHECK(all_agree());

        // load() brings back every level
        const std::vector<uint8_t> save = sim.save();
        const Totals saved = totals();
        sim.tick();
        sim.tick();
        sim.load(save);
        const Totals loaded = totals();
        CHECK(loaded.province == saved.province && loaded.district == saved.district && loaded.region == saved.region);
        CHECK(all_agree());

        sim.tick();
        std::vector<int32_t> end = sim.read_field(province, "seen");
        const std::vector<int32_t> ds = sim.read_field(district, "seen"), rs = sim.read_field(region, "seen");
        end.insert(end.end(), ds.begin(), ds.end());
        end.insert(end.end(), rs.begin(), rs.end());
        if (first.empty()) first.push_back(end);
        CHECK(first[0] == end);
        std::printf("three-step totals on %-44s %s\n", d.name.c_str(),
                    same && quiet && agrees && parts && first[0] == end ? "ok" : "WRONG");
    }

    // chains that cannot work are rejected, each with its own message
    {
        WorldBuilder w;
        const uint32_t a = w.entity("realm", 4), r = w.entity("region", 4), p = w.entity("province", 4);
        w.field(r, "realm", FieldType::Ref, "realm");
        w.field(a, "capital", FieldType::Ref, "province");
        w.field(p, "region", FieldType::Ref, "region");
        w.modifier_key("loop", p, FieldType::Rate, "region.realm.capital");
        w.modifier_key("missing", p, FieldType::Rate, "region.owner");
        w.modifier_key("empty", p, FieldType::Rate, "region.");
        auto bad = w.finish();
        bool loop = false, missing = false, empty = false;
        for (const auto& err : bad->errors) {
            loop |= err.message.find("'loop': inherit field 'capital' leads back to 'province'") != std::string::npos;
            missing |= err.message.find("'missing': inherit field 'owner' is not a ref field of 'region'") != std::string::npos;
            empty |= err.message.find("'empty': inherit field '' is not a ref field of 'region'") != std::string::npos;
        }
        CHECK(loop && missing && empty && bad->errors.size() == 3);
        for (const auto& k : bad->modifier_keys) CHECK(k.inherit_steps.empty());
    }

    // a data folder writes the steps as a list
    {
        auto g = load_game(std::string(TERNI_TEST_DATA) + "/chain_game");
        for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
        CHECK(g->ok() && g->warnings.empty());
        CHECK(g->modifier_keys.size() == 1 && g->modifier_keys[0].inherit_field == "district.region.realm" &&
              g->modifier_keys[0].inherit_steps.size() == 3);
    }
}

int main() {
    test_chain();
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
