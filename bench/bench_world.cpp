// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#include "bench_world.hpp"

#include <locale>
#include <sstream>

#include "terni/rng.hpp"

namespace terni::bench {

// ---------------------------------------------------------------------------------------
// The rules. Each has a twin in cpu_reference.cpp; change both together, and BENCH_VERSION.
// ---------------------------------------------------------------------------------------

// Pops work: output by size, skill and the province's modifiers (its own and its
// empire's), sold at the market price; the empire takes its tax. What they make reaches
// the market through their province (province_economy).
const char* POP_WORK = R"(
void main() {
    FOR_EACH_POP(i) {
        const uint p = pop_home[i];
        const uint k = province_market[p] * GOODS + uint(pop_job[i]);
        const int bonus = fx_clamp(FX_ONE + province_mod_output(p), FX_ONE / 10, 4 * FX_ONE);
        const int made = fx_mul(fx_mul(fx_mul(pop_size[i], pop_skill[i]), bonus), D.output_per_person);
        pop_made[i] = made;
        const int income = fx_mul(made, market_price[k]);
        const int tax = fx_mul(income, empire_tax_rate[province_owner[p]]);
        pop_paid[i] = tax;
        pop_wealth[i] = fx_clamp(pop_wealth[i] + income - tax, 0, WEALTH_MAX);
    }
}
)";

// Pops buy what they need of every good; satisfaction is the share they could afford.
const char* POP_CONSUME = R"(
void main() {
    FOR_EACH_POP(i) {
        const uint m = province_market[pop_home[i]];
        int cost = 0;
        for (uint g = 0u; g < GOODS; ++g) {
            cost += fx_mul(fx_mul(pop_size[i], NEED[g]), market_price[m * GOODS + g]);
        }
        const int wealth = pop_wealth[i];
        pop_satisfaction[i] = cost <= wealth ? FX_ONE : fx_div(wealth, cost);
        pop_wealth[i] = wealth - fx_min(wealth, cost);
    }
}
)";

// Pops grow or shrink; unhappy pops sometimes try another good and keep it if it sells
// for more.
const char* POP_GROWTH = R"(
void main() {
    FOR_EACH_POP(i) {
        const uint p = pop_home[i];
        const int growth = D.base_growth + fx_mul(pop_satisfaction[i] - D.sat_neutral, D.sat_growth) +
                           province_mod_pop_growth(p) - fx_mul(province_unrest[p], D.unrest_decline);
        pop_size[i] = fx_clamp(pop_size[i] + fx_mul(pop_size[i], growth), POP_MIN, POP_MAX);
        if (pop_satisfaction[i] < D.sat_neutral && rng_below(rng_roll(i, terni.tick, 1u), D.job_change_chance)) {
            const uint m = province_market[p] * GOODS, job = uint(pop_job[i]);
            const uint other = (job + 1u + rng_roll(i, terni.tick, 2u) % (GOODS - 1u)) % GOODS;
            if (market_price[m + other] > market_price[m + job]) pop_job[i] = int(other);
        }
    }
}
)";

// Armies raised last tick take their strength from their province; every army wears down,
// is paid for by its province's empire and calms its province until it is gone.
const char* ARMY_UPKEEP = R"(
void main() {
    FOR_EACH_ARMY(a) {
        const uint p = army_home[a];
        if (army_fresh[a] != 0) {
            army_fresh[a] = 0;
            army_strength[a] = 20 + fx_min(province_population[p] / 1000, 80);
        }
        army_strength[a] -= 1 + int(rng_roll(a, terni.tick, 4u) & 3u);
        if (army_strength[a] <= 0) {
            army_destroy(a);
        } else {
            atomicAdd(province_garrison[p], 1);
            atomicAdd(empire_upkeep[province_owner[p]], army_strength[a] * D.army_upkeep);
        }
    }
}
)";

// One workgroup per province sums its pops: what they made and need of every good in
// shared memory, then added to the province's market once (sums of many pops into few
// markets would otherwise all queue on the same global atomics); the rest in a fixed
// tree. The province pays its taxes and costs its empire for administration, and its
// unrest moves: up with its grievance and its pops' discontent, down with its garrison
// and its empire's stability. A restless province may raise an army.
const char* PROVINCE_ECONOMY = R"(
shared int s_people[256];
shared int s_paid[256];
shared int s_satisfaction[256];
shared uvec2 s_wealth[256];
shared int s_supply[GOODS];
shared int s_demand[GOODS];

