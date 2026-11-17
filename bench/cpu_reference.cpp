// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#include "cpu_reference.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "terni/fixed.hpp"
#include "terni/rng.hpp"

namespace terni::bench {

namespace {

// Threads created once. run() splits [0, n) into one contiguous chunk per thread; the
// calling thread takes the first. Loops too small to be worth waking threads for run on
// fewer of them, or on the caller alone.
class ThreadPool {
public:
    explicit ThreadPool(unsigned threads) : start_(threads), done_(threads) {
        for (unsigned t = 1; t < threads; ++t) workers_.emplace_back([this, t] { work(t); });
    }
    ~ThreadPool() {
        stop_.store(true);
        if (!workers_.empty()) start_.arrive_and_wait();
        for (auto& w : workers_) w.join();
    }
    unsigned size() const { return static_cast<unsigned>(workers_.size()) + 1; }

    // f(begin, end, thread) for each chunk; at least `grain` items per thread.
    template <class F>
    void run(uint32_t n, uint32_t grain, F&& f) {
        const unsigned used = static_cast<unsigned>(std::min<uint64_t>(size(), std::max<uint64_t>(1, n / grain)));
        if (used <= 1) {
            f(0u, n, 0u);
            return;
        }
        n_ = n;
        used_ = used;
        ctx_ = &f;
        call_ = [](void* ctx, uint32_t b, uint32_t e, unsigned t) {
            (*static_cast<std::remove_reference_t<F>*>(ctx))(b, e, t);
        };
        start_.arrive_and_wait();
        chunk(0);
        done_.arrive_and_wait();
    }

private:
    void work(unsigned t) {
        for (;;) {
            start_.arrive_and_wait();
            if (stop_.load()) return;
            chunk(t);
            done_.arrive_and_wait();
        }
    }
    void chunk(unsigned t) {
        if (t >= used_) return;
        const uint32_t b = static_cast<uint32_t>(uint64_t(n_) * t / used_);
        const uint32_t e = static_cast<uint32_t>(uint64_t(n_) * (t + 1) / used_);
        call_(ctx_, b, e, t);
    }

    std::barrier<> start_, done_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    // the job of the current run(); written before the start barrier, which publishes it
    uint32_t n_ = 0;
    unsigned used_ = 0;
    void* ctx_ = nullptr;
    void (*call_)(void*, uint32_t, uint32_t, unsigned) = nullptr;
};

// money64 values are two words, low first, as on the GPU.
int64_t get64(const int32_t* words, size_t i) {
    return static_cast<int64_t>((uint64_t(uint32_t(words[2 * i + 1])) << 32) | uint32_t(words[2 * i]));
}
void set64(int32_t* words, size_t i, int64_t v) {
    words[2 * i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v)));
    words[2 * i + 1] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32));
}
int64_t add64(int64_t a, int64_t b) { return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b)); }

void atomic_add(int32_t& target, int32_t v) { std::atomic_ref<int32_t>(target).fetch_add(v, std::memory_order_relaxed); }

// Rows per thread below which a loop is not split further (roughly equal work per chunk).
constexpr uint32_t POP_GRAIN = 4096, PROVINCE_GRAIN = 64, MARKET_GRAIN = 256, ARMY_GRAIN = 2048, SUM_GRAIN = 4096;

}  // namespace

struct CpuWorld::Impl {
    const Game& game;
    BenchConfig config;
    ThreadPool pool;
    uint64_t tick = 0;

    // Every field, laid out as Terni lays it out on the GPU, and the rows in use.
    std::vector<std::vector<std::vector<int32_t>>> columns;  // [entity][field]
    std::vector<uint32_t> count;

    // the dynamic armies
    std::vector<uint8_t> alive, prev;  // prev: alive when the last tick ended
    std::vector<uint32_t> generation;

    // Modifiers: the own total of every key on every entity type that has it, and the
    // instances still to expire. Totals are sums of integers, so the order of adds and
    // removes does not matter.
    std::vector<std::vector<std::vector<int32_t>>> own;  // [key][entity][row], empty where the key does not apply
    struct Pending {
        uint64_t due;
        HostEvent event;
    };
    std::vector<Pending> pending;
    std::vector<HostEvent> events;

    // defines
    int32_t output_per_person, base_growth, sat_neutral, sat_growth, unrest_decline, job_change_chance, price_adjust,
        price_min, price_max, unrest_rate, garrison_calm, stability_calm, revolt_at, revolt_chance, army_upkeep,
        admin_people, base_stability, debt_penalty, tax_step_up, tax_step_down, tax_min, tax_max;

