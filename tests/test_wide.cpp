// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// GPU parity of the 64/128-bit helpers in shaders/terni/wide.glsl (products, comparisons,
// truncating division with saturation) against __int128 on the CPU, on every device.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "terni/compute_runner.hpp"
#include "terni/rng.hpp"
#include "terni/vk_context.hpp"

using namespace terni;
__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

static constexpr uint32_t OUTS = 15;

static int64_t sat_div(i128 n, i128 d) {  // truncating, |q| saturated to INT64_MAX (sign kept)
    const bool negative = (n < 0) != (d < 0);
    const u128 un = n < 0 ? u128(0) - u128(n) : u128(n), ud = d < 0 ? u128(0) - u128(d) : u128(d);
    u128 q = un / ud;
    if (q > u128(INT64_MAX)) q = u128(INT64_MAX);
    return negative ? -int64_t(q) : int64_t(q);
}

int main() {
    constexpr uint32_t N = 1u << 18;
    std::vector<int64_t> a(N), b(N), c(N);
    std::vector<int32_t> r(N);
    auto h64 = [](uint32_t i) { return int64_t((uint64_t(rng::hash32(i * 2 + 1)) << 32) | rng::hash32(i * 2 + 2)); };
    for (uint32_t i = 0; i < N; ++i) {
        a[i] = h64(i * 3) >> (i % 50);  // all magnitudes
        b[i] = h64(i * 3 + 1) >> ((i / 7) % 60);
        c[i] = h64(i * 3 + 2) >> ((i / 3) % 63);
        if (c[i] == 0) c[i] = 1;
        r[i] = int32_t(rng::hash32(i ^ 0x5bd1e995u)) >> (i % 20);
    }
    const int64_t edge[] = {INT64_MIN, INT64_MAX, 0, 1, -1, INT64_MIN + 1, 65536, -65536};
    for (uint32_t i = 0; i < 8 * 8; ++i) {
        a[i] = edge[i % 8], b[i] = edge[i / 8], c[i] = edge[(i + 3) % 8] == 0 ? 7 : edge[(i + 3) % 8];
        r[i] = i % 2 ? INT32_MIN : INT32_MAX;
    }

    std::vector<uint32_t> expect(size_t(N) * OUTS);
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t* o = &expect[size_t(i) * OUTS];
        auto put64 = [](uint32_t* d, int64_t v) { d[0] = uint32_t(uint64_t(v)), d[1] = uint32_t(uint64_t(v) >> 32); };
        put64(o + 0, int64_t((i128(a[i]) * r[i]) >> 16));
        const u128 p = u128(i128(a[i]) * i128(b[i]));
        for (int k = 0; k < 4; ++k) o[2 + k] = uint32_t(p >> (32 * k));
        put64(o + 6, sat_div(i128(a[i]) * i128(b[i]), c[i]));
        put64(o + 8, sat_div(i128(a[i]) * 65536, c[i]));
        put64(o + 10, int64_t(uint64_t(a[i]) - uint64_t(b[i])));
        o[12] = a[i] < b[i] ? 1 : 0;
        put64(o + 13, a[i] < c[i] ? c[i] : a[i]);
    }

    const char* names[] = {"mul_i64_q16", "mul_i64_i64", "muldiv_i64", "div_q16_i64", "sub_i64", "lt_i64", "max_i64"};
    const uint32_t first[] = {0, 2, 6, 8, 10, 12, 13, OUTS};
    uint32_t params = N;
    std::vector<uint32_t> out(size_t(N) * OUTS);
    int tested = 0, failed = 0;
    for (const auto& dev : VulkanContext::list_devices()) {
        if (!dev.usable) continue;
        ++tested;
        std::vector<ComputeBufferSpec> buffers = {
            {"params", &params, sizeof(params)}, {"a", a.data(), a.size() * 8}, {"b", b.data(), b.size() * 8},
            {"c", c.data(), c.size() * 8},       {"r", r.data(), r.size() * 4}, {"out", out.data(), out.size() * 4},
        };
        ComputeRunner runner(dev.index, false, TERNI_TEST_SHADER_DIR, buffers, {{"parity_wide", N}}, {}, false);
        runner.tick();
        runner.read(5, out.data(), out.size() * 4);
        std::printf("[%u] %s\n", dev.index, dev.name.c_str());
        bool ok = true;
        for (int f = 0; f < 7; ++f) {
            uint32_t wrong = 0, example = UINT32_MAX;
            for (uint32_t i = 0; i < N; ++i)
                for (uint32_t k = first[f]; k < first[f + 1]; ++k)
                    if (out[size_t(i) * OUTS + k] != expect[size_t(i) * OUTS + k]) {
                        ++wrong;
                        if (example == UINT32_MAX) example = i;
                        break;
                    }
            ok &= wrong == 0;
            std::printf("    %-12s %s", names[f], wrong ? "MISMATCH" : "ok");
            if (wrong)
                std::printf(" (%u of %u; e.g. a=%lld b=%lld c=%lld r=%d)", wrong, N, (long long)a[example],
                            (long long)b[example], (long long)c[example], r[example]);
            std::printf("\n");
        }
        failed += !ok;
    }
    std::printf("test_wide: %s (%d device(s))\n", tested && !failed ? "PASS" : "FAIL", tested);
    return tested && !failed ? 0 : 1;
}
