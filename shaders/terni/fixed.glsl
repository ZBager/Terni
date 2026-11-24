// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Terni fixed-point arithmetic — GPU mirror of include/terni/fixed.hpp.
// Pure 32-bit: only int/uint, so it compiles anywhere (no 64-bit extensions).
//
//   quantity  Q24.8   (int)   people, goods, money
//   rate      Q16.16  (int)   prices, ratios, shares
//   aggregate int     whole units (TO_AGG floors)
//   money64   uvec2   (lo, hi) of an int64, updated with add_i64

const int QF = 8;
const int RF = 16;
const int FX_ONE = 1 << RF;  // rate 1.0

// x * r (r is a rate): bits [16, 48) of the signed 32x32->64 product.
int fx_mul(int x, int r) {
    int msb, lsb;
    imulExtended(x, r, msb, lsb);
    return (msb << 16) | int(uint(lsb) >> 16);
}

// a / b as a rate: exactly floor(a * 2^16 / b); a >= 0, b > 0, same format.
// Restoring long division for the fraction — see fx::div.
int fx_div(int a, int b) {
    uint ua = uint(a), ub = uint(b);
    uint q = ua / ub;
    uint r = ua % ub, frac = 0u;
    for (int bit = 0; bit < RF; ++bit) {
        r <<= 1u;
        frac <<= 1u;
        if (r >= ub) {
            r -= ub;
            frac |= 1u;
        }
    }
    return int((q << uint(RF)) | frac);
}

// n / d without dividing, exact for every n: the host computes the multiplier and shifts of
// each define's magnitude when the world starts (Granlund and Montgomery, "Division by
// invariant integers using multiplication"; define_reciprocal in src/game_sim.cpp).
uint terni_div(uint n, int d, uint mul, uint shift1, uint shift2) {
    uint high, low;
    umulExtended(n, mul, high, low);
    return (((n - high) >> shift1) + high) >> shift2;
}
int terni_div(int a, int d, uint mul, uint shift1, uint shift2) {  // truncates, like a / d
    uint sign_a = uint(a >> 31), sign = uint((a ^ d) >> 31);
    return int((terni_div((uint(a) ^ sign_a) - sign_a, d, mul, shift1, shift2) ^ sign) - sign);
}

// DIV_DEFINE(a, name): a / D.name for an int `a`, a / uint(D.name) for a uint. Legacy: it
// keeps working, but new rules should divide plainly; GPUs gain little from it.
#define DIV_DEFINE(a, name) \
    terni_div((a), D.name, D.terni_mul_##name, D.terni_shift1_##name, D.terni_shift2_##name)

int fx_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int fx_min(int a, int b) { return a < b ? a : b; }
int fx_max(int a, int b) { return a > b ? a : b; }

int to_agg(int q) { return q >> QF; }
int qty_from_count(uint n) { return int(n) << QF; }

// money64 += a (a may be negative): two-word add with explicit carry.
uvec2 add_i64(uvec2 t, int a) {
    uint lo = t.x + uint(a);
    uint carry = lo < t.x ? 1u : 0u;
    return uvec2(lo, t.y + carry + (a < 0 ? 0xFFFFFFFFu : 0u));
}