    // per-thread sums: [thread][market * 2 * GOODS + good] (supply, then demand), [thread][empire]
    std::vector<std::vector<int32_t>> part_goods, part_empire;

    int32_t* f(uint32_t e, const char* name) {
        const int i = game.entities[e].field_index(name);
        if (i < 0) throw std::logic_error(std::string("bench world has no field ") + name);
        return columns[e][size_t(i)].data();
    }

    Impl(const Game& g, const BenchConfig& c, unsigned threads) : game(g), config(c), pool(threads) {
        for (const auto& t : g.tables) {
            columns.push_back(t.columns);
            count.push_back(t.count);
        }
        const uint32_t cap = g.entities[ARMY].capacity;
        alive.assign(cap, 0);
        generation.assign(cap, 0);
        for (uint32_t a = 0; a < count[ARMY]; ++a) alive[a] = 1, generation[a] = 1;
        prev = alive;

        own.resize(g.modifier_keys.size(), std::vector<std::vector<int32_t>>(g.entities.size()));
        for (size_t k = 0; k < g.modifier_keys.size(); ++k) {
            const auto& key = g.modifier_keys[k];
            own[k][key.entity].assign(g.entities[key.entity].capacity, 0);
            for (const auto& step : key.inherit_steps) own[k][step.entity].assign(g.entities[step.entity].capacity, 0);
        }

        int32_t* const defines[] = {&output_per_person, &base_growth, &sat_neutral, &sat_growth, &unrest_decline,
                                    &job_change_chance, &price_adjust, &price_min, &price_max, &unrest_rate,
                                    &garrison_calm, &stability_calm, &revolt_at, &revolt_chance, &army_upkeep,
                                    &admin_people, &base_stability, &debt_penalty, &tax_step_up, &tax_step_down, &tax_min, &tax_max};
        const char* const names[] = {"output_per_person", "base_growth", "sat_neutral", "sat_growth", "unrest_decline",
                                     "job_change_chance", "price_adjust", "price_min", "price_max", "unrest_rate",
                                     "garrison_calm", "stability_calm", "revolt_at", "revolt_chance", "army_upkeep",
                                     "admin_people", "base_stability", "debt_penalty", "tax_step_up", "tax_step_down", "tax_min",
                                     "tax_max"};
        for (size_t i = 0; i < std::size(names); ++i) *defines[i] = g.define(names[i]);

        part_goods.assign(pool.size(), std::vector<int32_t>(size_t(g.entities[MARKET].capacity) * 2 * GOODS));
        part_empire.assign(pool.size(), std::vector<int32_t>(g.entities[EMPIRE].capacity));
    }

    void apply(const HostEvent& ev, int sign) {
        for (const auto& [key, value] : game.bundles[ev.bundle].values) {
            auto& totals = own[key][ev.entity];
            if (!totals.empty())
                totals[ev.row] = static_cast<int32_t>(static_cast<uint32_t>(totals[ev.row]) +
                                                      static_cast<uint32_t>(sign * fx::mul(value, ev.scale)));
        }
    }

    // The host's turn, as GameSim does it: events are added between ticks, and instances
    // whose time is up are removed at the start of the tick.
    void host() {
        host_events(game, config, tick, events);
        for (const auto& ev : events) {
            apply(ev, +1);
            if (ev.duration > 0) pending.push_back({tick + ev.duration, ev});
        }
        size_t kept = 0;
        for (const auto& p : pending) {
            if (p.due <= tick) apply(p.event, -1);
            else pending[kept++] = p;
        }
        pending.resize(kept);
    }

    // Per-thread sums -> the field, over [0, n).
    void combine(std::vector<std::vector<int32_t>>& parts, int32_t* out, uint32_t n) {
        pool.run(n, SUM_GRAIN, [&](uint32_t b, uint32_t e, unsigned) {
            for (uint32_t k = b; k < e; ++k) {
                int32_t sum = 0;
                for (auto& part : parts) {
                    sum += part[k];
                    part[k] = 0;
                }
                out[k] += sum;
            }
        });
    }