void main() {
    FOR_EACH_PROVINCE_GROUP(p) {
        const uint t = gl_LocalInvocationID.x;
        const uint first = province_first_pop[p], rows = province_pop_rows[p];
        if (t < GOODS) {
            s_supply[t] = 0;
            s_demand[t] = 0;
        }
        workgroup_sync();
        int people = 0, paid = 0, satisfaction = 0;
        uvec2 wealth = uvec2(0u);
        for (uint k = t; k < rows; k += 256u) {
            const uint i = first + k;
            people += to_agg(pop_size[i]);
            paid += to_agg(pop_paid[i]);
            satisfaction += pop_satisfaction[i];
            wealth = add_i64(wealth, i64_from_int(pop_wealth[i]));
            atomicAdd(s_supply[pop_job[i]], to_agg(pop_made[i]));
            for (uint g = 0u; g < GOODS; ++g) atomicAdd(s_demand[g], to_agg(fx_mul(pop_size[i], NEED[g])));
        }
        s_people[t] = people;
        s_paid[t] = paid;
        s_satisfaction[t] = satisfaction;
        s_wealth[t] = wealth;
        workgroup_sync();
        for (uint step = 128u; step > 0u; step >>= 1u) {
            if (t < step) {
                s_people[t] += s_people[t + step];
                s_paid[t] += s_paid[t + step];
                s_satisfaction[t] += s_satisfaction[t + step];
                s_wealth[t] = add_i64(s_wealth[t], s_wealth[t + step]);
            }
            workgroup_sync();
        }
        if (t < GOODS) {
            const uint k = province_market[p] * GOODS + t;
            atomicAdd(market_supply[k], s_supply[t]);
            atomicAdd(market_demand[k], s_demand[t]);
        }
        if (t == 0u) {
            const int mood = rows == 0u ? 0 : s_satisfaction[0] / int(rows);
            const uint e = province_owner[p];
            province_population[p] = s_people[0];
            province_wealth[p] = s_wealth[0];
            province_satisfaction[p] = mood;
            atomicAdd(empire_income[e], s_paid[0]);
            atomicAdd(empire_upkeep[e], s_people[0] / D.admin_people);
            const int unrest = province_unrest[p] + province_grievance[p] + fx_mul(D.sat_neutral - mood, D.unrest_rate) -
                               province_garrison[p] * D.garrison_calm - fx_mul(empire_stability[e], D.stability_calm);
            province_unrest[p] = fx_clamp(unrest, 0, FX_ONE);
            if (province_unrest[p] > D.revolt_at && rng_below(rng_roll(p, terni.tick, 3u), D.revolt_chance))
                province_raise[p] += 1u;
        }
        workgroup_sync();
    }
}
)";

// Prices move a tenth of the way to the base price times demand over supply (at most 4).
const char* MARKET_PRICES = R"(
void main() {
    FOR_EACH_MARKET(m) {
        for (uint g = 0u; g < GOODS; ++g) {
            const uint k = m * GOODS + g;
            const int supply = market_supply[k], demand = market_demand[k];
            const int ratio = supply <= 0 || demand / supply >= 4 ? 4 * FX_ONE : fx_div(demand, supply);
            const int price = market_price[k];
            const int target = fx_mul(BASE_PRICE[g], ratio);
            market_price[k] = fx_clamp(price + fx_mul(target - price, D.price_adjust), D.price_min, D.price_max);
        }
    }
}
)";

const char* EMPIRE_BUDGET = R"(
void main() {
    FOR_EACH_EMPIRE(e) {
        empire_treasury[e] = add_i64(empire_treasury[e], empire_income[e] - empire_upkeep[e]);
        const int debt = is_neg_i64(empire_treasury[e]) ? D.debt_penalty : 0;
        empire_stability[e] = fx_clamp(D.base_stability + empire_mod_stability(e) - debt, 0, FX_ONE);
    }
}
)";

// Once a month: empires in debt raise taxes, the others lower them.
const char* EMPIRE_POLICY = R"(
void main() {
    FOR_EACH_EMPIRE(e) {
        const int step = is_neg_i64(empire_treasury[e]) ? D.tax_step_up : -D.tax_step_down;
        empire_tax_rate[e] = fx_clamp(empire_tax_rate[e] + step, D.tax_min, D.tax_max);
    }
}
)";

