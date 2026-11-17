// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// terni_bench: runs the benchmark world (bench_world.hpp) on every usable Vulkan device and
// on the CPU reference at several thread counts, and reports the time per tick. Every run
// must end with the same state; a run that does not is reported as invalid. See
// docs/benchmark.md.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bench_world.hpp"
#include "cpu_reference.hpp"
#include "machine.hpp"
#include "terni/game.hpp"
#include "terni/vk_context.hpp"

using namespace terni;
using namespace terni::bench;

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct Stats {
    double median = 0, p10 = 0, p90 = 0, mean = 0, min = 0;
};

Stats stats_of(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]; };
    s.median = at(0.5);
    s.p10 = at(0.1);
    s.p90 = at(0.9);
    s.min = v.front();
    for (double x : v) s.mean += x;
    s.mean /= double(v.size());
    return s;
}

struct Run {
    bool cpu = false;
    std::string name;         // device name, or "CPU reference"
    std::string device_type;  // Vulkan devices: "discrete", "integrated", "cpu (software)", ...
    std::string driver;       // Vulkan devices: driver name and version
    unsigned threads = 0;     // CPU reference
    double start_ms = 0;      // GameSim or CpuWorld creation (rules from a warm SPIR-V cache)
    // total: a tick as the host sees it (compute, readback to system memory, submitting and
    // waiting, the host's events); compute: the stages alone (GPU timestamps; for the CPU
    // reference, which reads nothing back, the whole tick)
    Stats total, compute;
    std::string gpu_report;   // GameSim::report(): GPU time per stage, averaged over every tick
    StateHashes state;
    std::string differs;      // first field that differs from the first run, "" if none
    std::string error;        // the run failed
    std::string skipped;      // the device could not start the world (too large for it, say)
};

void usage() {
    std::puts(
        "usage: terni_bench [options]\n"
        "\n"
        "  --size NAME             tiny, small, medium (default), large, huge, giant or colossal\n"
        "  --provinces N           change the size: provinces, pops per province, empires, markets\n"
        "  --pops-per-province N\n"
        "  --empires N\n"
        "  --markets N\n"
        "  --seed N                starting state (default 1)\n"
        "  --ticks N               ticks measured (default 100; fewer for huge and larger)\n"
        "  --warmup N              ticks run first and not measured (default 20; fewer for huge and larger)\n"
        "  --device I              a Vulkan device, by index (repeatable; default every usable one)\n"
        "  --no-gpu                no Vulkan devices\n"
        "  --driver LIBRARY        add a Vulkan driver library, such as lavapipe\n"
        "  --lavapipe-threads N    threads of Mesa's lavapipe (sets LP_NUM_THREADS)\n"
        "  --threads LIST          CPU reference thread counts: 1,2,8 | powers (default: 1, 2, 4, ... and all)\n"
        "                          | sweep (every count from 1 to all) | all\n"
        "  --no-cpu                no CPU reference\n"
        "  --readback WHAT         read back every tick: ui (default: province unrest and empire\n"
        "                          treasuries) or pops (also every field of every pop)\n"
        "  --stages                print the GPU time of every stage\n"
        "  --json FILE             also write the results as JSON\n"
        "  --list                  list the sizes and the Vulkan devices, and stop");
}

