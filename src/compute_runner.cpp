// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#include "terni/compute_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

#include "terni/vk_context.hpp"
#include "device_clock.hpp"

namespace terni {

namespace {

constexpr uint32_t WORKGROUP_SIZE = 256;  // local_size_x in shaders/terni/compute.glsl
constexpr uint64_t STAGING_CHUNK = 256ull << 20;  // the first upload, and the most queue_write() stages, are in 256 MiB pieces
constexpr uint64_t TRANSFER_CHUNK = 8ull << 20;   // read() and write() in pieces that stay in the CPU's cache

constexpr VkPipelineStageFlags2 COMPUTE = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 TRANSFER = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr VkAccessFlags2 SHADER_RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
constexpr VkAccessFlags2 TRANSFER_RW = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src_stage;
    mb.srcAccessMask = src_access;
    mb.dstStageMask = dst_stage;
    mb.dstAccessMask = dst_access;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

}  // namespace

struct ComputeRunner::Impl {
    VulkanContext ctx;
    std::vector<ComputeStageSpec> stages;
    std::vector<GpuBuffer> buffers;
    GpuBuffer readback;
    GpuBuffer upload;  // lazily created for write()
    // batched transfers
    GpuBuffer staging;                                         // queued writes, grows on demand
    GpuBuffer staging_busy;  // the staging of the tick in flight; the two swap at every submission
    uint64_t staged = 0, staging_limit = 0;  // bytes queued, and the most that may be
    std::vector<std::pair<uint32_t, VkBufferCopy>> staged_copies;  // (binding, staging -> buffer)
    struct TickReadback { uint32_t binding; uint64_t offset, bytes, at, size; };  // size: bytes copied (<= bytes)
    std::vector<TickReadback> tick_reads;
    // Host-visible destinations of the tick readbacks, two so that the last finished
    // tick's results stay readable while the next tick writes the other one.
    GpuBuffer tick_rb[2];
    int rb_done = 0;  // which one holds the last finished tick
    // Ones that larger buffers replaced: hosts may still read the results in them, for as
    // many ticks as tick_readback() promises.
    struct Retired { GpuBuffer buffer; int ticks; };
    std::vector<Retired> retired_rb;
    void age_retired() {  // a tick begins
        std::erase_if(retired_rb, [&](Retired& r) {
            if (--r.ticks > 0) return false;
            ctx.destroy_buffer(r.buffer);
            return true;
        });
    }
    // The copies into each of them, recorded when first needed after `tick_reads` or the
    // destinations change (readbacks_changed) and resubmitted until then.
    VkCommandBuffer readback_cmds[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool readbacks_changed = false;
    // The submitted tick (tick_submit), until finish() accounts for it.
    struct Flight {
        bool active = false, seen_done = false;
        VkCommandBuffer pre = VK_NULL_HANDLE;
        std::vector<bool> run;  // the stages it runs
        bool ends_late = false;  // its last timestamp is at the end of its readbacks (query S + 1), not of its stages
        std::chrono::steady_clock::time_point submitted, done;
    } flight;
    detail::DeviceClock clock;
    uint32_t spin_us = 0;  // set_tick_spin()
    double last_tick_ms = 0;
    double last_gpu_ms = 0;  // stages only, from GPU timestamps
    double last_device_ms = 0;  // submission to the end of the tick, as the device saw it
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkPipeline> pipelines;
    std::vector<VkPipeline> extras;  // add_extra()
    VkQueryPool queries = VK_NULL_HANDLE;
    std::map<std::vector<bool>, VkCommandBuffer> tick_cmds;  // recorded ticks, by which stages run
    std::vector<ComputeRunner::StageTiming> timings;  // per stage
    std::vector<double> stage_ns;                      // the last tick's, from the timestamps
    uint64_t ticks = 0;
    double upload_ms = 0;
    uint64_t device_bytes = 0;
    uint64_t sync_reads = 0;
    std::string shader_dir;
    VkBool32 shared_reduction = VK_FALSE;
    std::vector<uint32_t> zero_each_tick;
    std::vector<std::vector<uint32_t>> clears;
    std::vector<ComputeSpecConstant> constants;
    // order check (tick_checked), made on first use
    std::vector<VkPipeline> check_pipelines;  // TERNI_ORDER_CHECK = 1
    std::vector<GpuBuffer> backup;            // the state before a checked stage
    GpuBuffer check_rb;                       // host-visible copy of every buffer

    Impl(uint32_t device, bool validation) : ctx(device, validation) {}

    uint32_t groups(uint32_t n) const {
        const uint32_t needed = (n + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
        return std::max(1u, std::min(needed, ctx.properties().limits.maxComputeWorkGroupCount[0]));
    }

    // One pipeline per stage. Specialization constants: 0 = USE_SHARED_REDUCTION,
    // 1 = TERNI_ORDER_CHECK (shaders/terni/compute.glsl), then the caller's `constants`.
    // Pipelines for `stages` (or the given SPIR-V instead), all with the same constants.
    std::vector<VkPipeline> make_pipelines(uint32_t order_check, const std::vector<std::vector<uint32_t>>* spirv = nullptr) {
        VkDevice d = ctx.device();
        std::vector<uint32_t> data{shared_reduction, order_check};  // VkBool32 is a 32-bit word too
        std::vector<VkSpecializationMapEntry> entries{{0, 0, 4}, {1, 4, 4}};
        for (const auto& c : constants) {
            entries.push_back({c.id, static_cast<uint32_t>(data.size() * 4), 4});
            data.push_back(c.value);
        }
        const VkSpecializationInfo spec{static_cast<uint32_t>(entries.size()), entries.data(), data.size() * 4,
                                        data.data()};
        const size_t S = spirv ? spirv->size() : stages.size();
        std::vector<VkShaderModule> modules(S);
        std::vector<VkComputePipelineCreateInfo> cis(S);
        for (size_t s = 0; s < S; ++s) {
            modules[s] = spirv                    ? ctx.create_shader((*spirv)[s])
                         : stages[s].spirv.empty() ? ctx.load_shader(shader_dir + "/" + stages[s].shader + ".spv")
                                                   : ctx.create_shader(stages[s].spirv);
            cis[s] = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cis[s].stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cis[s].stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cis[s].stage.module = modules[s];
            cis[s].stage.pName = "main";
            cis[s].stage.pSpecializationInfo = &spec;
            cis[s].layout = layout;
        }
        std::vector<VkPipeline> out(S);
        VK_CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, static_cast<uint32_t>(S), cis.data(), nullptr, out.data()));
        for (VkShaderModule mod : modules) vkDestroyShaderModule(d, mod, nullptr);
        return out;
    }

