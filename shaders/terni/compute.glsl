// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Common compute-shader scaffolding. Include first (after #version).

// 256 threads: a multiple of NVIDIA's warp (32) and AMD's wave (64).
layout(local_size_x = 256) in;

// Order check (GameSim::tick_checked): a pipeline built with TERNI_ORDER_CHECK = 1 runs
// as a single workgroup that visits the items of every FOR_EACH / FOR_EACH_GROUP one at
// a time, forwards, or backwards when terni_order.backward != 0. Normal pipelines use 0;
// the driver folds the checks below away.
layout(constant_id = 1) const uint TERNI_ORDER_CHECK = 0u;
layout(push_constant) uniform TerniOrder { uint backward; } terni_order;

uint terni_order_item(uint k, uint n) {  // the k-th item a loop over n items visits
    return TERNI_ORDER_CHECK != 0u && terni_order.backward != 0u ? n - 1u - k : k;
}
uint terni_each_first() {  // check runs: only the first thread walks the items
    return TERNI_ORDER_CHECK != 0u ? (gl_GlobalInvocationID.x == 0u ? 0u : 0xFFFFFFFFu) : gl_GlobalInvocationID.x;
}
uint terni_each_step() { return TERNI_ORDER_CHECK != 0u ? 1u : gl_NumWorkGroups.x * gl_WorkGroupSize.x; }
uint terni_group_first() { return TERNI_ORDER_CHECK != 0u ? 0u : gl_WorkGroupID.x; }
uint terni_group_step() { return TERNI_ORDER_CHECK != 0u ? 1u : gl_NumWorkGroups.x; }

// Grid-stride loop over n elements (works for any n and any dispatch size).
#define FOR_EACH(i, n)                                                                             \
    for (uint terni_k_##i = terni_each_first(), i = terni_order_item(terni_k_##i, (n)); terni_k_##i < (n); \
         terni_k_##i += terni_each_step(), i = terni_order_item(terni_k_##i, (n)))

// Workgroup-uniform loop: one WORKGROUP per item (e.g. per planet), threads
// cooperate inside it via shared memory. barrier() is legal inside.
#define FOR_EACH_GROUP(i, n)                                                                       \
    for (uint terni_k_##i = terni_group_first(), i = terni_order_item(terni_k_##i, (n)); terni_k_##i < (n); \
         terni_k_##i += terni_group_step(), i = terni_order_item(terni_k_##i, (n)))

// Shared-memory writes before this point are visible to the whole workgroup after it.
void workgroup_sync() {
    memoryBarrierShared();
    barrier();
}