    void step() {
        host();
        const uint32_t t = static_cast<uint32_t>(tick);
        const uint32_t empires = count[EMPIRE], markets = count[MARKET], provinces = count[PROVINCE], pops = count[POP];

        int32_t *treasury = f(EMPIRE, "treasury"), *income = f(EMPIRE, "income"), *upkeep = f(EMPIRE, "upkeep"),
                *tax_rate = f(EMPIRE, "tax_rate"), *stability = f(EMPIRE, "stability");
        int32_t *price = f(MARKET, "price"), *supply = f(MARKET, "supply"), *demand = f(MARKET, "demand");
        int32_t *owner = f(PROVINCE, "owner"), *market = f(PROVINCE, "market"), *first_pop = f(PROVINCE, "first_pop"),
                *pop_rows = f(PROVINCE, "pop_rows"), *population = f(PROVINCE, "population"),
                *pwealth = f(PROVINCE, "wealth"), *psatisfaction = f(PROVINCE, "satisfaction"),
                *unrest = f(PROVINCE, "unrest"), *grievance = f(PROVINCE, "grievance"), *garrison = f(PROVINCE, "garrison"), *raise = f(PROVINCE, "raise");
        int32_t *home = f(POP, "home"), *size = f(POP, "size"), *job = f(POP, "job"), *skill = f(POP, "skill"),
                *wealth = f(POP, "wealth"), *satisfaction = f(POP, "satisfaction"), *paid = f(POP, "paid"),
                *made = f(POP, "made");
        int32_t *army_home = f(ARMY, "home"), *strength = f(ARMY, "strength"), *fresh = f(ARMY, "fresh");

        const uint32_t empire_cap = game.entities[EMPIRE].capacity;
        const int output_key = game.modifier_key_index("output"), growth_key = game.modifier_key_index("pop_growth"),
                  stability_key = game.modifier_key_index("stability");
        const int32_t *output_own = own[size_t(output_key)][PROVINCE].data(),
                      *output_up = own[size_t(output_key)][EMPIRE].data(),
                      *growth_own = own[size_t(growth_key)][PROVINCE].data(),
                      *growth_up = own[size_t(growth_key)][EMPIRE].data(),
                      *stability_own = own[size_t(stability_key)][EMPIRE].data();
        // province_mod_<key>(p): own total plus the owner's
        auto mod_output = [&](uint32_t p) {
            const uint32_t e = uint32_t(owner[p]);
            return output_own[p] + (e < empire_cap ? output_up[e] : 0);
        };
        auto mod_growth = [&](uint32_t p) {
            const uint32_t e = uint32_t(owner[p]);
            return growth_own[p] + (e < empire_cap ? growth_up[e] : 0);
        };

        // the fields stages clear at the start of the tick
        std::fill_n(supply, size_t(game.entities[MARKET].capacity) * GOODS, 0);
        std::fill_n(demand, size_t(game.entities[MARKET].capacity) * GOODS, 0);
        std::fill_n(garrison, game.entities[PROVINCE].capacity, 0);
        std::fill_n(upkeep, empire_cap, 0);
        std::fill_n(income, empire_cap, 0);

        // pop_work
        pool.run(pops, POP_GRAIN, [&](uint32_t b, uint32_t e, unsigned) {
            for (uint32_t i = b; i < e; ++i) {
                const uint32_t p = uint32_t(home[i]);
                const uint32_t k = uint32_t(market[p]) * GOODS + uint32_t(job[i]);
                const int32_t bonus = fx::clamp(fx::ONE + mod_output(p), fx::ONE / 10, 4 * fx::ONE);
                const int32_t output = fx::mul(fx::mul(fx::mul(size[i], skill[i]), bonus), output_per_person);
                made[i] = output;
                const int32_t earned = fx::mul(output, price[k]);
                const int32_t tax = fx::mul(earned, tax_rate[owner[p]]);
                paid[i] = tax;
                wealth[i] = fx::clamp(wealth[i] + earned - tax, 0, WEALTH_MAX);
            }
        });

        // pop_consume
        pool.run(pops, POP_GRAIN, [&](uint32_t b, uint32_t e, unsigned) {
            for (uint32_t i = b; i < e; ++i) {
                const uint32_t m = uint32_t(market[home[i]]);
                int32_t cost = 0;
                for (uint32_t g = 0; g < GOODS; ++g) cost += fx::mul(fx::mul(size[i], NEED[g]), price[m * GOODS + g]);
                const int32_t w = wealth[i];
                satisfaction[i] = cost <= w ? fx::ONE : fx::div(w, cost);
                wealth[i] = w - fx::min(w, cost);
            }
        });

        // army_upkeep: few armies add to any one province or empire, so atomics are cheap here
        pool.run(count[ARMY], ARMY_GRAIN, [&](uint32_t b, uint32_t e, unsigned) {
            for (uint32_t a = b; a < e; ++a) {
                if (!alive[a]) continue;
                const uint32_t p = uint32_t(army_home[a]);
                if (fresh[a] != 0) {
                    fresh[a] = 0;
                    strength[a] = 20 + fx::min(population[p] / 1000, 80);
                }
                strength[a] -= 1 + int32_t(rng::roll(a, t, 4) & 3u);
                if (strength[a] <= 0) {
                    alive[a] = 0;
                } else {
                    atomic_add(garrison[p], 1);
                    atomic_add(upkeep[owner[p]], strength[a] * army_upkeep);
                }
            }
        });

        // province_economy: market and empire sums per thread, combined after the stage
        pool.run(provinces, PROVINCE_GRAIN, [&](uint32_t b, uint32_t e, unsigned th) {
            int32_t* goods = part_goods[th].data();  // supply then demand, per market
            int32_t* taxes = part_empire[th].data();
            for (uint32_t p = b; p < e; ++p) {
                const uint32_t first = uint32_t(first_pop[p]), rows = uint32_t(pop_rows[p]);
                int32_t people = 0, taxed = 0, mood_sum = 0, offered[GOODS] = {}, wanted[GOODS] = {};
                int64_t riches = 0;
                for (uint32_t i = first; i < first + rows; ++i) {
                    people += fx::to_agg(size[i]);
                    taxed += fx::to_agg(paid[i]);
                    mood_sum += satisfaction[i];
                    riches = add64(riches, wealth[i]);
                    offered[job[i]] += fx::to_agg(made[i]);
                    for (uint32_t g = 0; g < GOODS; ++g) wanted[g] += fx::to_agg(fx::mul(size[i], NEED[g]));
                }
                const uint32_t k = uint32_t(market[p]) * 2 * GOODS;
                for (uint32_t g = 0; g < GOODS; ++g) {
                    goods[k + g] += offered[g];
                    goods[k + GOODS + g] += wanted[g];
                }
                const int32_t mood = rows == 0 ? 0 : mood_sum / int32_t(rows);
                const uint32_t em = uint32_t(owner[p]);
                population[p] = people;
                set64(pwealth, p, riches);
                psatisfaction[p] = mood;
                taxes[em] += taxed;
                atomic_add(upkeep[em], people / admin_people);
                const int32_t u = unrest[p] + grievance[p] + fx::mul(sat_neutral - mood, unrest_rate) -
                                  garrison[p] * garrison_calm - fx::mul(stability[em], stability_calm);
                unrest[p] = fx::clamp(u, 0, fx::ONE);
                if (unrest[p] > revolt_at && rng::below(rng::roll(p, t, 3), revolt_chance))
                    raise[p] = int32_t(uint32_t(raise[p]) + 1u);
            }
        });
        combine(part_empire, income, empires);
        pool.run(markets, MARKET_GRAIN, [&](uint32_t b, uint32_t e, unsigned) {
            for (uint32_t m = b; m < e; ++m)
                for (uint32_t g = 0; g < GOODS; ++g) {
                    int32_t offered = 0, wanted = 0;
                    for (auto& part : part_goods) {
                        int32_t* goods = part.data() + size_t(m) * 2 * GOODS;
                        offered += goods[g];
                        wanted += goods[GOODS + g];
                        goods[g] = goods[GOODS + g] = 0;
                    }
                    supply[m * GOODS + g] += offered;
                    demand[m * GOODS + g] += wanted;
                }
        });

        // pop_growth
        pool.run(pops, POP_GRAIN, [&](uint32_t b, uint32_t e, unsigned) {
            for (uint32_t i = b; i < e; ++i) {
                const uint32_t p = uint32_t(home[i]);
                const int32_t growth = base_growth + fx::mul(satisfaction[i] - sat_neutral, sat_growth) + mod_growth(p) -
                                       fx::mul(unrest[p], unrest_decline);
                size[i] = fx::clamp(size[i] + fx::mul(size[i], growth), POP_MIN, POP_MAX);
                if (satisfaction[i] < sat_neutral && rng::below(rng::roll(i, t, 1), job_change_chance)) {
                    const uint32_t m = uint32_t(market[p]) * GOODS, now = uint32_t(job[i]);
                    const uint32_t other = (now + 1u + rng::roll(i, t, 2) % (GOODS - 1u)) % GOODS;
                    if (price[m + other] > price[m + now]) job[i] = int32_t(other);
                }
            }
        });

        // market_prices
        pool.run(markets, MARKET_GRAIN, [&](uint32_t b, uint32_t e, unsigned) {
            for (uint32_t m = b; m < e; ++m)
                for (uint32_t g = 0; g < GOODS; ++g) {
                    const uint32_t k = m * GOODS + g;
                    const int32_t s = supply[k], d = demand[k];
                    const int32_t ratio = s <= 0 || d / s >= 4 ? 4 * fx::ONE : fx::div(d, s);
                    const int32_t target = fx::mul(BASE_PRICE[g], ratio);
                    price[k] = fx::clamp(price[k] + fx::mul(target - price[k], price_adjust), price_min, price_max);
                }
        });

        // empire_budget and, once a month, empire_policy: too few empires to share out
        for (uint32_t e = 0; e < empires; ++e) {
            const int64_t money = add64(get64(treasury, e), income[e] - upkeep[e]);
            set64(treasury, e, money);
            const int32_t debt = money < 0 ? debt_penalty : 0;
            stability[e] = fx::clamp(base_stability + stability_own[e] - debt, 0, fx::ONE);
        }
        if (tick % 30 == 29)
            for (uint32_t e = 0; e < empires; ++e) {
                const int32_t delta = get64(treasury, e) < 0 ? tax_step_up : -tax_step_down;
                tax_rate[e] = fx::clamp(tax_rate[e] + delta, tax_min, tax_max);
            }

        ++tick;
        spawn(raise, army_home);
    }

