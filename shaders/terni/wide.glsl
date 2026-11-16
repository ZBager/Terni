// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Terni wide integers — exact 64- and 128-bit arithmetic from 32-bit operations only
// (no int64 in shaders, so it runs on every Vulkan device). Two's complement:
//
//   i64   uvec2 (lo, hi)           same layout as money64 fields
//   i128  uvec4 (w0, w1, w2, w3)   w0 least significant
//
// Typical use: exact weighted sums, e.g. sum(size * value) with 32-bit sizes and
// 64-bit values, where the host divides the 128-bit total afterwards.

uvec2 i64_from_int(int a) { return uvec2(uint(a), a < 0 ? 0xFFFFFFFFu : 0u); }

uvec2 add_i64(uvec2 a, uvec2 b) {
    uint carry;
    uint lo = uaddCarry(a.x, b.x, carry);
    return uvec2(lo, a.y + b.y + carry);
}

uvec4 i128_from_i64(uvec2 a) { uint s = (a.y & 0x80000000u) != 0u ? 0xFFFFFFFFu : 0u; return uvec4(a.x, a.y, s, s); }

uvec4 add_i128(uvec4 a, uvec4 b) {
    uvec4 r;
    uint c, c1, c2;
    r.x = uaddCarry(a.x, b.x, c);
    r.y = uaddCarry(a.y, b.y, c1);
    r.y = uaddCarry(r.y, c, c2);
    c = c1 + c2;
    r.z = uaddCarry(a.z, b.z, c1);
    r.z = uaddCarry(r.z, c, c2);
    c = c1 + c2;
    r.w = a.w + b.w + c;
    return r;
}

uvec4 neg_i128(uvec4 a) { return add_i128(~a, uvec4(1u, 0u, 0u, 0u)); }

// a (int32) * b (i64) -> exact i128
uvec4 mul_i32_i64(int a, uvec2 b) {
    const bool negative = (a < 0) != ((b.y & 0x80000000u) != 0u);
    const uint ua = a < 0 ? uint(-(a + 1)) + 1u : uint(a);  // |a|, also for INT_MIN
    uvec2 ub = b;
    if ((b.y & 0x80000000u) != 0u) {                       // |b| (two's complement negate)
        uint carry;
        ub.x = uaddCarry(~b.x, 1u, carry);
        ub.y = ~b.y + carry;
    }
    uint h0, l0, h1, l1;
    umulExtended(ua, ub.x, h0, l0);
    umulExtended(ua, ub.y, h1, l1);
    uint c;
    const uint w1 = uaddCarry(h0, l1, c);
    const uvec4 r = uvec4(l0, w1, h1 + c, 0u);
    return negative ? neg_i128(r) : r;
}

// acc += a * b, exactly
uvec4 mad_i128(uvec4 acc, int a, uvec2 b) { return add_i128(acc, mul_i32_i64(a, b)); }

// ---- more i64 / i128 helpers (comparisons, negation, shifts, products, division) ----

bool is_neg_i64(uvec2 a) { return (a.y & 0x80000000u) != 0u; }
bool is_neg_i128(uvec4 a) { return (a.w & 0x80000000u) != 0u; }

uvec2 neg_i64(uvec2 a) {
    uint c;
    const uint lo = uaddCarry(~a.x, 1u, c);
    return uvec2(lo, ~a.y + c);
}
uvec2 sub_i64(uvec2 a, uvec2 b) { return add_i64(a, neg_i64(b)); }
uvec2 abs_i64(uvec2 a) { return is_neg_i64(a) ? neg_i64(a) : a; }  // INT64_MIN stays INT64_MIN (as unsigned: 2^63)

bool lt_i64(uvec2 a, uvec2 b) { return a.y != b.y ? int(a.y) < int(b.y) : a.x < b.x; }
bool le_i64(uvec2 a, uvec2 b) { return !lt_i64(b, a); }
bool eq_i64(uvec2 a, uvec2 b) { return a == b; }
uvec2 min_i64(uvec2 a, uvec2 b) { return lt_i64(b, a) ? b : a; }
uvec2 max_i64(uvec2 a, uvec2 b) { return lt_i64(a, b) ? b : a; }

bool lt_u64(uvec2 a, uvec2 b) { return a.y != b.y ? a.y < b.y : a.x < b.x; }

