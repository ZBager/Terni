// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// What a host engine relies on when embedding Terni: the embedded GLSL library,
// device selection (preference / UUID), the SPIR-V cache and cache-only starts, a
// host's global locale, and per-tick transfers.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <locale>
#include <span>
#include <sstream>
#include <string>

#include "terni/game.hpp"
#include "terni/rng.hpp"
#include "terni/vk_context.hpp"

using namespace terni;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static bool throws_with(const std::function<void()>& f, const std::string& text) {
    try {
        f();
    } catch (const std::exception& e) {
        if (std::string(e.what()).find(text) != std::string::npos) return true;
        std::printf("  unexpected message: %s\n", e.what());
        return false;
    }
    std::printf("  no exception (expected '%s')\n", text.c_str());
    return false;
}

// Number formatting a host may install as the global C++ locale (like de_DE): 16384 is
// written "16.384" and 0.5 "0,5".
struct GroupedDigits : std::numpunct<char> {
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
    char do_decimal_point() const override { return ','; }
};

static uint64_t run(const Game& game, const GameSimOptions& o, CompileStats* stats = nullptr, int ticks = 50) {
    GameSim sim(game, o);
    for (int t = 0; t < ticks; ++t) sim.tick();
    if (stats) *stats = sim.compile_stats();
    return combine(hash_fields(sim.snapshot()));
}

