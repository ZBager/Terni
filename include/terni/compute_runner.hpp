// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#pragma once
// Model-independent Vulkan compute runner.
//
// A simulation model describes itself as
//   - a list of storage buffers  (binding = index in the list)
//   - a list of stages           (one compute shader each, run in order every tick,
//                                 separated by memory barriers)
//   - which buffers are zeroed at the start of every tick (atomic accumulators), or
//     of every tick in which a given stage runs
// and the runner does the rest: device, upload, descriptors, pipelines, pre-recorded
// command buffers (one per set of stages that run together), GPU timestamps and readback.
//

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace terni {

struct ComputeBufferSpec {
    std::string name;
    const void* data;    // initial contents (uploaded once)
    uint64_t bytes;
};

// A specialization constant given to every shader (layout(constant_id = id) in GLSL).
// Ids 0 and 1 are the runner's own (shared reduction, order check).
struct ComputeSpecConstant {
    uint32_t id, value;
};

struct ComputeStageSpec {
    std::string shader;            // name; also the SPIR-V file name (without .spv) when `spirv` is empty
    uint32_t elements;             // threads to launch (grid-stride loops handle any count)
    std::vector<uint32_t> spirv;   // in-memory SPIR-V (e.g. compiled at load time); overrides the file
    std::vector<uint32_t> zero;    // buffers zeroed at the start of every tick in which this stage runs
};

class ComputeRunner {
public:
    // shared_reduction is passed to every shader as specialization constant 0, and
    // `constants` (ids from 2 up) as given.
    ComputeRunner(uint32_t device_index, bool validation, const std::string& shader_dir,
                  const std::vector<ComputeBufferSpec>& buffers, const std::vector<ComputeStageSpec>& stages,
                  const std::vector<uint32_t>& zero_each_tick, bool shared_reduction,
                  const std::vector<ComputeSpecConstant>& constants = {});
    ~ComputeRunner();
    ComputeRunner(const ComputeRunner&) = delete;
    ComputeRunner& operator=(const ComputeRunner&) = delete;

    // Submit one tick and wait. `run` has one entry per stage: false skips the stage (and
    // its `zero` buffers) in this tick; empty runs every stage.
    void tick(const std::vector<bool>& run = {});
    // ---- a tick that does not block ----
    // tick() is tick_submit() then tick_wait(). Between the two the host may keep working:
    // queue_write() goes to the next tick, and tick_readback() still gives the last
    // finished tick's results. read(), write(), flush_queued(), add_tick_readback() and
    // tick_checked() wait for a tick in flight first.
    void tick_submit(const std::vector<bool>& run = {});  // a tick still in flight is finished first
    bool tick_done();                                      // never blocks
    void tick_wait();                                      // blocks; nothing to do if none is in flight
    bool tick_in_flight() const;                           // submitted and not yet waited for
    // Copy `bytes` of buffer `binding`, starting at byte `offset`, back to the host.
    void read(uint32_t binding, void* dst, uint64_t bytes, uint64_t offset = 0);
    // Copy host data into buffer `binding` at byte `offset` (between ticks).
    void write(uint32_t binding, const void* src, uint64_t bytes, uint64_t offset = 0);

    // ---- batched transfers: ride in the next tick's single GPU submission ----
    // Hosts that feed the simulation every tick and read results every tick pay one
    // GPU round trip per tick instead of one per write/read.
    //
    // Queue host data for buffer `binding`; copied in before the tick's first stage.
    // `src` is copied immediately (the caller may reuse it).
    void queue_write(uint32_t binding, const void* src, uint64_t bytes, uint64_t offset = 0);
    void flush_queued();    // apply queued writes now (e.g. before saving)
    void discard_queued();  // drop queued writes (e.g. when loading a save)
    // Move the writes queued on `from` (a runner with the same buffers) to this one, in
    // their order: for a host that carries on with another runner.
    void take_queued(ComputeRunner& from);
    // A buffer range copied back to the host after every tick's last stage; returns an id.
    // `bytes` is the most it will ever copy (host memory is reserved for that much).
    uint32_t add_tick_readback(uint32_t binding, uint64_t bytes, uint64_t offset = 0);
    // Copy only the first `bytes` (<= the registered size; 0 = nothing) from the next tick on.
    void set_tick_readback_bytes(uint32_t id, uint64_t bytes);
    uint64_t tick_readback_bytes(uint32_t id) const;  // bytes it copies now
    // Host copy of a registered range as of the last finished tick (tick_wait()); valid
    // until the tick after the next one is submitted.
    const void* tick_readback(uint32_t id) const;
    // Set that host copy, as if the last tick had brought back `src` (the first `bytes` of
    // the range): for a host that carries results over from another runner.
    void set_tick_readback(uint32_t id, const void* src, uint64_t bytes);

    // ---- order check (tests) ----
    // One tick in which every stage with check[s] set runs three times from the state
    // before it: with the order-check pipeline (one workgroup visiting the items one at a
    // time) forwards, then backwards, then normally, which is the result kept. `run` as in
    // tick(). Reports the buffers whose words differ between the forward run and the backward run
    // (parallel = false) or the normal run (parallel = true), in rows of row_words[b] words.
    struct OrderDiff {
        uint32_t stage, binding;
        bool parallel;
        uint64_t first_row, rows;
    };
    std::vector<OrderDiff> tick_checked(const std::vector<bool>& check, const std::vector<uint64_t>& row_words,
                                        const std::vector<bool>& run = {});
    uint64_t sync_reads() const;  // read() calls so far; each waits for the GPU

    std::string device_name() const;
    std::string report() const;  // upload time, memory, GPU time per stage
    double last_tick_ms() const;      // wall time of the last tick incl. batched transfers, from submission
                                      // until it was seen done (tick_done() or the wait)
    double last_tick_gpu_ms() const;  // GPU time of the last tick's stages (timestamps; 0 if unsupported)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terni