namespace {

std::string shared_constants() {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    auto array = [&](const char* name, const int32_t* v) {
        o << "const int " << name << "[" << GOODS << "] = int[" << GOODS << "](";
        for (uint32_t g = 0; g < GOODS; ++g) o << (g ? ", " : "") << v[g];
        o << ");\n";
    };
    o << "const uint GOODS = " << GOODS << "u;\n";
    array("BASE_PRICE", BASE_PRICE);
    array("NEED", NEED);
    o << "const int POP_MIN = " << POP_MIN << ", POP_MAX = " << POP_MAX << ", WEALTH_MAX = " << WEALTH_MAX << ";\n";
    return o.str();
}

struct Define {
    const char* name;
    FieldType type;
    int32_t raw;
};
const Define DEFINES[] = {
    {"output_per_person", FieldType::Rate, fx::rate(1, 100)},
    {"base_growth", FieldType::Rate, fx::rate(2, 10000)},
    {"sat_neutral", FieldType::Rate, fx::rate(6, 10)},
    {"sat_growth", FieldType::Rate, fx::rate(2, 1000)},
    {"unrest_decline", FieldType::Rate, fx::rate(2, 1000)},
    {"job_change_chance", FieldType::Rate, fx::rate(2, 100)},
    {"price_adjust", FieldType::Rate, fx::rate(1, 10)},
    {"price_min", FieldType::Rate, fx::rate(1, 10)},
    {"price_max", FieldType::Rate, fx::rate(64)},
    {"unrest_rate", FieldType::Rate, fx::rate(5, 100)},
    {"garrison_calm", FieldType::Rate, fx::rate(1, 100)},
    {"stability_calm", FieldType::Rate, fx::rate(2, 100)},
    {"revolt_at", FieldType::Rate, fx::rate(1, 2)},
    {"revolt_chance", FieldType::Rate, fx::rate(5, 100)},
    {"army_upkeep", FieldType::Int, 1},
    {"admin_people", FieldType::Int, 1000},
    {"base_stability", FieldType::Rate, fx::rate(1, 2)},
    {"debt_penalty", FieldType::Rate, fx::rate(2, 10)},
    {"tax_step_up", FieldType::Rate, fx::rate(1, 100)},
    {"tax_step_down", FieldType::Rate, fx::rate(5, 1000)},
    {"tax_min", FieldType::Rate, fx::rate(5, 100)},
    {"tax_max", FieldType::Rate, fx::rate(1, 2)},
};

// A value from the seed: the same on every machine and compiler (no <random>, whose
// distributions differ between standard libraries).
uint32_t seeded(const BenchConfig& c, uint32_t row, uint32_t salt) { return rng::roll(row, c.seed, salt); }

}  // namespace

std::string BenchConfig::check() const {
    if (provinces == 0 || pops_per_province == 0 || empires == 0 || markets == 0)
        return "every size must be at least 1";
    if (empires > provinces || markets > provinces) return "there must be no more empires or markets than provinces";
    if (pops() > 0x7FFFFFFFull) return "too many pops (at most 2^31)";
    if (pops_per_province > 100000) return "at most 100000 pops per province";  // province sums stay in 32 bits
    return "";
}

std::vector<BenchConfig> standard_sizes() {
    // the largest take seconds per tick on a CPU: fewer ticks, still past a monthly stage
    return {
        {"tiny", 64, 16, 4, 8},
        {"small", 1000, 50, 20, 50},
        {"medium", 10000, 100, 100, 500},
        {"large", 30000, 200, 300, 1500},
        {"huge", 50000, 200, 500, 2500, 1, 50, 10},
        {"giant", 100000, 500, 1000, 5000, 1, 30, 5},
        {"colossal", 100000, 1000, 1000, 5000, 1, 30, 5},
    };
}

bool find_size(const std::string& name, BenchConfig& out) {
    for (const auto& s : standard_sizes())
        if (s.name == name) {
            out = s;
            return true;
        }
    return false;
}