int main() {
    auto game = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    if (!game->ok()) return 1;
    const uint32_t stages = static_cast<uint32_t>(game->stages.size()) + 1;  // + built-in tick stage

    // ---- device selection
    const auto devices = VulkanContext::list_devices();
    auto type_of = [&](int i) { return i < 0 ? std::string("none") : devices[size_t(i)].type; };
    const int discrete = pick_device(DevicePreference::Discrete);
    const int cpu = pick_device(DevicePreference::Cpu);
    const int integrated = pick_device(DevicePreference::Integrated);
    std::printf("picked: discrete-first=%s  integrated-first=%s  cpu-first=%s\n", type_of(discrete).c_str(),
                type_of(integrated).c_str(), type_of(cpu).c_str());
    CHECK(discrete >= 0);
    for (const auto& d : devices) {
        if (!d.usable) continue;
        if (d.type == "discrete") CHECK(type_of(discrete) == "discrete");
        if (d.type == "integrated") CHECK(type_of(integrated) == "integrated");
        if (d.type == "cpu (software)") CHECK(type_of(cpu) == "cpu (software)");
        CHECK(pick_device(DevicePreference::Cpu, &d.uuid) == int(d.index));  // UUID match beats preference
    }
    std::array<uint8_t, 16> unknown{};
    unknown.fill(0xAB);
    CHECK(pick_device(DevicePreference::Discrete, &unknown) == discrete);  // no match: preference decides

    // ---- embedded GLSL library vs folder override: identical results
    GameSimOptions embedded;  // all defaults: picked device, embedded library, no cache
    const uint64_t reference = run(*game, embedded);
    GameSimOptions folder = embedded;
    folder.shader_library_dir = TERNI_SOURCE_DIR "/shaders";
    CHECK(run(*game, folder) == reference);
    if (cpu >= 0) {  // and across devices
        GameSimOptions on_cpu = embedded;
        on_cpu.device = cpu;
        CHECK(run(*game, on_cpu) == reference);
    }

    // ---- SPIR-V cache
    const fs::path cache = fs::path(TERNI_TEST_BINARY_DIR) / "test_spirv_cache";
    fs::remove_all(cache);
    GameSimOptions cached = embedded;
    cached.spirv_cache_dir = cache.string();
    CompileStats first, second, third;
    CHECK(run(*game, cached, &first) == reference);
    CHECK(run(*game, cached, &second) == reference);
    std::printf("cache: cold start compiled %u in %.1f ms, warm start loaded %u from cache in %.1f ms\n", first.compiled,
                first.ms, second.cached, second.ms);
    CHECK(first.compiled == stages && first.cached == 0);
    CHECK(second.compiled == 0 && second.cached == stages);

    // a corrupted cache file is detected and recompiled
    for (const auto& e : fs::directory_iterator(cache)) {
        std::fstream f(e.path(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(20);
        f.put(char(0x5A));
        break;
    }
    CHECK(run(*game, cached, &third) == reference);
    CHECK(third.compiled == 1 && third.cached == stages - 1);

    // a changed rule misses the cache (only that stage recompiles)
    auto modded = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    modded->stages[0].source += "\n// a modder's edit\n";
    CompileStats changed;
    run(*modded, cached, &changed, 1);
    CHECK(changed.compiled == 1 && changed.cached == stages - 1);

    // ---- cache only: never compiles, names what is missing, writes nothing
    {
        const fs::path only = fs::path(TERNI_TEST_BINARY_DIR) / "test_cache_only";
        fs::remove_all(only);
        GameSimOptions o = embedded;
        o.cache_only = true;
        CHECK(throws_with([&] { GameSim sim(*game, o); }, "cache-only start: no spirv_cache_dir"));
        o.spirv_cache_dir = only.string();
        const std::string all = std::to_string(stages) + " of " + std::to_string(stages) + " shaders are not in the SPIR-V cache";
        CHECK(throws_with([&] { GameSim sim(*game, o); }, all));
        CHECK(!fs::exists(only));
        const CompileStats filled = compile_rules(*game, only.string());  // no device involved
        CHECK(filled.compiled == stages);
        CompileStats loaded;
        CHECK(run(*game, o, &loaded) == reference);
        CHECK(loaded.cached == stages && loaded.compiled == 0);
        CHECK(throws_with([&] { GameSim sim(*modded, o); }, "(" + modded->stages[0].name + ")"));  // the edited rule
        // the same cache serves a normal start too: the key does not depend on who compiled
        GameSimOptions normal = embedded;
        normal.spirv_cache_dir = only.string();
        CompileStats warm;
        CHECK(run(*game, normal, &warm) == reference);
        CHECK(warm.cached == stages && warm.compiled == 0);
    }

    // ---- a host's global C++ locale (digit grouping, decimal comma) changes nothing
    {
        const std::locale previous = std::locale::global(std::locale(std::locale::classic(), new GroupedDigits));
        std::ostringstream probe;
        probe << 4096;
        CHECK(probe.str() == "4.096");  // the locale is in effect for new streams
        WorldBuilder wide;  // a number the locale would group (capacities are not in the prelude)
        wide.field(wide.entity("planet", 4), "stock", FieldType::Int, "", 4096);
        const std::string prelude = generate_prelude(*wide.finish());
        CHECK(prelude.find("PLANET_STOCK_SIZE = 4096u;") != std::string::npos);
        CHECK(prelude.find("4.096") == std::string::npos);
        auto reloaded = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
        CHECK(reloaded->ok() && game_checksum(*reloaded) == game_checksum(*game));
        GameSim sim(*game, embedded);  // compiles every rule under that locale
        for (int t = 0; t < 50; ++t) sim.tick();
        CHECK(combine(hash_fields(sim.snapshot())) == reference);
        const std::string report = sim.report();  // "upload 1.2 ms, ..."
        size_t i = std::string("upload ").size();
        while (i < report.size() && report[i] >= '0' && report[i] <= '9') ++i;
        CHECK(report.rfind("upload ", 0) == 0 && i < report.size() && report[i] == '.');
        std::locale::global(previous);
    }

    // ---- per-tick host feed + results in one GPU submission: span writes over many rows,
    //      read_each_tick / tick_result, and exact 128-bit sums (terni/wide.glsl)
    {
        constexpr uint32_t N = 5000;
        WorldBuilder w;
        const uint32_t cell = w.entity("cell", N);
        w.field(cell, "weight", FieldType::Int);
        w.field(cell, "value", FieldType::Money64);  // i64 per cell
        w.field(cell, "sum", FieldType::Int, "", 4);   // i128 running sum of weight * value
        w.stage("accumulate", "cell", R"(
void main() {
    FOR_EACH_CELL(i) {
        uvec4 s = uvec4(cell_sum[i * 4u], cell_sum[i * 4u + 1u], cell_sum[i * 4u + 2u], cell_sum[i * 4u + 3u]);
        s = mad_i128(s, cell_weight[i], cell_value[i]);
        for (uint k = 0u; k < 4u; ++k) cell_sum[i * 4u + k] = int(s[k]);
    }
}
)");
        for (uint32_t r = 0; r < N; ++r) w.create(cell);
        auto world = w.finish();
        CHECK(world->ok());
        // s += x << (32 * limb) on a 128-bit two's-complement number held as four 32-bit
        // limbs (portable: no __int128)
        auto add_shifted = [](uint32_t* s, int64_t x, int limb) {
            const uint64_t ux = uint64_t(x), ext = x < 0 ? 0xffffffffu : 0u;
            uint64_t carry = 0;
            for (int k = limb; k < 4; ++k) {
                const uint64_t part = k == limb ? uint32_t(ux) : k == limb + 1 ? uint32_t(ux >> 32) : ext;
                const uint64_t t = s[k] + part + carry;
                s[k] = uint32_t(t), carry = t >> 32;
            }
        };
        for (const auto& d : devices) {
            if (!d.usable) continue;
            GameSimOptions o;
            o.device = int(d.index);
            GameSim sim(*world, o);
            const uint32_t sums = sim.read_each_tick(cell, "sum");
            std::vector<uint32_t> expect(size_t(N) * 4, 0);
            std::vector<int32_t> weights(N), values(size_t(N) * 2);
            bool same = true;
            for (int t = 0; t < 20; ++t) {
                for (uint32_t r = 0; r < N; ++r) {
                    weights[r] = int32_t(rng::hash32(r * 31 + uint32_t(t))) >> (r % 9);
                    const int64_t v = int64_t((uint64_t(rng::hash32(r + 7u * uint32_t(t))) << 32) | rng::hash32(r ^ uint32_t(t)));
                    values[2 * r] = int32_t(uint32_t(v)), values[2 * r + 1] = int32_t(uint32_t(uint64_t(v) >> 32));
                    // weight * v = weight * low half + (weight * high half) << 32, each exact in int64
                    add_shifted(&expect[size_t(r) * 4], int64_t(weights[r]) * int64_t(uint32_t(uint64_t(v))), 0);
                    add_shifted(&expect[size_t(r) * 4], int64_t(weights[r]) * int64_t(int32_t(uint32_t(uint64_t(v) >> 32))), 1);
                }
                sim.write_field(cell, "weight", 0, std::span<const int32_t>(weights));
                sim.write_field(cell, "value", 0, std::span<const int32_t>(values));
                sim.tick();
                const auto got = sim.tick_result(sums);
                for (size_t k = 0; k < size_t(N) * 4; ++k) same &= uint32_t(got[k]) == expect[k];
            }
            CHECK(same);
            CHECK(sim.read_field(cell, "sum") == std::vector<int32_t>(sim.tick_result(sums).begin(), sim.tick_result(sums).end()));
            std::printf("per-tick feed on %-40s %s, last tick %.2f ms (GPU stages %.3f ms)\n", d.name.c_str(),
                        same ? "exact" : "WRONG", sim.last_tick_ms(), sim.last_tick_gpu_ms());
            CHECK(sim.last_tick_gpu_ms() >= 0 && sim.last_tick_gpu_ms() <= sim.last_tick_ms());
        }
    }

    // ---- per-tick readbacks copy the rows in use (not the whole capacity) and follow the
    //      count restored by load(); explicit ranges are kept as asked
    {
        auto make = [](uint32_t count) {
            WorldBuilder w;
            const uint32_t unit = w.entity("unit", 1000);
            w.field(unit, "n", FieldType::Int);
            w.field(unit, "pair", FieldType::Int, "", 2);
            w.stage("count_up", "unit", R"(
void main() {
    FOR_EACH_UNIT(i) {
        unit_n[i] += int(i) + 1;
        unit_pair[i * 2u] = int(terni.tick);
        unit_pair[i * 2u + 1u] = int(terni.unit_count);
    }
}
)");
            for (uint32_t r = 0; r < count; ++r) w.create(unit);
            return w.finish();
        };
        auto small = make(10), large = make(30);
        CHECK(small->ok() && large->ok());
        CHECK(game_checksum(*small) == game_checksum(*large));  // starting state is not part of it
        for (const auto& d : devices) {
            if (!d.usable) continue;
            GameSimOptions o;
            o.device = int(d.index);
            GameSim sim(*small, o);
            const uint32_t n = sim.read_each_tick(0, "n");
            const uint32_t pair = sim.read_each_tick(0, "pair");
            const uint32_t tail = sim.read_each_tick(0, "n", 8);      // rows 8.. that are in use
            const uint32_t span = sim.read_each_tick(0, "n", 5, 20);  // exactly rows 5..24
            const uint32_t none = sim.read_each_tick(0, "n", 0, 0);
            CHECK(throws_with([&] { sim.read_each_tick(0, "n", 1001); }, "beyond the capacity"));
            CHECK(sim.tick_result(n).size() == 10 && sim.tick_result(pair).size() == 20);  // before the first tick too
            sim.tick();
            sim.tick();
            auto r = [&](uint32_t id) { return sim.tick_result(id); };
            CHECK(r(n).size() == 10 && r(pair).size() == 20 && r(tail).size() == 2 && r(span).size() == 20 &&
                  r(none).empty());
            CHECK(r(n)[9] == 20 && r(tail)[0] == 18 && r(pair)[19] == 10);
            CHECK(r(span)[0] == 12 && r(span)[4] == 20 && r(span)[5] == 0);  // rows 10.. are not in use
            GameSim other(*large, o);
            for (int t = 0; t < 3; ++t) other.tick();
            sim.load(other.save());
            CHECK(r(n).size() == 30 && r(tail).size() == 22 && r(span).size() == 20 && r(none).empty());
            sim.tick();
            CHECK(r(n)[29] == 4 * 30 && r(pair)[58] == 3 && r(pair)[59] == 30 && r(tail)[21] == 4 * 30);
            const auto field = sim.read_field(0, "n", 0, 30);
            CHECK(std::vector<int32_t>(r(n).begin(), r(n).end()) == field);
            std::printf("rows-in-use readbacks on %-40s %s\n", d.name.c_str(), failures == 0 ? "ok" : "WRONG");
        }
    }

    // ---- dispatch = workgroup: 256 threads per entity, shared-memory reduction; more entities
    //      than one dispatch's 65535 groups, so FOR_EACH_GROUP must stride
    {
        constexpr uint32_t N = 70000, K = 300;
        WorldBuilder w;
        const uint32_t row = w.entity("row", N);
        w.field(row, "v", FieldType::Int, "", K);
        w.field(row, "total", FieldType::Money64);
        w.stage("row_sum", "row", R"(
shared uvec2 partial[256];
void main() {
    FOR_EACH_ROW_GROUP(r) {
        const uint t = gl_LocalInvocationID.x;
        uvec2 s = uvec2(0u);
        for (uint k = t; k < ROW_V_SIZE; k += 256u) s = add_i64(s, i64_from_int(row_v[r * ROW_V_SIZE + k]));
        partial[t] = s;
        workgroup_sync();
        for (uint step = 128u; step > 0u; step >>= 1u) {  // fixed tree order: deterministic
            if (t < step) partial[t] = add_i64(partial[t], partial[t + step]);
            workgroup_sync();
        }
        if (t == 0u) row_total[r] = partial[0];
        workgroup_sync();
    }
}
)");
        w.stage_dispatch("row_sum", Dispatch::Workgroup);
        for (uint32_t r = 0; r < N; ++r) w.create(row);
        w.stage_dispatch("no_such_stage", Dispatch::Workgroup);  // reported as an error
        auto bad = w.finish();
        CHECK(bad->errors.size() == 1);
        bad->errors.clear();
        std::vector<int32_t> v(size_t(N) * K);
        std::vector<int64_t> expect(N, 0);
        for (uint32_t r = 0; r < N; ++r)
            for (uint32_t k = 0; k < K; ++k) {
                const int32_t x = int32_t(rng::hash32(r * K + k));
                v[size_t(r) * K + k] = x;
                expect[r] += x;
            }
        for (const auto& d : devices) {
            if (!d.usable) continue;
            GameSimOptions o;
            o.device = int(d.index);
            GameSim sim(*bad, o);
            sim.write_field(row, "v", 0, std::span<const int32_t>(v));
            sim.tick();
            const auto t = sim.read_field(row, "total");
            bool same = true;
            for (uint32_t r = 0; r < N; ++r)
                same &= (int64_t(uint64_t(uint32_t(t[2 * r])) | (uint64_t(uint32_t(t[2 * r + 1])) << 32))) == expect[r];
            CHECK(same);
            std::printf("workgroup dispatch on %-40s %s (GPU %.3f ms)\n", d.name.c_str(), same ? "exact" : "WRONG",
                        sim.last_tick_gpu_ms());
        }
    }

    std::printf("test_embedding: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