    // The end of the tick: each province, in row order, gets the armies it asked for, each
    // the lowest row that is not alive now and was not alive when the last tick ended
    // (a destroyed row rests for a tick). Requests that do not fit wait.
    void spawn(int32_t* raise, int32_t* army_home) {
        const uint32_t cap = game.entities[ARMY].capacity;
        free_rows.clear();
        for (uint32_t a = 0; a < cap; ++a) {
            if (!alive[a] && !prev[a]) free_rows.push_back(a);
            prev[a] = alive[a];
        }
        const auto& fields = game.entities[ARMY].fields;
        size_t next = 0;
        for (uint32_t p = 0; p < count[PROVINCE] && next < free_rows.size(); ++p)
            for (; raise[p] != 0 && next < free_rows.size(); raise[p] = int32_t(uint32_t(raise[p]) - 1u)) {
                const uint32_t a = free_rows[next++];
                alive[a] = prev[a] = 1;
                ++generation[a];
                count[ARMY] = std::max(count[ARMY], a + 1);
                for (size_t fi = 0; fi < fields.size(); ++fi) columns[ARMY][fi][a] = int32_t(fields[fi].default_raw);
                army_home[a] = int32_t(p);
            }
    }
    std::vector<uint32_t> free_rows;
};

CpuWorld::CpuWorld(const Game& game, const BenchConfig& config, unsigned threads)
    : impl_(std::make_unique<Impl>(game, config, std::max(1u, threads))) {}
