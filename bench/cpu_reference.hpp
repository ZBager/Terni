// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// The benchmark world on the CPU, written the way a careful CPU engine would write it:
// flat arrays in the same layout as Terni's, no allocation per entity, terni::fx and
// terni::rng for the arithmetic, and a pool of threads created once, each stage's rows
// split statically between them. Sums that many rows add to one value are kept per
// thread and combined after the stage. After every tick its state hash equals Terni's.
//
// It is a fair baseline, not the fastest CPU code possible: it runs the same stages in
// the same order (no fusing of loops), and has no hand-written SIMD.

#include <cstdint>
#include <memory>

#include "bench_world.hpp"

namespace terni::bench {

class CpuWorld {
public:
    // Starts from the state `game` was built with. `threads` >= 1; the calling thread is one of them.
    CpuWorld(const Game& game, const BenchConfig& config, unsigned threads);
    ~CpuWorld();
    CpuWorld(const CpuWorld&) = delete;
    CpuWorld& operator=(const CpuWorld&) = delete;

    void tick();  // the host's events (host_events), then every stage, then the armies asked for
    uint64_t ticks() const;
    unsigned threads() const;
    StateHashes state_hashes() const;  // laid out like bench::state_hashes(GameSim&, ...)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terni::bench