    // Buffers to zero at the start of a tick in which the stages in `run` run.
    std::vector<uint32_t> zeroed(const std::vector<bool>& run) const {
        std::vector<uint32_t> out = zero_each_tick;
        for (size_t s = 0; s < stages.size(); ++s)
            if (run[s]) out.insert(out.end(), stages[s].zero.begin(), stages[s].zero.end());
        for (size_t c = 0; c < clears.size(); ++c)
            if (run[stages.size() + c]) out.insert(out.end(), clears[c].begin(), clears[c].end());
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // The tick in which the stages in `run` run, recorded once and resubmitted. A skipped
    // stage still writes its timestamp, so it measures as zero.
    VkCommandBuffer tick_cmd(const std::vector<bool>& run) {
        if (auto it = tick_cmds.find(run); it != tick_cmds.end()) return it->second;
        if (tick_cmds.size() >= 64) {  // many co-prime schedules: start over rather than grow (none is in flight)
            for (const auto& [r, c] : tick_cmds) ctx.free_command_buffer(c);
            tick_cmds.clear();
        }
        const size_t S = stages.size();
        VkCommandBuffer cmd = ctx.allocate_command_buffer();
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        barrier(cmd, COMPUTE | TRANSFER, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                COMPUTE | TRANSFER, SHADER_RW | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (queries) {
            vkCmdResetQueryPool(cmd, queries, 0, static_cast<uint32_t>(S + 2));
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
        }
        for (uint32_t b : zeroed(run)) vkCmdFillBuffer(cmd, buffers[b].buffer, 0, VK_WHOLE_SIZE, 0);
        barrier(cmd, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, COMPUTE, SHADER_RW);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        const uint32_t forwards = 0;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(forwards), &forwards);
        for (size_t s = 0; s < S; ++s) {
            if (run[s]) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[s]);
                vkCmdDispatch(cmd, groups(stages[s].elements), 1, 1);
                barrier(cmd, COMPUTE, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, COMPUTE, SHADER_RW);
            }
            if (queries)
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, static_cast<uint32_t>(s + 1));
        }
        VK_CHECK(vkEndCommandBuffer(cmd));
        tick_cmds.emplace(run, cmd);
        return cmd;
    }

    std::vector<bool> run_mask(const std::vector<bool>& run) const {
        const size_t n = stages.size() + clears.size();
        if (run.empty()) return std::vector<bool>(n, true);
        if (run.size() != n) throw std::invalid_argument("tick: one run entry per stage and list of clears");
        return run;
    }

    // The queued host writes, in the order they were queued. Copies to one buffer go out as
    // one command; a write that overlaps an earlier one of the same batch waits for it
    // behind a barrier, so the later write wins (copies without one may run in any order).
    void record_staged(VkCommandBuffer cmd) {
        std::vector<std::vector<VkBufferCopy>> regions(buffers.size());
        std::vector<std::map<uint64_t, uint64_t>> written(buffers.size());  // disjoint [lo, hi) per buffer
        std::vector<uint32_t> touched;
        const auto emit = [&] {
            for (uint32_t b : touched) {
                vkCmdCopyBuffer(cmd, staging.buffer, buffers[b].buffer, static_cast<uint32_t>(regions[b].size()),
                                regions[b].data());
                regions[b].clear();
                written[b].clear();
            }
            touched.clear();
        };
        for (const auto& [binding, region] : staged_copies) {
            auto& w = written[binding];
            const uint64_t lo = region.dstOffset, hi = lo + region.size;
            auto next = w.lower_bound(hi);  // first range starting at or after hi: no overlap from here on
            if (next != w.begin() && std::prev(next)->second > lo) {
                emit();
                barrier(cmd, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            }
            if (regions[binding].empty()) touched.push_back(binding);
            regions[binding].push_back(region);
            written[binding].emplace(lo, hi);
        }
        emit();
    }

    // Wait for the tick in flight, if any, and account for it: free its command buffers,
    // its time and timestamps, and make its readbacks the current ones.
    void finish() {
        if (!flight.active) return;
        ctx.wait_submitted(spin_us);
        if (!flight.seen_done) flight.done = std::chrono::steady_clock::now();
        if (flight.pre) ctx.free_command_buffer(flight.pre);
        last_tick_ms = std::chrono::duration<double, std::milli>(flight.done - flight.submitted).count();
        ++ticks;
        // What the device did, whenever the host looked: from the submission, on the device's
        // clock, to the tick's last timestamp. Without that clock, the wall time if the host waited
        // for the tick (it is seen done at once), or the stages' GPU time if it polled.
        last_device_ms = last_tick_ms;
        if (queries) {
            const size_t S = stages.size(), n = flight.ends_late ? S + 2 : S + 1;
            std::vector<uint64_t> ts(n);
            VK_CHECK(vkGetQueryPoolResults(ctx.device(), queries, 0, static_cast<uint32_t>(n),
                                           ts.size() * sizeof(uint64_t), ts.data(), sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            const double period = ctx.properties().limits.timestampPeriod;
            const uint32_t bits = ctx.timestamp_valid_bits();
            const uint64_t mask = bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1;
            for (size_t s = 0; s < S; ++s) stage_ns[s] = double((ts[s + 1] - ts[s]) & mask) * period;
            last_gpu_ms = double((ts[S] - ts[0]) & mask) * period / 1e6;
            if (clock.known())
                last_device_ms = clock.ms(flight.submitted, ts[n - 1], mask, period, last_gpu_ms, last_tick_ms);
            else if (flight.seen_done)
                last_device_ms = last_gpu_ms;
        }
        for (size_t s = 0; s < timings.size(); ++s) {
            auto& t = timings[s];
            t.ran = flight.run[s];
            t.last_ms = t.ran && queries ? stage_ns[s] / 1e6 : 0;  // a skipped stage's timestamps differ by noise
            t.runs += t.ran;
            t.total_ms += t.last_ms;
        }
        rb_done ^= 1;
        flight = {};
    }

    // Copies of the tick readbacks into tick_rb[rb_done ^ 1], recorded into `cmd`.
    void record_readbacks(VkCommandBuffer cmd) {
        barrier(cmd, COMPUTE, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, TRANSFER, VK_ACCESS_2_TRANSFER_READ_BIT);
        for (const auto& r : tick_reads) {
            if (r.size == 0) continue;  // a zero-sized copy is invalid in Vulkan
            const VkBufferCopy region{r.offset, r.at, r.size};
            vkCmdCopyBuffer(cmd, buffers[r.binding].buffer, tick_rb[rb_done ^ 1].buffer, 1, &region);
        }
        barrier(cmd, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
    // The copies of the tick about to be submitted (none is in flight).
    VkCommandBuffer readback_cmd() {
        if (readbacks_changed) {
            for (VkCommandBuffer& c : readback_cmds)
                if (c) ctx.free_command_buffer(c), c = VK_NULL_HANDLE;
            readbacks_changed = false;
        }
        VkCommandBuffer& cmd = readback_cmds[rb_done ^ 1];
        if (cmd) return cmd;
        cmd = ctx.allocate_command_buffer();
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        record_readbacks(cmd);
        if (queries)  // the tick ends when its results are back
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, static_cast<uint32_t>(stages.size() + 1));
        VK_CHECK(vkEndCommandBuffer(cmd));
        return cmd;
    }
    bool any_readback() const {
        return std::any_of(tick_reads.begin(), tick_reads.end(), [](const auto& r) { return r.size > 0; });
    }

    // Record a one-off command buffer, submit it and wait.
    template <class F>
    void run_once(F&& record) {
        VkCommandBuffer cmd = ctx.allocate_command_buffer();
        VkCommandBufferBeginInfo once{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        once.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &once));
        record(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));
        ctx.submit_and_wait(cmd);
        ctx.free_command_buffer(cmd);
    }
};

ComputeRunner::ComputeRunner(uint32_t device_index, bool validation, const std::string& shader_dir,
                             const std::vector<ComputeBufferSpec>& specs, const std::vector<ComputeStageSpec>& stages,
                             const std::vector<uint32_t>& zero_each_tick, bool shared_reduction,
                             const std::vector<ComputeSpecConstant>& constants,
                             const std::vector<std::vector<uint32_t>>& clears, bool small_fields)
    : impl_(std::make_unique<Impl>(device_index, validation)) {
    Impl& m = *impl_;
    VulkanContext& ctx = m.ctx;
    VkDevice d = ctx.device();
    const uint32_t n = static_cast<uint32_t>(specs.size());
    m.stages = stages;
    m.shader_dir = shader_dir;
    m.shared_reduction = shared_reduction ? VK_TRUE : VK_FALSE;
    m.zero_each_tick = zero_each_tick;
    m.clears = clears;
    for (const auto& c : constants)
        if (c.id < 2) throw std::invalid_argument("ComputeRunner: specialization constants 0 and 1 are the runner's own");
    m.constants = constants;
    if (small_fields && !ctx.small_fields())
        throw std::runtime_error("device '" + ctx.device_name() +
                                 "' cannot store 8 and 16-bit values, which this game's small fields need");
    if (ctx.properties().limits.maxPerStageDescriptorStorageBuffers < n)
        throw std::runtime_error("device supports too few storage buffers per shader stage");

    // ---- buffers + upload through a reused 256 MiB staging buffer
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t total = 0;
    const uint64_t max_range = ctx.properties().limits.maxStorageBufferRange;
    for (const auto& s : specs) {
        const uint64_t bytes = std::max<uint64_t>(s.bytes, 4);  // Vulkan forbids empty buffers
        if (bytes > max_range)
            throw std::runtime_error("buffer '" + s.name + "' (" + std::to_string(bytes >> 20) +
                                     " MiB) exceeds maxStorageBufferRange");
        m.buffers.push_back(ctx.create_buffer(bytes,
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        total += bytes;
    }
    m.device_bytes = total;
    const uint64_t chunk = std::min(total, STAGING_CHUNK);
    GpuBuffer staging = ctx.create_buffer(chunk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t b = 0; b < n; ++b) {
        for (uint64_t off = 0; off < specs[b].bytes; off += chunk) {
            const uint64_t len = std::min(chunk, specs[b].bytes - off);
            std::memcpy(staging.mapped, static_cast<const char*>(specs[b].data) + off, len);
            VkCommandBuffer cmd = ctx.allocate_command_buffer();
            VkCommandBufferBeginInfo once{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            once.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &once));
            VkBufferCopy region{0, off, len};
            vkCmdCopyBuffer(cmd, staging.buffer, m.buffers[b].buffer, 1, &region);
            VK_CHECK(vkEndCommandBuffer(cmd));
            ctx.submit_and_wait(cmd);
            ctx.free_command_buffer(cmd);
        }
    }
    ctx.destroy_buffer(staging);
    m.staging_limit = chunk;
    m.readback = ctx.create_buffer(std::min(total, TRANSFER_CHUNK), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    m.upload_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // ---- descriptors: binding b = buffer b
    std::vector<VkDescriptorSetLayoutBinding> lb(n);
    for (uint32_t b = 0; b < n; ++b) lb[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = n;
    lci.pBindings = lb.data();
    VK_CHECK(vkCreateDescriptorSetLayout(d, &lci, nullptr, &m.set_layout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(d, &pci, nullptr, &m.pool));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m.set_layout;
    VK_CHECK(vkAllocateDescriptorSets(d, &ai, &m.set));
    std::vector<VkDescriptorBufferInfo> infos(n);
    std::vector<VkWriteDescriptorSet> writes(n);
    for (uint32_t b = 0; b < n; ++b) {
        infos[b] = {m.buffers[b].buffer, 0, VK_WHOLE_SIZE};
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b].dstSet = m.set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(d, n, writes.data(), 0, nullptr);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};  // terni_order (compute.glsl)
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m.set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(d, &plci, nullptr, &m.layout));

    // ---- pipelines
    const size_t S = stages.size();
    m.pipelines = m.make_pipelines(0);

    if (ctx.timestamp_valid_bits()) {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = static_cast<uint32_t>(S + 2);  // after each stage, and at the end of the readbacks
        VK_CHECK(vkCreateQueryPool(d, &qci, nullptr, &m.queries));
    }
    m.timings.assign(S, {});
    m.stage_ns.assign(S, 0.0);

    m.tick_cmd(m.run_mask({}));  // the usual tick, recorded up front
}

ComputeRunner::~ComputeRunner() {
    Impl& m = *impl_;
    VkDevice d = m.ctx.device();
    vkDeviceWaitIdle(d);
    if (m.flight.pre) m.ctx.free_command_buffer(m.flight.pre);
    for (VkCommandBuffer c : m.readback_cmds)
        if (c) m.ctx.free_command_buffer(c);
    for (VkPipeline p : m.pipelines) vkDestroyPipeline(d, p, nullptr);
    for (VkPipeline p : m.check_pipelines) vkDestroyPipeline(d, p, nullptr);
    for (VkPipeline p : m.extras) vkDestroyPipeline(d, p, nullptr);
    for (auto& b : m.backup) m.ctx.destroy_buffer(b);
    if (m.check_rb.buffer) m.ctx.destroy_buffer(m.check_rb);
    vkDestroyPipelineLayout(d, m.layout, nullptr);
    vkDestroyDescriptorPool(d, m.pool, nullptr);
    vkDestroyDescriptorSetLayout(d, m.set_layout, nullptr);
    if (m.queries) vkDestroyQueryPool(d, m.queries, nullptr);
    for (const auto& [run, cmd] : m.tick_cmds) m.ctx.free_command_buffer(cmd);
    for (auto& b : m.buffers) m.ctx.destroy_buffer(b);
    m.ctx.destroy_buffer(m.readback);
    if (m.upload.buffer) m.ctx.destroy_buffer(m.upload);
    if (m.staging.buffer) m.ctx.destroy_buffer(m.staging);
    if (m.staging_busy.buffer) m.ctx.destroy_buffer(m.staging_busy);
    for (auto& rb : m.tick_rb)
        if (rb.buffer) m.ctx.destroy_buffer(rb);
    for (auto& r : m.retired_rb) m.ctx.destroy_buffer(r.buffer);
}

void ComputeRunner::tick(const std::vector<bool>& run) {
    tick_submit(run);
    impl_->finish();
}

void ComputeRunner::tick_submit(const std::vector<bool>& run) {
    Impl& m = *impl_;
    m.finish();
    m.age_retired();
    Impl::Flight f;
    f.run = m.run_mask(run);
    VkCommandBuffer tick_cmd = m.tick_cmd(f.run);
    std::vector<VkCommandBuffer> cmds;
    if (!m.staged_copies.empty()) {
        VkCommandBufferBeginInfo once{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        once.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        f.pre = m.ctx.allocate_command_buffer();
        VK_CHECK(vkBeginCommandBuffer(f.pre, &once));
        barrier(f.pre, COMPUTE, SHADER_RW, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        m.record_staged(f.pre);
        barrier(f.pre, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, COMPUTE | TRANSFER,
                SHADER_RW | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VK_CHECK(vkEndCommandBuffer(f.pre));
        cmds.push_back(f.pre);
    }
    cmds.push_back(tick_cmd);
    if (m.any_readback()) {
        cmds.push_back(m.readback_cmd());
        f.ends_late = m.queries != VK_NULL_HANDLE;
    }
    if (m.queries)
        m.clock.update([&](uint64_t& ticks) { return m.ctx.device_clock(ticks); }, std::chrono::steady_clock::now);
    f.submitted = std::chrono::steady_clock::now();
    m.ctx.submit(cmds);
    f.active = true;
    m.flight = f;
    // the GPU reads this staging until the tick is done: the next tick's writes go to the other one
    std::swap(m.staging, m.staging_busy);
    m.staged = 0;
    m.staged_copies.clear();
}

bool ComputeRunner::tick_done() {
    Impl& m = *impl_;
    if (!m.flight.active) return true;
    if (!m.flight.seen_done && m.ctx.submitted_done()) {
        m.flight.seen_done = true;
        m.flight.done = std::chrono::steady_clock::now();
    }
    return m.flight.seen_done;
}

void ComputeRunner::tick_wait() { impl_->finish(); }

void ComputeRunner::set_tick_spin(uint32_t microseconds) { impl_->spin_us = microseconds; }

bool ComputeRunner::tick_in_flight() const { return impl_->flight.active; }

void ComputeRunner::read(uint32_t binding, void* dst, uint64_t bytes, uint64_t offset) {
    Impl& m = *impl_;
    m.finish();
    ++m.sync_reads;
    const uint64_t chunk = m.readback.size;
    for (uint64_t off = 0; off < bytes; off += chunk) {
        const uint64_t len = std::min(chunk, bytes - off);
        VkCommandBuffer cmd = m.ctx.allocate_command_buffer();
        VkCommandBufferBeginInfo once{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        once.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &once));
        barrier(cmd, COMPUTE, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, TRANSFER, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy region{offset + off, 0, len};
        vkCmdCopyBuffer(cmd, m.buffers[binding].buffer, m.readback.buffer, 1, &region);
        barrier(cmd, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        VK_CHECK(vkEndCommandBuffer(cmd));
        m.ctx.submit_and_wait(cmd);
        m.ctx.free_command_buffer(cmd);
        std::memcpy(static_cast<char*>(dst) + off, m.readback.mapped, len);
    }
}

void ComputeRunner::write(uint32_t binding, const void* src, uint64_t bytes, uint64_t offset) {
    Impl& m = *impl_;
    m.finish();
    if (!m.upload.buffer)
        m.upload = m.ctx.create_buffer(m.readback.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const uint64_t chunk = m.upload.size;
    for (uint64_t off = 0; off < bytes; off += chunk) {
        const uint64_t len = std::min(chunk, bytes - off);
        std::memcpy(m.upload.mapped, static_cast<const char*>(src) + off, len);
        VkCommandBuffer cmd = m.ctx.allocate_command_buffer();
        VkCommandBufferBeginInfo once{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        once.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &once));
        // previous ticks' shader writes must finish before we overwrite
        barrier(cmd, COMPUTE, SHADER_RW, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferCopy region{0, offset + off, len};
        vkCmdCopyBuffer(cmd, m.upload.buffer, m.buffers[binding].buffer, 1, &region);
        VK_CHECK(vkEndCommandBuffer(cmd));
        m.ctx.submit_and_wait(cmd);
        m.ctx.free_command_buffer(cmd);
    }
}

void ComputeRunner::queue_write(uint32_t binding, const void* src, uint64_t bytes, uint64_t offset) {
    Impl& m = *impl_;
    if (bytes == 0) return;
    const uint64_t limit = m.staging_limit;
    if (bytes > limit) {                      // too big to stage: apply directly
        flush_queued();
        write(binding, src, bytes, offset);
        return;
    }
    if (m.staged + bytes > limit) flush_queued();  // keeps the order of writes
    if (m.staged + bytes > m.staging.size) {      // grow, keeping what is staged (not in use by the GPU)
        const uint64_t need = m.staged + bytes;
        const uint64_t size = std::min(limit, std::max<uint64_t>(need, std::max<uint64_t>(1 << 20, m.staging.size * 2)));
        GpuBuffer grown = m.ctx.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (m.staged) std::memcpy(grown.mapped, m.staging.mapped, m.staged);
        if (m.staging.buffer) m.ctx.destroy_buffer(m.staging);
        m.staging = grown;
    }
    std::memcpy(static_cast<char*>(m.staging.mapped) + m.staged, src, bytes);
    m.staged_copies.push_back({binding, VkBufferCopy{m.staged, offset, bytes}});
    m.staged += (bytes + 15) & ~uint64_t(15);  // keep copies 16-byte aligned in staging
}

void ComputeRunner::flush_queued() {
    Impl& m = *impl_;
    if (m.staged_copies.empty()) return;
    m.finish();
    VkCommandBuffer cmd = m.ctx.allocate_command_buffer();
    VkCommandBufferBeginInfo once{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    once.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &once));
    barrier(cmd, COMPUTE, SHADER_RW, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    m.record_staged(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    m.ctx.submit_and_wait(cmd);
    m.ctx.free_command_buffer(cmd);
    m.staged = 0;
    m.staged_copies.clear();
}

void ComputeRunner::discard_queued() {
    impl_->staged = 0;
    impl_->staged_copies.clear();
}

void ComputeRunner::take_queued(ComputeRunner& from,
                                const std::function<void(uint32_t, const void*, uint64_t, uint64_t)>& take) {
    const Impl& f = *from.impl_;
    for (const auto& [binding, region] : f.staged_copies) {
        const char* src = static_cast<const char*>(f.staging.mapped) + region.srcOffset;
        if (take) take(binding, src, region.size, region.dstOffset);
        else queue_write(binding, src, region.size, region.dstOffset);
    }
    from.discard_queued();
}

uint32_t ComputeRunner::add_tick_readback(uint32_t binding, uint64_t bytes, uint64_t offset) {
    Impl& m = *impl_;
    if (binding >= m.buffers.size() || offset + bytes > m.buffers[binding].size)
        throw std::out_of_range("add_tick_readback: range outside buffer");
    m.finish();  // the buffers below may be replaced
    const uint64_t at = m.tick_reads.empty() ? 0 : (m.tick_reads.back().at + m.tick_reads.back().bytes + 15) & ~uint64_t(15);
    m.tick_reads.push_back({binding, offset, bytes, at, bytes});
    m.readbacks_changed = true;
    const uint64_t total = at + bytes;
    for (auto& rb : m.tick_rb)
        if (total > rb.size) {  // grow, keeping the last tick's results
            const uint64_t size = std::max<uint64_t>(total, rb.size * 2);
            GpuBuffer grown = m.ctx.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            std::memset(grown.mapped, 0, grown.size);
            if (rb.buffer) {
                std::memcpy(grown.mapped, rb.mapped, rb.size);
                // the last tick's results stay until the tick after the next one, those before until the next
                m.retired_rb.push_back({rb, &rb == &m.tick_rb[m.rb_done] ? 2 : 1});
            }
            rb = grown;
        }
    return static_cast<uint32_t>(m.tick_reads.size() - 1);
}

const void* ComputeRunner::tick_readback(uint32_t id) const {
    const Impl& m = *impl_;
    return static_cast<const char*>(m.tick_rb[m.rb_done].mapped) + m.tick_reads.at(id).at;
}

void ComputeRunner::set_tick_readback(uint32_t id, const void* src, uint64_t bytes) {
    Impl& m = *impl_;
    const auto& r = m.tick_reads.at(id);
    if (bytes > r.bytes) throw std::out_of_range("set_tick_readback: more than the registered range");
    m.finish();  // a tick in flight would make its own results the current ones
    std::memcpy(static_cast<char*>(m.tick_rb[m.rb_done].mapped) + r.at, src, bytes);
}

void ComputeRunner::set_tick_readback_bytes(uint32_t id, uint64_t bytes) {
    auto& r = impl_->tick_reads.at(id);
    if (bytes > r.bytes) throw std::out_of_range("set_tick_readback_bytes: more than the registered range");
    if (r.size == bytes) return;
    r.size = bytes;
    impl_->readbacks_changed = true;
}

uint64_t ComputeRunner::tick_readback_bytes(uint32_t id) const { return impl_->tick_reads.at(id).size; }

std::vector<ComputeRunner::OrderDiff> ComputeRunner::tick_checked(const std::vector<bool>& check,
                                                                  const std::vector<uint64_t>& row_bytes,
                                                                  const std::vector<bool>& run_in,
                                                                  uint32_t counts_binding,
                                                                  std::vector<std::vector<uint32_t>>* counts) {
    Impl& m = *impl_;
    const std::vector<bool> runs = m.run_mask(run_in);
    const auto t0 = std::chrono::steady_clock::now();
    const size_t S = m.stages.size(), B = m.buffers.size();
    if (check.size() != S || row_bytes.size() != B || counts_binding >= B)
        throw std::invalid_argument("tick_checked: one entry per stage / buffer");
    m.finish();
    m.age_retired();
    flush_queued();  // host writes come first, as in tick()
    if (m.check_pipelines.empty()) m.check_pipelines = m.make_pipelines(1);
    std::vector<uint64_t> at(B);  // where each buffer sits in check_rb
    uint64_t total = 0;
    for (size_t b = 0; b < B; ++b) at[b] = total, total += m.buffers[b].size;
    if (m.backup.empty())
        for (const auto& buf : m.buffers)
            m.backup.push_back(m.ctx.create_buffer(buf.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    if (!m.check_rb.buffer)
        m.check_rb = m.ctx.create_buffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    const auto after_everything = [](VkCommandBuffer cmd) {
        barrier(cmd, COMPUTE | TRANSFER, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                COMPUTE | TRANSFER, SHADER_RW | TRANSFER_RW);
    };
    const auto copy_state = [&](bool to_backup) {  // the state <-> its backup
        m.run_once([&](VkCommandBuffer cmd) {
            after_everything(cmd);
            for (size_t b = 0; b < B; ++b) {
                const VkBufferCopy region{0, 0, m.buffers[b].size};
                vkCmdCopyBuffer(cmd, to_backup ? m.buffers[b].buffer : m.backup[b].buffer,
                                to_backup ? m.backup[b].buffer : m.buffers[b].buffer, 1, &region);
            }
        });
    };
    const auto run = [&](size_t s, bool one_at_a_time, uint32_t backward) {
        m.run_once([&](VkCommandBuffer cmd) {
            after_everything(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, one_at_a_time ? m.check_pipelines[s] : m.pipelines[s]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &m.set, 0, nullptr);
            vkCmdPushConstants(cmd, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(backward), &backward);
            vkCmdDispatch(cmd, one_at_a_time ? 1 : m.groups(m.stages[s].elements), 1, 1);
        });
    };
    const auto read_state = [&] {
        m.run_once([&](VkCommandBuffer cmd) {
            after_everything(cmd);
            for (size_t b = 0; b < B; ++b) {
                const VkBufferCopy region{0, at[b], m.buffers[b].size};
                vkCmdCopyBuffer(cmd, m.buffers[b].buffer, m.check_rb.buffer, 1, &region);
            }
            barrier(cmd, TRANSFER, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        });
        return static_cast<const uint32_t*>(m.check_rb.mapped);
    };
    std::vector<OrderDiff> diffs;
    const auto compare = [&](size_t s, bool parallel, const std::vector<uint32_t>& expected, const uint32_t* got) {
        for (size_t b = 0; b < B; ++b) {
            if (row_bytes[b] == 0) continue;
            const uint64_t first = at[b] / 4, words = m.buffers[b].size / 4, per = row_bytes[b];
            OrderDiff d{static_cast<uint32_t>(s), static_cast<uint32_t>(b), parallel, 0, 0};
            uint64_t last = UINT64_MAX;
            for (uint64_t w = 0; w < words; ++w) {
                if (expected[first + w] == got[first + w]) continue;
                // a word of a small field can hold values of several rows
                const auto* e = reinterpret_cast<const uint8_t*>(&expected[first + w]);
                const auto* g = reinterpret_cast<const uint8_t*>(&got[first + w]);
                for (uint64_t k = 0; k < 4; ++k) {
                    const uint64_t row = (w * 4 + k) / per;
                    if (e[k] == g[k] || row == last) continue;
                    last = row;
                    if (d.rows++ == 0) d.first_row = row;
                }
            }
            if (d.rows) diffs.push_back(d);
        }
    };

    m.run_once([&](VkCommandBuffer cmd) {  // tick start, as in the recorded tick
        after_everything(cmd);
        for (uint32_t b : m.zeroed(runs)) vkCmdFillBuffer(cmd, m.buffers[b].buffer, 0, VK_WHOLE_SIZE, 0);
    });
    std::vector<uint32_t> forwards;
    if (counts) counts->assign(S, {});
    for (size_t s = 0; s < S; ++s) {
        if (!runs[s]) continue;
        if (!check[s]) {
            run(s, false, 0);
            continue;
        }
        copy_state(true);
        run(s, true, 0);  // forwards, one item at a time
        const uint32_t* f = read_state();
        forwards.assign(f, f + total / 4);
        if (counts) {
            const auto from = forwards.begin() + std::ptrdiff_t(at[counts_binding] / 4);
            (*counts)[s].assign(from, from + std::ptrdiff_t(m.buffers[counts_binding].size / 4));
        }
        copy_state(false);
        run(s, true, 1);  // backwards
        compare(s, false, forwards, read_state());
        copy_state(false);
        run(s, false, 0);  // the normal parallel run: its result is kept
        compare(s, true, forwards, read_state());
    }
    if (m.any_readback()) m.run_once([&](VkCommandBuffer cmd) { m.record_readbacks(cmd); });  // as in tick()
    m.rb_done ^= 1;
    m.last_tick_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m.last_gpu_ms = 0;  // not measured
    m.last_device_ms = m.last_tick_ms;
    for (size_t s = 0; s < S; ++s) m.timings[s].ran = runs[s], m.timings[s].last_ms = 0;
    return diffs;
}

uint64_t ComputeRunner::sync_reads() const { return impl_->sync_reads; }

double ComputeRunner::last_tick_ms() const { return impl_->last_tick_ms; }
double ComputeRunner::last_tick_gpu_ms() const { return impl_->last_gpu_ms; }

uint32_t ComputeRunner::add_extra(const std::vector<uint32_t>& spirv) {
    Impl& m = *impl_;
    const std::vector<std::vector<uint32_t>> one{spirv};
    m.extras.push_back(m.make_pipelines(0, &one).front());
    return static_cast<uint32_t>(m.extras.size() - 1);
}

void ComputeRunner::run_extra(uint32_t id, uint32_t elements) {
    Impl& m = *impl_;
    if (id >= m.extras.size()) throw std::out_of_range("run_extra: no such pipeline");
    m.finish();
    m.run_once([&](VkCommandBuffer cmd) {
        barrier(cmd, COMPUTE | TRANSFER, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, COMPUTE,
                SHADER_RW);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &m.set, 0, nullptr);
        const uint32_t forwards = 0;
        vkCmdPushConstants(cmd, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(forwards), &forwards);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.extras[id]);
        vkCmdDispatch(cmd, m.groups(elements), 1, 1);
        barrier(cmd, COMPUTE, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, COMPUTE | TRANSFER,
                SHADER_RW | VK_ACCESS_2_TRANSFER_READ_BIT);
    });
}
double ComputeRunner::last_tick_device_ms() const { return impl_->last_device_ms; }
const std::vector<ComputeRunner::StageTiming>& ComputeRunner::stage_timings() const { return impl_->timings; }

std::string ComputeRunner::device_name() const { return impl_->ctx.device_name(); }

std::string ComputeRunner::report() const {
    const Impl& m = *impl_;
    std::ostringstream os;
    os.imbue(std::locale::classic());  // the same text whatever global locale the host has set
    os.setf(std::ios::fixed);
    os.precision(1);
    os << "upload " << m.upload_ms << " ms, " << double(m.device_bytes) / (1024.0 * 1024.0) << " MiB on GPU";
    if (m.queries && m.ticks) {
        double total = 0;
        os << "\n      GPU time per tick:";
        for (size_t s = 0; s < m.stages.size(); ++s) {
            const double us = m.timings[s].total_ms / double(m.ticks) * 1000.0;
            total += us;
            os << ' ' << m.stages[s].shader << ' ' << us << "us,";
        }
        os << " total " << total << " us";
    }
    return os.str();
}

}  // namespace terni