CpuWorld::~CpuWorld() = default;
void CpuWorld::tick() { impl_->step(); }
uint64_t CpuWorld::ticks() const { return impl_->tick; }
unsigned CpuWorld::threads() const { return impl_->pool.size(); }

StateHashes CpuWorld::state_hashes() const {
    const Impl& m = *impl_;
    StateHashes s;
    for (uint32_t e = 0; e < m.game.entities.size(); ++e) {
        const EntityDef& def = m.game.entities[e];
        for (size_t fi = 0; fi < def.fields.size(); ++fi) {
            const FieldDef& fd = def.fields[fi];
            s.fields.emplace_back(def.name + "." + fd.name,
                                  hash_words(m.columns[e][fi].data(), size_t(m.count[e]) * fd.size * fd.words()));
        }
        if (def.dynamic) {
            std::vector<int32_t> alive(m.count[e]), generation(m.count[e]);
            for (uint32_t r = 0; r < m.count[e]; ++r) {
                alive[r] = m.alive[r];
                generation[r] = static_cast<int32_t>(m.generation[r]);
            }
            s.fields.emplace_back(def.name + ".alive", hash_words(alive.data(), alive.size()));
            s.fields.emplace_back(def.name + ".generation", hash_words(generation.data(), generation.size()));
        }
    }
    const int32_t tick[2] = {static_cast<int32_t>(m.tick), static_cast<int32_t>(m.tick >> 32)};
    s.fields.emplace_back("tick", hash_words(tick, 2));
    return s;
}

}  // namespace terni::bench
