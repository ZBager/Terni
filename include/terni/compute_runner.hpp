// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#pragma once
// Model-independent Vulkan compute runner.
//
// A simulation model describes itself as
//   - a list of storage buffers  (binding = index in the list)
//   - a list of stages           (one compute shader each, run in order every tick,
//                                 separated by memory barriers)
//   - which buffers are zeroed at the start of every tick (atomic accumulators)
// and the runner does the rest: device, upload, descriptors, pipelines, one
// pre-recorded command buffer per tick, GPU timestamps and readback.
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

struct ComputeStageSpec {
    std::string shader;            // name; also the SPIR-V file name (without .spv) when `spirv` is empty
    uint32_t elements;             // threads to launch (grid-stride loops handle any count)
    std::vector<uint32_t> spirv;   // in-memory SPIR-V (e.g. compiled at load time); overrides the file
};

class ComputeRunner {
public:
    // shared_reduction is passed to every shader as specialization constant 0.
    ComputeRunner(uint32_t device_index, bool validation, const std::string& shader_dir,
                  const std::vector<ComputeBufferSpec>& buffers, const std::vector<ComputeStageSpec>& stages,
                  const std::vector<uint32_t>& zero_each_tick, bool shared_reduction);
    ~ComputeRunner();
    ComputeRunner(const ComputeRunner&) = delete;
    ComputeRunner& operator=(const ComputeRunner&) = delete;

    void tick();  // submit one tick and wait
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
    // A buffer range copied back to the host after every tick's last stage; returns an id.
    // `bytes` is the most it will ever copy (host memory is reserved for that much).
    uint32_t add_tick_readback(uint32_t binding, uint64_t bytes, uint64_t offset = 0);
    // Copy only the first `bytes` (<= the registered size; 0 = nothing) from the next tick on.
    void set_tick_readback_bytes(uint32_t id, uint64_t bytes);
    uint64_t tick_readback_bytes(uint32_t id) const;  // bytes it copies now
    // Host copy of a registered range as of the last completed tick.
    const void* tick_readback(uint32_t id) const;

    // ---- order check (tests) ----
    // One tick in which every stage with check[s] set runs three times from the state
    // before it: with the order-check pipeline (one workgroup visiting the items one at a
    // time) forwards, then backwards, then normally, which is the result kept. Reports
    // the buffers whose words differ between the forward run and the backward run
    // (parallel = false) or the normal run (parallel = true), in rows of row_words[b] words.
    struct OrderDiff {
        uint32_t stage, binding;
        bool parallel;
        uint64_t first_row, rows;
    };
    std::vector<OrderDiff> tick_checked(const std::vector<bool>& check, const std::vector<uint64_t>& row_words);
    uint64_t sync_reads() const;  // read() calls so far; each waits for the GPU

    std::string device_name() const;
    std::string report() const;  // upload time, memory, GPU time per stage
    double last_tick_ms() const;      // wall time of the last tick() incl. batched transfers
    double last_tick_gpu_ms() const;  // GPU time of the last tick's stages (timestamps; 0 if unsupported)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terni