std::unique_ptr<Game> build_world(const BenchConfig& c) {
    WorldBuilder b("bench");
    const uint32_t empire = b.entity("empire", c.empires);
    b.field(empire, "treasury", FieldType::Money64);
    b.field(empire, "income", FieldType::Agg);
    b.field(empire, "upkeep", FieldType::Agg);
    b.field(empire, "tax_rate", FieldType::Rate);
    b.field(empire, "stability", FieldType::Rate);

    const uint32_t market = b.entity("market", c.markets);
    b.field(market, "price", FieldType::Rate, "", GOODS);
    b.field(market, "supply", FieldType::Agg, "", GOODS);
    b.field(market, "demand", FieldType::Agg, "", GOODS);

    const uint32_t province = b.entity("province", c.provinces);
    b.field(province, "owner", FieldType::Ref, "empire");
    b.field(province, "market", FieldType::Ref, "market");
    b.field(province, "first_pop", FieldType::Count);
    b.field(province, "pop_rows", FieldType::Count);
    b.field(province, "population", FieldType::Agg);
    b.field(province, "wealth", FieldType::Money64);
    b.field(province, "satisfaction", FieldType::Rate);
    b.field(province, "unrest", FieldType::Rate);
    b.field(province, "grievance", FieldType::Rate);
    b.field(province, "garrison", FieldType::Agg);
    b.field(province, "raise", FieldType::Count);

    const uint32_t pop = b.entity("pop", static_cast<uint32_t>(c.pops()));
    b.field(pop, "home", FieldType::Ref, "province");
    b.field(pop, "size", FieldType::Quantity);
    b.field(pop, "job", FieldType::Int);
    b.field(pop, "skill", FieldType::Rate);
    b.field(pop, "wealth", FieldType::Quantity);
    b.field(pop, "satisfaction", FieldType::Rate);
    b.field(pop, "paid", FieldType::Quantity);
    b.field(pop, "made", FieldType::Quantity);

    const uint32_t army = b.entity("army", c.army_capacity());
    b.entity_dynamic(army);
    b.field(army, "home", FieldType::Ref, "province");
    b.field(army, "strength", FieldType::Int);
    b.field(army, "fresh", FieldType::Int, "", 1, 1);
    b.spawn(army, "province", "raise", "home");

    for (const auto& d : DEFINES) {
        b.define(d.name, d.type);
        b.set_define(d.name, d.raw);
    }

    const uint32_t output = b.modifier_key("output", province, FieldType::Rate, "owner");
    const uint32_t pop_growth = b.modifier_key("pop_growth", province, FieldType::Rate, "owner");
    const uint32_t stability = b.modifier_key("stability", empire, FieldType::Rate);
    b.bundle("harvest", {{output, fx::rate(2, 10)}, {pop_growth, fx::rate(1, 1000)}});
    b.bundle("famine", {{output, -fx::rate(3, 10)}, {pop_growth, -fx::rate(4, 1000)}});
    b.bundle("reform", {{stability, fx::rate(1, 10)}, {output, fx::rate(5, 100)}});
    b.modifier_capacity(1u << 16);

    const std::string constants = shared_constants();
    b.stage("pop_work", "pop", constants + POP_WORK);
    b.stage("pop_consume", "pop", constants + POP_CONSUME);
    b.stage("army_upkeep", "army", constants + ARMY_UPKEEP, {"province.garrison", "empire.upkeep"});
    b.stage("province_economy", "province", constants + PROVINCE_ECONOMY,
            {"market.supply", "market.demand", "empire.income"});
    b.stage_dispatch("province_economy", Dispatch::Workgroup);
    b.stage("pop_growth", "pop", constants + POP_GROWTH);
    b.stage("market_prices", "market", constants + MARKET_PRICES);
    b.stage("empire_budget", "empire", constants + EMPIRE_BUDGET);
    b.stage("empire_policy", "empire", constants + EMPIRE_POLICY);
    b.stage_schedule("empire_policy", 30, 29);

    // ---- the starting state
    for (uint32_t e = 0; e < c.empires; ++e) {
        b.create(empire);
        b.set(empire, e, "treasury", int64_t(seeded(c, e, 1) % 50000) - 10000);
        b.set(empire, e, "tax_rate", fx::rate(10, 100) + int32_t(seeded(c, e, 2) % uint32_t(fx::rate(15, 100))));
        b.set(empire, e, "stability", fx::rate(1, 2));
    }
    for (uint32_t m = 0; m < c.markets; ++m) {
        b.create(market);
        for (uint32_t g = 0; g < GOODS; ++g) b.set(market, m, "price", BASE_PRICE[g], g);
    }
    for (uint32_t p = 0; p < c.provinces; ++p) {
        b.create(province);
        b.set(province, p, "owner", uint64_t(p) * c.empires / c.provinces);  // empires and markets hold
        b.set(province, p, "market", uint64_t(p) * c.markets / c.provinces);  // neighbouring provinces
        b.set(province, p, "first_pop", uint64_t(p) * c.pops_per_province);
        b.set(province, p, "pop_rows", c.pops_per_province);
        b.set(province, p, "unrest", int32_t(seeded(c, p, 3) % uint32_t(fx::rate(45, 100))));
        b.set(province, p, "grievance", int32_t(seeded(c, p, 8) % uint32_t(fx::rate(25, 1000))));
    }
    const uint32_t pops = static_cast<uint32_t>(c.pops());
    for (uint32_t i = 0; i < pops; ++i) {
        b.create(pop);
        b.set(pop, i, "home", i / c.pops_per_province);
        b.set(pop, i, "size", fx::qty(200) + int32_t(seeded(c, i, 4) % uint32_t(fx::qty(4800))));
        b.set(pop, i, "job", seeded(c, i, 5) % GOODS);
        b.set(pop, i, "skill", fx::rate(1, 2) + int32_t(seeded(c, i, 6) % uint32_t(fx::ONE)));
        b.set(pop, i, "wealth", int32_t(seeded(c, i, 7) % uint32_t(fx::qty(100))));
        b.set(pop, i, "satisfaction", fx::rate(1, 2));
    }
    for (uint32_t p = 0; p < c.provinces; p += 16) {
        const uint32_t row = b.create(army);
        b.set(army, row, "home", p);
    }
    return b.finish();
}

