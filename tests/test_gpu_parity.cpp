// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// GPU parity test: every GLSL primitive in shaders/terni/ must produce the same
// bits as its C++ counterpart, on every Vulkan device in the machine.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "terni/compute_runner.hpp"
#include "terni/fixed.hpp"
#include "terni/rng.hpp"
#include "terni/vk_context.hpp"

using namespace terni;
__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

int main() {
    constexpr uint32_t N = 1u << 20, KEYS = 64, OUTS = 19;
    struct Params { uint32_t n, num_keys; } params{N, KEYS};
    std::vector<int32_t> a(N), b(N), num(N), den(N), vals(N), outputs(size_t(N) * OUTS), totals(KEYS);
    std::vector<uint32_t> keys(N);
    for (uint32_t i = 0; i < N; ++i) {
        a[i] = static_cast<int32_t>(rng::hash32(i * 3 + 1));
        b[i] = static_cast<int32_t>(rng::hash32(i * 3 + 2)) >> (i % 17);  // mix of magnitudes
        den[i] = 1 + static_cast<int32_t>(rng::hash32(i * 5 + 3) % (i % 2 ? 60000u : 0x3FFFFFFFu));
        const int64_t n3 = int64_t(rng::hash32(i * 7 + 4) % static_cast<uint32_t>(den[i])) * 3;  // result < 3
        num[i] = n3 > INT32_MAX ? den[i] : static_cast<int32_t>(n3);
        keys[i] = (i / 300) % KEYS;  // runs of equal keys, like entities grouped by owner
        vals[i] = static_cast<int32_t>(rng::hash32(i * 11 + 5) % 2001) - 1000;
    }
    // edge cases for the wide-integer helpers: INT32_MIN / INT64_MIN operands
    a[0] = INT32_MIN, b[0] = INT32_MIN, num[0] = 0;          // i32 min * i64 min
    a[1] = INT32_MIN, b[1] = -1, num[1] = 0;                 // i32 min * small negative i64
    a[2] = -1, b[2] = INT32_MAX, num[2] = 0;

    // CPU reference
    std::vector<int32_t> expect(size_t(N) * OUTS), expect_totals(KEYS, 0);
    for (uint32_t i = 0; i < N; ++i) {
        int32_t* o = &expect[size_t(i) * OUTS];
        o[0] = fx::mul(a[i], b[i]);
        o[1] = fx::div(num[i], den[i]);
        o[2] = fx::clamp(a[i], -1000000, 1000000);
        o[3] = fx::to_agg(a[i]);
        o[4] = static_cast<int32_t>(rng::hash32(static_cast<uint32_t>(a[i])));
        const uint32_t roll = rng::roll(i, static_cast<uint32_t>(b[i]), 7);
        o[5] = static_cast<int32_t>(roll);
        o[6] = rng::below(roll, b[i] & 0xFFFF) ? 1 : 0;
        // int64 (hi = b, lo = a) + sign-extended delta, in wrapping uint64 math
        const uint64_t acc = ((static_cast<uint64_t>(static_cast<uint32_t>(b[i])) << 32) | static_cast<uint32_t>(a[i])) +
                             static_cast<uint64_t>(static_cast<int64_t>(num[i] - den[i]));
        o[7] = static_cast<int32_t>(acc);
        o[8] = static_cast<int32_t>(acc >> 32);
        auto i64_of = [](int32_t lo, int32_t hi) {
            return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32) | static_cast<uint32_t>(lo));
        };
        auto put128 = [](int32_t* dst, i128 v) {
            const u128 u = static_cast<u128>(v);
            for (int k = 0; k < 4; ++k) dst[k] = static_cast<int32_t>(static_cast<uint32_t>(u >> (32 * k)));
        };
        put128(o + 9, i128(a[i]) * i64_of(num[i], b[i]));
        put128(o + 13, i128(i64_of(a[i], b[i])) + i128(den[i] ^ b[i]) * i64_of(vals[i], a[i]));
        const uint64_t s = static_cast<uint64_t>(i64_of(a[i], b[i])) + static_cast<uint64_t>(i64_of(num[i], den[i]));
        o[17] = static_cast<int32_t>(s);
        o[18] = static_cast<int32_t>(s >> 32);
        expect_totals[keys[i]] += vals[i];
    }

    const char* names[OUTS] = {"fx_mul", "fx_div", "fx_clamp", "to_agg", "hash32", "rng_roll", "rng_below", "add_i64.lo",
                               "add_i64.hi", "mul_i32_i64.0", "mul_i32_i64.1", "mul_i32_i64.2", "mul_i32_i64.3",
                               "mad_i128.0", "mad_i128.1", "mad_i128.2", "mad_i128.3", "i64+i64.lo", "i64+i64.hi"};
    int failed_devices = 0, tested = 0;
    for (const auto& dev : VulkanContext::list_devices()) {
        if (!dev.usable) continue;
        ++tested;
        std::vector<ComputeBufferSpec> buffers = {
            {"params", &params, sizeof(params)},
            {"a", a.data(), a.size() * 4},
            {"b", b.data(), b.size() * 4},
            {"num", num.data(), num.size() * 4},
            {"den", den.data(), den.size() * 4},
            {"outputs", outputs.data(), outputs.size() * 4},
            {"keys", keys.data(), keys.size() * 4},
            {"vals", vals.data(), vals.size() * 4},
            {"totals", totals.data(), totals.size() * 4},
        };
        ComputeRunner runner(dev.index, false, TERNI_TEST_SHADER_DIR, buffers,
                             {{"parity_ops", N}, {"parity_reduce", N}}, {8}, true);
        runner.tick();
        runner.read(5, outputs.data(), outputs.size() * 4);
        runner.read(8, totals.data(), totals.size() * 4);

        int wrong[OUTS] = {};
        for (size_t k = 0; k < outputs.size(); ++k) wrong[k % OUTS] += outputs[k] != expect[k];
        int wrong_totals = 0;
        for (uint32_t k = 0; k < KEYS; ++k) wrong_totals += totals[k] != expect_totals[k];

        bool ok = wrong_totals == 0;
        std::printf("[%u] %s\n", dev.index, dev.name.c_str());
        for (uint32_t o = 0; o < OUTS; ++o) {
            ok &= wrong[o] == 0;
            std::printf("    %-11s %s", names[o], wrong[o] ? "MISMATCH" : "ok");
            if (wrong[o]) std::printf(" (%d of %u)", wrong[o], N);
            std::printf("\n");
        }
        std::printf("    %-11s %s\n", "keyed sum", wrong_totals ? "MISMATCH" : "ok");
        failed_devices += !ok;
    }
    std::printf("test_gpu_parity: %s (%d device(s))\n", tested && !failed_devices ? "PASS" : "FAIL", tested);
    return tested && !failed_devices ? 0 : 1;
}
