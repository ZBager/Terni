// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// The benchmark world: empires, provinces, pops, markets with several goods, and armies
// that come and go, built in code from a size and a seed. terni_bench runs it on Vulkan
// devices with GameSim and on the CPU with CpuWorld (cpu_reference.hpp); both must end
// with the same state hash.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "terni/fixed.hpp"
#include "terni/game.hpp"
#include "terni/state_hash.hpp"

namespace terni::bench {

// Changes whenever the world, its rules or the host's events change: results are
// comparable only within one version (and one size).
constexpr uint32_t BENCH_VERSION = 1;

constexpr uint32_t GOODS = 8;

// Constants the rules share with the CPU reference (raw fixed point). The rules get them
// as GLSL constants generated from these, so the two cannot disagree.
constexpr int32_t BASE_PRICE[GOODS] = {fx::rate(1), fx::rate(3, 2), fx::rate(2), fx::rate(4, 5),
                                       fx::rate(3), fx::rate(5), fx::rate(6, 5), fx::rate(8)};
constexpr int32_t NEED[GOODS] = {fx::rate(4, 1000), fx::rate(2, 1000), fx::rate(1, 1000), fx::rate(3, 1000),
                                 fx::rate(5, 10000), fx::rate(2, 10000), fx::rate(1, 1000), fx::rate(1, 10000)};
constexpr int32_t POP_MIN = fx::qty(10), POP_MAX = fx::qty(20000), WEALTH_MAX = fx::qty(100000);

struct BenchConfig {
    std::string name = "custom";
    uint32_t provinces = 0;
    uint32_t pops_per_province = 0;
    uint32_t empires = 0;
    uint32_t markets = 0;
    uint32_t seed = 1;
    uint32_t ticks = 100, warmup = 20;  // terni_bench's defaults for this size

    uint64_t pops() const { return uint64_t(provinces) * pops_per_province; }
    uint32_t army_capacity() const { return provinces / 4 > 16 ? provinces / 4 : 16; }
    std::string check() const;  // "" if the sizes can be built, else why not
};

// The named sizes: tiny (the smoke test), small, medium, large, and huge, giant and colossal
// (10, 50 and 100 million pops) to find the limits of a machine.
std::vector<BenchConfig> standard_sizes();
bool find_size(const std::string& name, BenchConfig& out);

// Entity type indices in the built Game.
enum Entity : uint32_t { EMPIRE = 0, MARKET = 1, PROVINCE = 2, POP = 3, ARMY = 4 };

std::unique_ptr<Game> build_world(const BenchConfig& config);

// What the host does before a tick: modifiers for events (harvests, famines, reforms),
// the same on every run. `tick` is the number of completed ticks.
struct HostEvent {
    uint32_t bundle = 0;
    uint32_t entity = 0;
    uint32_t row = 0;
    int32_t scale = 1 << 16;
    uint32_t duration = 0;
};
void host_events(const Game& game, const BenchConfig& config, uint64_t tick, std::vector<HostEvent>& out);

// The state compared between runs, as one hash per field: every field over the rows in
// use, then which army rows are alive and their generations, then the tick. Built the same
// way from a GameSim and from a CpuWorld. Hashes, not copies, so that runs of the largest
// worlds do not each keep gigabytes of state.
struct StateHashes {
    std::vector<std::pair<std::string, uint64_t>> fields;
    uint64_t combined() const;
};
StateHashes state_hashes(GameSim& sim, const Game& game);
std::string first_difference(const StateHashes& a, const StateHashes& b);  // field name, "" if none

// terni::hash_values() of the words, each sign-extended, without copying them.
uint64_t hash_words(const int32_t* words, size_t n);

}  // namespace terni::bench
