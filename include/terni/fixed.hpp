// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Terni fixed-point arithmetic -- pure 32-bit, deterministic on every CPU and GPU.
//
// Every simulation value is an int32 in one of three formats:
//
//   quantity   Q24.8   people, goods, money          range +/-8.4M, step 1/256
//   rate       Q16.16  prices, ratios, shares, mood   range +/-32768, step 1/65536
//   aggregate  int32   sums over many entities        whole units (to_agg floors)
//
// Rules that make mixed formats work:
//   - mul(x, r): r is ALWAYS a rate; the result has x's format.
//   - two quantities are never multiplied together.
//   - div(a, b): a and b in the same format, a >= 0, b > 0; result is an exact
//     (floored) rate.
//   - values that grow forever (treasuries, research totals) are money64: an int64
//     value, stored on the GPU as two 32-bit words added with carry (add_i64 in GLSL).
//
// The GPU mirror is shaders/terni/fixed.glsl -- same operations, same bits.
// Never use floats in simulation state: they differ between CPUs, GPUs and compilers.

#include <cstdint>

namespace terni::fx {

using fixed = int32_t;    // quantity or rate
using agg = int32_t;      // aggregate (whole units)
using money64 = int64_t;  // unbounded accumulator

constexpr int QF = 8;   // fractional bits of quantities
constexpr int RF = 16;  // fractional bits of rates

constexpr fixed ONE = fixed(1) << RF;  // rate 1.0

// ---- setup-time constructors (data loading, never per tick) -----------------
constexpr fixed rate(int64_t num, int64_t den = 1) {
    return static_cast<fixed>(static_cast<int64_t>(static_cast<uint64_t>(num) << RF) / den);
}
constexpr fixed qty(int64_t num, int64_t den = 1) {
    return static_cast<fixed>(static_cast<int64_t>(static_cast<uint64_t>(num) << QF) / den);
}
constexpr fixed qty_from_count(uint32_t n) { return static_cast<fixed>(static_cast<int64_t>(n) << QF); }

// Quantity -> aggregate (floor to whole units). Convert per entity before summing,
// so every backend adds exactly the same integers.
constexpr agg to_agg(fixed q) { return q >> QF; }

// ---- per-tick operations ------------------------------------------------------
// x * r: bits [16, 48) of the exact signed 64-bit product. The GPU gets the same
// bits from a 32x32->64 multiply (imulExtended; 16-bit limbs in HLSL).
constexpr fixed mul(fixed x, fixed r) { return static_cast<fixed>((static_cast<int64_t>(x) * r) >> RF); }

// a / b as a rate: exactly floor(a * 2^16 / b), using only 32-bit integers.
// Integer part q = a / b; the 16 fractional bits come from restoring long
// division of the remainder (r < b < 2^31, so r << 1 always fits in 32 bits).
// Requires a >= 0, b > 0 and a result below 2^15 (callers guarantee the range).
constexpr fixed div(fixed a, fixed b) {
    const uint32_t ua = static_cast<uint32_t>(a), ub = static_cast<uint32_t>(b);
    const uint32_t q = ua / ub;
    uint32_t r = ua % ub, frac = 0;
    for (int bit = 0; bit < RF; ++bit) {
        r <<= 1;
        frac <<= 1;
        if (r >= ub) {
            r -= ub;
            frac |= 1;
        }
    }
    return static_cast<fixed>((q << RF) | frac);
}

constexpr fixed clamp(fixed v, fixed lo, fixed hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr fixed min(fixed a, fixed b) { return a < b ? a : b; }
constexpr fixed max(fixed a, fixed b) { return a > b ? a : b; }

// ---- display only -- never feed back into the simulation ----------------------
inline double qty_to_double(int64_t v) { return static_cast<double>(v) / double(1 << QF); }
inline double rate_to_double(int64_t v) { return static_cast<double>(v) / double(1 << RF); }

// ---- compile-time checks -------------------------------------------------------
static_assert(mul(qty(3), rate(4)) == qty(12));
static_assert(mul(qty(-3), rate(4)) == qty(-12));
static_assert(mul(rate(1, 2), rate(1, 2)) == rate(1, 4));
static_assert(mul(rate(-1), rate(1, 2)) == -rate(1, 2));
static_assert(div(qty(3), qty(4)) == rate(3, 4));
static_assert(div(qty(10), qty(4)) == rate(10, 4));
static_assert(div(1, 3) == rate(1, 3));
static_assert(div(1 << 28, 1 << 29) == rate(1, 2));
static_assert(div(30000000, 60000000) == rate(1, 2));
static_assert(div(2000000000, 2147483647) == static_cast<fixed>((int64_t(2000000000) << 16) / 2147483647));
static_assert(to_agg(qty(5, 2)) == 2);

}  // namespace terni::fx