// i128 >> s (arithmetic, 0 <= s < 32), low 64 bits: e.g. Q.16 products back to Q.16
uvec2 shr_i128_to_i64(uvec4 v, uint s) {
    if (s == 0u) return v.xy;
    return uvec2((v.x >> s) | (v.y << (32u - s)), (v.y >> s) | (v.z << (32u - s)));
}

// (a * r) >> 16, floored: a 64-bit Q.16 value times a 32-bit Q16.16 factor
uvec2 mul_i64_q16(uvec2 a, int r) { return shr_i128_to_i64(mul_i32_i64(r, a), 16u); }

// a (i64) * b (i64) -> exact i128
uvec4 mul_i64_i64(uvec2 a, uvec2 b) {
    const bool negative = is_neg_i64(a) != is_neg_i64(b);
    const uvec2 ua = abs_i64(a), ub = abs_i64(b);
    uint h00, l00, h01, l01, h10, l10, h11, l11;
    umulExtended(ua.x, ub.x, h00, l00);
    umulExtended(ua.x, ub.y, h01, l01);
    umulExtended(ua.y, ub.x, h10, l10);
    umulExtended(ua.y, ub.y, h11, l11);
    // a*b = l00 + (h00 + l01 + l10) << 32 + (h01 + h10 + l11) << 64 + h11 << 96
    uvec4 r = uvec4(l00, h00, l11, h11);
    r = add_i128(r, uvec4(0u, l01, h01, 0u));
    r = add_i128(r, uvec4(0u, l10, h10, 0u));
    return negative ? neg_i128(r) : r;
}

// Unsigned 128 / 64 -> quotient (low 64 bits, saturated to 0xFFFF..FF if it would not fit), restoring division.
uvec2 div_u128_u64(uvec4 n, uvec2 d) {
    uvec2 q = uvec2(0u), r = uvec2(0u);
    bool overflow = false;
    // start at the numerator's highest set bit: small operands take few steps
    const int top_bit = n.w != 0u ? 96 + findMSB(n.w) : n.z != 0u ? 64 + findMSB(n.z)
                      : n.y != 0u ? 32 + findMSB(n.y) : findMSB(n.x);  // -1 when n == 0
    for (int i = top_bit; i >= 0; --i) {
        const uint bit = (n[i >> 5] >> uint(i & 31)) & 1u;
        const bool top = (r.y & 0x80000000u) != 0u;  // r << 1 would overflow 64 bits: r >= d for sure
        r = uvec2((r.x << 1) | bit, (r.y << 1) | (r.x >> 31));
        if (top || !lt_u64(r, d)) {
            uint borrow;
            const uint lo = usubBorrow(r.x, d.x, borrow);
            r = uvec2(lo, r.y - d.y - borrow);
            if (i >= 64) overflow = true;
            else if (i >= 32) q.y |= 1u << uint(i - 32);
            else q.x |= 1u << uint(i);
        }
    }
    return overflow ? uvec2(0xFFFFFFFFu) : q;
}

// truncating signed division of an i128 by an i64 (d != 0) -> i64 (saturated on overflow)
uvec2 div_i128_i64(uvec4 n, uvec2 d) {
    const bool negative = is_neg_i128(n) != is_neg_i64(d);
    const uvec4 un = is_neg_i128(n) ? neg_i128(n) : n;
    uvec2 q = div_u128_u64(un, abs_i64(d));
    if ((q.y & 0x80000000u) != 0u) q = uvec2(0xFFFFFFFFu, 0x7FFFFFFFu);  // saturate to INT64_MAX
    return negative ? neg_i64(q) : q;
}

// a * b / c with an exact 128-bit intermediate (like fp::mul_div), truncating; c != 0
uvec2 muldiv_i64(uvec2 a, uvec2 b, uvec2 c) { return div_i128_i64(mul_i64_i64(a, b), c); }

// (a << 16) / b: a 64-bit Q.16 value divided by a 64-bit Q.16 value -> Q.16 result (truncating)
uvec2 div_q16_i64(uvec2 a, uvec2 b) {
    const uvec4 wide = i128_from_i64(a);
    const uvec4 shifted = uvec4(wide.x << 16, (wide.y << 16) | (wide.x >> 16), (wide.z << 16) | (wide.y >> 16),
                                (wide.w << 16) | (wide.z >> 16));
    return div_i128_i64(shifted, b);
}
