// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// DIV_DEFINE(a, name) gives exactly a / D.name (int) and a / uint(D.name) (uint), for
// divisors and numerators of every magnitude and at the edges, on every device.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "terni/game.hpp"
#include "terni/rng.hpp"

using namespace terni;

int main() {
    constexpr uint32_t ROWS = 1u << 16;
    std::vector<int32_t> divisors = {1, 2, 3, 5, 7, 10, 100, 641, 1000, 1237, 4095, 4096, 4097, 65535, 65536, 65537,
                                     1 << 30, (1 << 30) + 1, INT32_MAX - 1, INT32_MAX, -1, -2, -7, -1000, INT32_MIN};
    for (uint32_t i = 0; i < 25; ++i) divisors.push_back(1 + int32_t(rng::hash32(i + 1) >> (1 + i)));
    const uint32_t D = uint32_t(divisors.size());

    // numerators: edges, multiples of the divisors and their neighbours, then every magnitude
    std::vector<int32_t> a = {0, 1, -1, 2, -2, INT32_MAX, INT32_MAX - 1, INT32_MIN, INT32_MIN + 1};
    for (const int32_t d : divisors)
        for (const uint32_t k : {1u, 2u, 3u, 1000u, uint32_t(INT32_MAX) / uint32_t(d < 0 ? 0u - uint32_t(d) : uint32_t(d)),
                                 0xFFFFFFFFu / uint32_t(d < 0 ? 0u - uint32_t(d) : uint32_t(d))})
            for (const uint32_t by : {0u, 1u, 0xFFFFFFFFu}) a.push_back(int32_t(k * uint32_t(d) + by));
    for (uint32_t i = 0; a.size() < ROWS; ++i) {
        const uint32_t h = rng::hash32(i * 3 + 7) >> (i % 32);
        a.push_back(int32_t(i % 2 ? 0u - h : h));
    }

    std::string rule = "void main() {\n    FOR_EACH_CELL(i) {\n        const int a = cell_a[i];\n";
    WorldBuilder b;
    const uint32_t cell = b.entity("cell", ROWS);
    b.field(cell, "a", FieldType::Int);
    b.field(cell, "q", FieldType::Int, "", 2 * D);
    for (uint32_t k = 0; k < D; ++k) {
        const std::string name = "by_" + std::to_string(k), at = std::to_string(2 * D) + "u * i + " + std::to_string(2 * k) + "u";
        b.define(name, FieldType::Int);
        b.set_define(name, divisors[k]);
        rule += "        cell_q[" + at + "] = DIV_DEFINE(a, " + name + ");\n";
        rule += "        cell_q[" + at + " + 1u] = int(DIV_DEFINE(uint(a), " + name + "));\n";
    }
    b.define("unset", FieldType::Int);
    b.set_define("unset", 0);
    rule += "        cell_a[i] = a;\n    }\n}\n";
    b.stage("divide", "cell", rule);
    b.set_rows(cell, b.create_rows(cell, ROWS), "a", a);
    const auto game = b.finish();
    for (const auto& e : game->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& w : game->warnings) std::printf("  warning: %s\n", w.text().c_str());
    if (!game->ok() || !game->warnings.empty()) return 1;

    int failures = 0, devices = 0;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        ++devices;
        GameSimOptions o;
        o.device = int(d.index);
        GameSim sim(*game, o);
        sim.tick();
        const std::vector<int32_t> q = sim.read_field(cell, "q");
        uint64_t wrong = 0;
        for (uint32_t i = 0; i < ROWS; ++i)
            for (uint32_t k = 0; k < D; ++k) {
                const int32_t by = divisors[k];
                // 64 bits, as INT32_MIN / -1 does not fit 32 (it wraps, in DIV_DEFINE too)
                const int32_t want = int32_t(uint32_t(uint64_t(int64_t(a[i]) / by)));
                const uint32_t uwant = uint32_t(a[i]) / uint32_t(by < 0 ? 0u - uint32_t(by) : uint32_t(by));
                const size_t at = size_t(i) * 2 * D + 2 * k;
                // the uint form is for defines above 0
                if (q[at] == want && (by < 0 || uint32_t(q[at + 1]) == uwant)) continue;
                if (++wrong <= 5)
                    std::printf("  %d / %d: int %d (want %d), uint %u (want %u)\n", a[i], by, q[at], want,
                                uint32_t(q[at + 1]), uwant);
            }
        std::printf("DIV_DEFINE on %-45s %s\n", d.name.c_str(), wrong ? "WRONG" : "exact");
        failures += wrong != 0;
    }
    if (devices == 0) std::printf("no usable device\n");
    return failures == 0 && devices > 0 ? 0 : 1;
}