void host_events(const Game& game, const BenchConfig& c, uint64_t tick, std::vector<HostEvent>& out) {
    out.clear();
    const uint32_t harvest = uint32_t(game.bundle_index("harvest")), famine = uint32_t(game.bundle_index("famine")),
                   reform = uint32_t(game.bundle_index("reform"));
    const uint32_t t = static_cast<uint32_t>(tick);
    // a few provinces a tick get a harvest or a famine for 5 to 44 ticks, at 0.5x to 1.5x
    const uint32_t n = 1 + c.provinces / 256;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t r = rng::roll(k, t, 101), s = rng::roll(k, t, 102);
        out.push_back({(r >> 20) & 1u ? harvest : famine, PROVINCE, r % c.provinces,
                       fx::rate(1, 2) + int32_t(s % uint32_t(fx::ONE)), 5 + (s >> 16) % 40});
    }
    // every tenth tick an empire reforms, for 50 ticks
    if (t % 10 == 0) out.push_back({reform, EMPIRE, rng::roll(0, t, 103) % c.empires, fx::ONE, 50});
}

uint64_t hash_words(const int32_t* words, size_t n) {
    uint64_t x = 0xcbf29ce484222325ull ^ n;  // as terni::hash_values
    for (size_t i = 0; i < n; ++i) {
        x ^= static_cast<uint64_t>(static_cast<int64_t>(words[i]));
        x *= 0x100000001b3ull;
        x ^= x >> 32;
    }
    return x;
}

uint64_t StateHashes::combined() const {
    std::vector<uint64_t> h;
    for (const auto& f : fields) h.push_back(f.second);
    return combine(h);
}

std::string first_difference(const StateHashes& a, const StateHashes& b) {
    for (size_t i = 0; i < a.fields.size() && i < b.fields.size(); ++i)
        if (a.fields[i] != b.fields[i]) return a.fields[i].first;
    return a.fields.size() == b.fields.size() ? "" : "<field count>";
}

StateHashes state_hashes(GameSim& sim, const Game& game) {
    StateHashes s;
    auto add = [&](std::string name, const std::vector<int32_t>& words) {
        s.fields.emplace_back(std::move(name), hash_words(words.data(), words.size()));
    };
    for (uint32_t e = 0; e < game.entities.size(); ++e) {
        const EntityDef& def = game.entities[e];
        const uint32_t count = sim.entity_count(e);
        for (const auto& f : def.fields)
            add(def.name + "." + f.name, count ? sim.read_field(e, f.name, 0, count) : std::vector<int32_t>{});
        if (def.dynamic) {
            std::vector<int32_t> alive(count), generation(count);
            for (uint32_t r = 0; r < count; ++r) {
                alive[r] = sim.entity_alive(e, r) ? 1 : 0;
                generation[r] = static_cast<int32_t>(sim.entity_generation(e, r));
            }
            add(def.name + ".alive", alive);
            add(def.name + ".generation", generation);
        }
    }
    const int32_t tick[2] = {static_cast<int32_t>(sim.ticks()), static_cast<int32_t>(sim.ticks() >> 32)};
    s.fields.emplace_back("tick", hash_words(tick, 2));
    return s;
}

}  // namespace terni::bench
