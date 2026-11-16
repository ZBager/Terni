// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// CPU unit tests for the fixed-point and rng primitives.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "terni/fixed.hpp"
#include "terni/rng.hpp"

using namespace terni;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

int main() {
    // mul == exact floor((x * r) / 2^16) for in-range values
    uint32_t s = 1;
    for (int k = 0; k < 1000000; ++k) {
        s = rng::hash32(s + k);
        const fx::fixed x = static_cast<fx::fixed>(s) >> 8;  // ~24-bit values
        const fx::fixed r = static_cast<fx::fixed>(rng::hash32(s)) >> 12;
        const int64_t exact = static_cast<int64_t>(x) * r;
        const int64_t floor_div = exact >= 0 ? exact / 65536 : -((-exact + 65535) / 65536);
        CHECK(fx::mul(x, r) == static_cast<fx::fixed>(floor_div));
        if (failures > 10) break;
    }
    // div: exactly floor(a * 2^16 / b) for every divisor size
    for (int k = 0; k < 1000000; ++k) {
        s = rng::hash32(s + k);
        const fx::fixed b = 1 + static_cast<fx::fixed>(s % 0x3FFFFFFF);
        const int64_t a64 = int64_t(rng::hash32(s) % static_cast<uint32_t>(b)) * 3;  // a/b < 3
        if (a64 > INT32_MAX) continue;
        const fx::fixed a = static_cast<fx::fixed>(a64);
        const int64_t exact = (static_cast<int64_t>(a) << 16) / b;
        const int64_t got = fx::div(a, b);
        CHECK(got == exact);
        if (failures > 10) break;
    }
    // rng: below() hits roughly the requested probability
    int hits = 0;
    for (uint32_t e = 0; e < 100000; ++e) hits += rng::below(rng::roll(e, 42, 1), fx::rate(1, 4));
    CHECK(hits > 24000 && hits < 26000);
    // money64 range example: quantities never overflow when used per the rules
    CHECK(fx::to_agg(fx::qty(8000000)) == 8000000);

    std::printf("test_fixed: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