bool parse_threads(const std::string& spec, unsigned logical, std::vector<unsigned>& out) {
    out.clear();
    if (spec == "powers") {
        for (unsigned n = 1; n < logical; n *= 2) out.push_back(n);
        out.push_back(logical);
    } else if (spec == "sweep") {
        for (unsigned n = 1; n <= logical; ++n) out.push_back(n);
    } else if (spec == "all") {
        out.push_back(logical);
    } else {
        std::stringstream in(spec);
        for (std::string item; std::getline(in, item, ',');) {
            if (item.empty() || item.find_first_not_of("0123456789") != std::string::npos) return false;
            const unsigned long n = std::stoul(item);
            if (n == 0 || n > 4096) return false;
            out.push_back(static_cast<unsigned>(n));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return !out.empty();
}

bool parse_count(const char* text, uint32_t& out) {
    const std::string s = text;
    if (s.empty() || s.size() > 10 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    const unsigned long long v = std::stoull(s);
    if (v > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

std::string get_env(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;  // getenv() is deprecated (C4996) with MSVC
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) return "";
    std::string text = value;
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string hex(uint64_t v) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

std::string json_string(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += char(c);
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", c);
            o += u;
        } else o += char(c);
    }
    return o + "\"";
}

}  // namespace

int main(int argc, char** argv) {
    BenchConfig config;
    find_size("medium", config);
    uint32_t ticks = 0, warmup = 0;
    bool ticks_given = false, warmup_given = false;
    std::vector<uint32_t> devices;
    std::vector<std::string> drivers;
    std::string thread_spec = "powers", json_path, lavapipe_threads;
    bool gpu = true, cpu = true, show_stages = false, list = false, resized = false;
    std::string readback = "ui";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        auto count_arg = [&](uint32_t& out) {
            if (!has_value || !parse_count(argv[++i], out)) {
                std::printf("error: %s needs a whole number\n", a.c_str());
                std::exit(2);
            }
        };
        if (a == "--size" && has_value) {
            if (!find_size(argv[++i], config)) {
                std::printf("error: no size '%s' (see --list)\n", argv[i]);
                return 2;
            }
        } else if (a == "--provinces") count_arg(config.provinces), resized = true;
        else if (a == "--pops-per-province") count_arg(config.pops_per_province), resized = true;
        else if (a == "--empires") count_arg(config.empires), resized = true;
        else if (a == "--markets") count_arg(config.markets), resized = true;
        else if (a == "--seed") count_arg(config.seed);
        else if (a == "--ticks") count_arg(ticks), ticks_given = true;
        else if (a == "--warmup") count_arg(warmup), warmup_given = true;
        else if (a == "--device") {
            uint32_t d = 0;
            count_arg(d);
            devices.push_back(d);
        } else if (a == "--driver" && has_value) drivers.push_back(argv[++i]);
        else if (a == "--lavapipe-threads" && has_value) {
            uint32_t n = 0;
            count_arg(n);
            lavapipe_threads = std::to_string(n);
        } else if (a == "--threads" && has_value) thread_spec = argv[++i];
        else if (a == "--json" && has_value) json_path = argv[++i];
        else if (a == "--no-gpu") gpu = false;
        else if (a == "--no-cpu") cpu = false;
        else if (a == "--stages") show_stages = true;
        else if (a == "--readback" && has_value) {
            readback = argv[++i];
            if (readback != "ui" && readback != "pops") {
                std::printf("error: --readback takes ui or pops\n");
                return 2;
            }
        }
        else if (a == "--list") list = true;
        else {
            usage();
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (resized) config.name = "custom";
    if (!ticks_given) ticks = config.ticks;
    if (!warmup_given) warmup = config.warmup;
    if (const std::string why = config.check(); !why.empty()) {
        std::printf("error: %s\n", why.c_str());
        return 2;
    }
    if (ticks == 0) {
        std::puts("error: --ticks must be at least 1");
        return 2;
    }

    // lavapipe reads its thread count when the Vulkan instance is made, so before anything else
    if (!lavapipe_threads.empty()) set_env("LP_NUM_THREADS", lavapipe_threads);
    lavapipe_threads = get_env("LP_NUM_THREADS");

    const Machine machine = describe_machine();
    std::vector<unsigned> thread_counts;
    if (cpu && !parse_threads(thread_spec, std::max(1u, machine.logical_cores), thread_counts)) {
        std::printf("error: --threads takes a list such as 1,2,8, or powers, sweep or all\n");
        return 2;
    }

    try {
        for (const auto& d : drivers) add_vulkan_driver(d);
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    std::vector<DeviceInfo> device_list = gpu || list ? VulkanContext::list_devices() : std::vector<DeviceInfo>{};
    if (list) {
        std::puts("sizes:");
        for (const auto& s : standard_sizes())
            std::printf("  %-8s %6u provinces x %4u pops = %9llu pops, %4u empires, %4u markets, %3u + %u ticks\n",
                        s.name.c_str(), s.provinces, s.pops_per_province, static_cast<unsigned long long>(s.pops()),
                        s.empires, s.markets, s.warmup, s.ticks);
        std::puts("Vulkan devices:");
        for (const auto& d : device_list)
            std::printf("  [%u] %s (%s, %s)%s\n", d.index, d.name.c_str(), d.type.c_str(), d.driver.c_str(),
                        d.usable ? "" : ("  not usable: " + d.why_not).c_str());
        return 0;
    }
    if (gpu && devices.empty())
        for (const auto& d : device_list)
            if (d.usable) devices.push_back(d.index);
    if (!gpu) devices.clear();
    if (devices.empty() && thread_counts.empty()) {
        std::puts("error: nothing to run (no usable Vulkan device and no CPU reference)");
        return 1;
    }

    std::printf("terni_bench %u, Terni %s (%s, %s build)\n", BENCH_VERSION, TERNI_BENCH_TERNI_VERSION, TERNI_BENCH_COMMIT,
                TERNI_BENCH_BUILD_TYPE);
    std::printf("world %s: %u provinces, %llu pops (%u per province), %u empires, %u markets, %u goods, seed %u\n",
                config.name.c_str(), config.provinces, static_cast<unsigned long long>(config.pops()),
                config.pops_per_province, config.empires, config.markets, GOODS, config.seed);
    std::printf("machine: %s, %u cores, %u threads; %s; %s\n", machine.cpu.c_str(), machine.physical_cores,
                machine.logical_cores, machine.os.c_str(), machine.compiler.c_str());
    if (std::string(TERNI_BENCH_BUILD_TYPE) != "Release")
        std::puts("warning: not a Release build; the times say little");

    const auto t_build = Clock::now();
    std::unique_ptr<Game> game = build_world(config);
    const double build_ms = ms_since(t_build);
    for (const auto& e : game->errors) std::printf("error: %s\n", e.text().c_str());
    for (const auto& w : game->warnings) std::printf("warning: %s\n", w.text().c_str());
    if (!game->ok()) return 1;

    // Compile once into a fresh cache: that is the cold compile time; every device then
    // starts from the cache, as a game does from its second start on.
    namespace fs = std::filesystem;
    const fs::path cache = fs::temp_directory_path() /
                           ("terni_bench_" + std::to_string(Clock::now().time_since_epoch().count()));
    CompileStats compiled;
    try {
        compiled = compile_rules(*game, cache.string());
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    uint64_t state_bytes = 0;
    for (const auto& t : game->tables)
        for (const auto& c : t.columns) state_bytes += c.size() * 4;
    std::printf("built the world in %.0f ms (%.0f MiB of fields); compiled %u rules in %.0f ms\n\n", build_ms,
                double(state_bytes) / (1024.0 * 1024.0), compiled.compiled, compiled.ms);
    // read back every tick: the fields asked for, and the alive bits Terni brings back itself
    uint64_t readback_bytes = uint64_t(config.provinces) * 4 + uint64_t(config.empires) * 8 +
                              uint64_t(config.army_capacity() + 31) / 32 * 4;
    if (readback == "pops")
        for (const auto& f : game->entities[POP].fields) readback_bytes += config.pops() * f.size * f.words() * 4;
    char amount[32];
    if (readback_bytes >= 1024 * 1024) std::snprintf(amount, sizeof amount, "%.1f MiB", double(readback_bytes) / 1048576.0);
    else std::snprintf(amount, sizeof amount, "%.0f KiB", double(readback_bytes) / 1024.0);
    const std::string readback_text = std::string(readback == "pops"
                                                      ? "every pop field, province unrest and empire treasuries"
                                                      : "province unrest and empire treasuries") +
                                      " (" + amount + " per tick)";
    std::printf("%u warm-up ticks, then %u measured; read back: %s\n", warmup, ticks, readback_text.c_str());

    std::vector<Run> runs;
    std::vector<HostEvent> events;
    for (uint32_t d : devices) {
        Run r;
        for (const auto& info : device_list)
            if (info.index == d) r.name = info.name, r.device_type = info.type, r.driver = info.driver;
        std::printf("  %s%s ...", r.name.empty() ? ("device " + std::to_string(d)).c_str() : r.name.c_str(),
                    r.driver.empty() ? "" : (", driver " + r.driver).c_str());
        std::fflush(stdout);
        try {
            GameSimOptions options;
            options.device = int(d);
            options.spirv_cache_dir = cache.string();
            const auto t0 = Clock::now();
            std::unique_ptr<GameSim> started;
            std::vector<std::pair<std::string, uint32_t>> pop_reads;  // (field, read_each_tick id)
            try {
                started = std::make_unique<GameSim>(*game, options);
                r.start_ms = ms_since(t0);
                // what a host's map and interface would read every tick ...
                started->read_each_tick(PROVINCE, "unrest");
                started->read_each_tick(EMPIRE, "treasury");
                // ... or a host that keeps a copy of every pop
                if (readback == "pops")
                    for (const auto& f : game->entities[POP].fields)
                        pop_reads.emplace_back("pop." + f.name, started->read_each_tick(POP, f.name));
            } catch (const std::exception& e) {
                r.skipped = e.what();  // usually more memory or larger buffers than the device has
                started.reset();
            }
            if (!started) {
                std::printf(" skipped: %s\n", r.skipped.c_str());
                runs.push_back(std::move(r));
                continue;
            }
            GameSim& sim = *started;
            r.name = sim.device_name();
            std::vector<double> wall, gpu_ms;
            for (uint32_t t = 0; t < warmup + ticks; ++t) {
                const auto t1 = Clock::now();
                host_events(*game, config, sim.ticks(), events);
                for (const auto& ev : events) sim.add_modifier(ev.bundle, ev.entity, ev.row, ev.scale, ev.duration);
                sim.tick();
                if (t >= warmup) {
                    wall.push_back(ms_since(t1));
                    gpu_ms.push_back(sim.last_tick_gpu_ms());
                }
            }
            r.total = stats_of(wall);
            r.compute = stats_of(gpu_ms);
            r.gpu_report = sim.report();
            // what came back with the last tick must be the state itself
            std::vector<std::pair<std::string, uint64_t>> read_back;
            for (const auto& [name, id] : pop_reads) {
                const auto words = sim.tick_result(id);
                read_back.emplace_back(name, hash_words(words.data(), words.size()));
            }
            r.state = state_hashes(sim, *game);
            for (const auto& got : read_back)
                for (const auto& f : r.state.fields)
                    if (f.first == got.first && f.second != got.second)
                        throw std::runtime_error(got.first + " as read back with the last tick differs from the state");
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        std::printf(" %s\n", r.error.empty() ? "done" : ("failed: " + r.error).c_str());
        runs.push_back(std::move(r));
    }
    for (unsigned n : thread_counts) {
        Run r;
        r.cpu = true;
        r.name = "CPU reference";
        r.threads = n;
        std::printf("  CPU reference, %u thread%s ...", n, n == 1 ? "" : "s");
        std::fflush(stdout);
        const auto t0 = Clock::now();
        CpuWorld world(*game, config, n);
        r.start_ms = ms_since(t0);
        std::vector<double> wall;
        for (uint32_t t = 0; t < warmup + ticks; ++t) {
            const auto t1 = Clock::now();
            world.tick();
            if (t >= warmup) wall.push_back(ms_since(t1));
        }
        r.total = r.compute = stats_of(wall);
        r.state = world.state_hashes();
        std::printf(" done\n");
        runs.push_back(std::move(r));
    }
    std::error_code ignored;
    fs::remove_all(cache, ignored);

    // every run must end in the same state as the first that finished
    const Run* first = nullptr;
    bool identical = true;
    for (auto& r : runs) {
        if (!r.error.empty() || !r.skipped.empty()) continue;
        if (!first) first = &r;
        r.differs = first_difference(first->state, r.state);
        identical &= r.differs.empty();
    }
    const Run* one_thread = nullptr;
    for (const auto& r : runs)
        if (r.cpu && r.threads == 1) one_thread = &r;

    std::printf("\n%-46s %10s %19s %11s %9s %9s\n", "", "total ms", "p10 - p90", "compute ms", "vs 1 CPU", "start ms");
    for (const auto& r : runs) {
        std::string label = r.cpu ? "CPU reference, " + std::to_string(r.threads) + (r.threads == 1 ? " thread" : " threads")
                                  : r.name;
        if (label.size() > 46) label = label.substr(0, 46);
        if (!r.error.empty() || !r.skipped.empty()) {
            std::printf("%-46s %s: %s\n", label.c_str(), r.error.empty() ? "skipped" : "failed",
                        (r.error.empty() ? r.skipped : r.error).c_str());
            continue;
        }
        if (!r.differs.empty()) {
            std::printf("%-46s INVALID: %s differs from the first run\n", label.c_str(), r.differs.c_str());
            continue;
        }
        char range[32], speed[16] = "";
        std::snprintf(range, sizeof range, "%.3f - %.3f", r.total.p10, r.total.p90);
        if (one_thread && r.total.median > 0)
            std::snprintf(speed, sizeof speed, "%.1fx", one_thread->total.median / r.total.median);
        std::printf("%-46s %10.3f %19s %11.3f %9s %9.0f\n", label.c_str(), r.total.median, range, r.compute.median,
                    speed, r.start_ms);
    }
    if (first)
        std::printf("\nstate after %u ticks: %s (hash %s)\n", warmup + ticks,
                    identical ? "identical in every run" : "RUNS DIFFER, their times are not valid",
                    hex(first->state.combined()).c_str());
    std::printf("total ms: median time of a tick as the host sees it: compute, readback to system memory, and\n"
                "submitting and waiting. Read back every tick:\n  %s.\n"
                "compute ms: the stages alone (GPU timestamps; the CPU reference reads nothing back, so for it\n"
                "the two are the same). p10 - p90: the spread of total ms. start ms: creating the simulation,\n"
                "rules from the SPIR-V cache. lavapipe threads: %s.\n",
                readback_text.c_str(), lavapipe_threads.empty() ? "one per logical core (LP_NUM_THREADS not set)" : lavapipe_threads.c_str());
    if (show_stages)
        for (const auto& r : runs)
            if (!r.cpu && r.error.empty() && r.skipped.empty()) std::printf("\n%s\n  %s\n", r.name.c_str(), r.gpu_report.c_str());

    if (!json_path.empty()) {
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o.setf(std::ios::fixed);
        o.precision(4);
        auto stats = [&](const Stats& s) {
            o << "{\"median\": " << s.median << ", \"p10\": " << s.p10 << ", \"p90\": " << s.p90 << ", \"mean\": " << s.mean
              << ", \"min\": " << s.min << "}";
        };
        o << "{\n  \"benchmark\": \"terni_bench\",\n  \"benchmark_version\": " << BENCH_VERSION
          << ",\n  \"terni_version\": " << json_string(TERNI_BENCH_TERNI_VERSION)
          << ",\n  \"commit\": " << json_string(TERNI_BENCH_COMMIT)
          << ",\n  \"state_mib\": " << double(state_bytes) / (1024.0 * 1024.0) << ",\n  \"build_type\": " << json_string(TERNI_BENCH_BUILD_TYPE) << ",\n  \"world\": {\"size\": "
          << json_string(config.name) << ", \"provinces\": " << config.provinces << ", \"pops_per_province\": "
          << config.pops_per_province << ", \"pops\": " << config.pops() << ", \"empires\": " << config.empires
          << ", \"markets\": " << config.markets << ", \"goods\": " << GOODS << ", \"army_capacity\": "
          << config.army_capacity() << ", \"seed\": " << config.seed << "},\n  \"warmup_ticks\": " << warmup
          << ",\n  \"ticks\": " << ticks << ",\n  \"machine\": {\"cpu\": " << json_string(machine.cpu)
          << ", \"physical_cores\": " << machine.physical_cores << ", \"logical_cores\": " << machine.logical_cores
          << ", \"os\": " << json_string(machine.os) << ", \"compiler\": " << json_string(machine.compiler)
          << ", \"lavapipe_threads\": " << json_string(lavapipe_threads.empty() ? "default" : lavapipe_threads)
          << "},\n  \"readback\": " << json_string(readback) << ",\n  \"readback_mib_per_tick\": "
          << double(readback_bytes) / (1024.0 * 1024.0) << ",\n  \"build_world_ms\": " << build_ms << ",\n  \"compile_ms\": " << compiled.ms << ",\n  \"runs\": [";
        for (size_t i = 0; i < runs.size(); ++i) {
            const Run& r = runs[i];
            o << (i ? "," : "") << "\n    {\"kind\": " << (r.cpu ? "\"cpu reference\"" : "\"vulkan\"");
            if (r.cpu) o << ", \"threads\": " << r.threads;
            else
                o << ", \"device\": " << json_string(r.name) << ", \"device_type\": " << json_string(r.device_type)
                  << ", \"driver\": " << json_string(r.driver);
            if (!r.error.empty() || !r.skipped.empty()) {
                o << ", \"" << (r.error.empty() ? "skipped" : "error")
                  << "\": " << json_string(r.error.empty() ? r.skipped : r.error) << "}";
                continue;
            }
            o << ", \"start_ms\": " << r.start_ms << ", \"total_ms\": ";
            stats(r.total);
            o << ", \"compute_ms\": ";
            stats(r.compute);
            if (!r.cpu) o << ", \"stage_report\": " << json_string(r.gpu_report);
            o << ", \"state_hash\": \"" << hex(r.state.combined()) << "\", \"valid\": "
              << (r.differs.empty() ? "true" : "false") << "}";
        }
        o << "\n  ],\n  \"identical\": " << (identical ? "true" : "false") << "\n}\n";
        std::ofstream out(json_path, std::ios::binary);
        out << o.str();
        if (!out) {
            std::printf("error: cannot write %s\n", json_path.c_str());
            return 1;
        }
        std::printf("results written to %s\n", json_path.c_str());
    }

    bool failed = !identical || !first;
    for (const auto& r : runs) failed |= !r.error.empty();
    return failed ? 1 : 0;
}
