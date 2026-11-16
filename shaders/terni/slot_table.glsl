// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Workgroup-local "pre-summing" table in shared memory.
//
// Problem: when every thread (one per entity) does atomicAdd into a small set of
// global counters (e.g. per-empire totals), thousands of threads hitting the
// same addresses serialise.
//
// Fix: the 256 threads of a workgroup first add into a tiny hash table in
// shared memory (on-chip, much faster atomics, contention limited to one
// workgroup), keyed by owner id (empire, market, ...). Then each used table entry is added
// to global memory ONCE per workgroup.
//
// Integer sums => the result is identical no matter which thread grabs which
// slot or in which order the adds happen.
//
// Requires compute.glsl (workgroup_sync). Before including, define:
//   TABLE_SLOTS number of distinct keys one workgroup can hold
//   VALUES      int values per key

const uint EMPTY_KEY = 0xFFFFFFFFu;

shared uint s_key[TABLE_SLOTS];
shared int s_val[TABLE_SLOTS * VALUES];

// Whole workgroup must call this, followed by a barrier.
void table_clear() {
    for (uint i = gl_LocalInvocationIndex; i < TABLE_SLOTS; i += gl_WorkGroupSize.x) s_key[i] = EMPTY_KEY;
    for (uint i = gl_LocalInvocationIndex; i < TABLE_SLOTS * VALUES; i += gl_WorkGroupSize.x) s_val[i] = 0;
}

// Find the slot for `key`, inserting it if new (open addressing, linear
// probing; atomicCompSwap makes concurrent inserts of the same key safe).
// Returns -1 if the table is full — the caller then falls back to global atomics.
int table_slot(uint key) {
    uint h = key % TABLE_SLOTS;  // ids in one workgroup are mostly consecutive => few collisions
    for (uint probe = 0u; probe < TABLE_SLOTS; ++probe) {
        uint prev = atomicCompSwap(s_key[h], EMPTY_KEY, key);
        if (prev == EMPTY_KEY || prev == key) return int(h);
        h = (h + 1u) % TABLE_SLOTS;
    }
    return -1;
}
