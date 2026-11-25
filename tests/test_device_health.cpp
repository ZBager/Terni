// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Device health: tick times are judged by the median of 32 ticks against the best median
// so far, and a simulation moves to another device without any change to its state:
// a session with host writes, modifiers (timed, inherited), per-tick results and a tick in
// flight gives the same observations and the same final state whether it stays on one
// device or moves between any two in the middle. A host that polls a background tick
// rarely is judged by what the device did, not by how often it looked.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "device_clock.hpp"
#include "device_health.hpp"
#include "terni/fixed.hpp"
#include "terni/game.hpp"
#include "terni/vk_context.hpp"

using namespace terni;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static void test_monitor() {
    using detail::DeviceHealthMonitor;
    const size_t W = DeviceHealthMonitor::WINDOW;
    DeviceHealthMonitor m;
    auto window = [&](double ms, size_t spikes = 0, double spike_ms = 0) {
        for (size_t i = 0; i < W; ++i) m.add(i < spikes ? spike_ms : ms);
    };
    for (size_t i = 0; i + 1 < W; ++i) m.add(2.0);
    CHECK(m.health().ticks == W - 1 && m.health().recent_ms == 0 && m.health().usual_ms == 0 && !m.health().slow);
    m.add(2.0);  // the first full window
    CHECK(m.health().recent_ms == 2.0 && m.health().usual_ms == 2.0 && !m.health().slow);
    window(2.0, W / 2 - 1, 500.0);  // late ticks, but fewer than half: the median does not move
    CHECK(m.health().recent_ms == 2.0 && !m.health().slow);
    window(7.9);  // slower, under 4 times
    CHECK(m.health().recent_ms == 7.9 && m.health().usual_ms == 2.0 && !m.health().slow);
    window(40.0);  // twentyfold
    CHECK(m.health().slow && m.health().recent_ms == 40.0 && m.health().usual_ms == 2.0);
    window(1.5);  // free again, and faster than ever: the new usual
    CHECK(!m.health().slow && m.health().usual_ms == 1.5 && m.health().ticks == 5 * W);
    m.reset();
    CHECK(m.health().ticks == 0 && m.health().usual_ms == 0 && m.health().recent_ms == 0 && !m.health().slow);
    window(0.02);
    window(0.5);  // 25 times a tiny tick is still under a millisecond more: jitter, not a busy device
    CHECK(!m.health().slow && m.health().usual_ms == 0.02);
    window(1.5);
    CHECK(m.health().slow);
}

// The device's clock: 2 steps a nanosecond, 36 valid bits, read rarely.
static void test_clock() {
    using detail::DeviceClock;
    using std::chrono::microseconds;
    const uint64_t mask = (uint64_t(1) << 36) - 1;
    const double period = 0.5;
    DeviceClock::Time host{std::chrono::hours(1)};
    uint64_t device = mask - 1000000;  // it wraps around soon
    int reads = 0;
    bool works = true;
    const auto pass = [&](microseconds us, double drift = 0) {  // both clocks go on
        host += us;
        device = (device + uint64_t(double(us.count()) * 2000.0 * (1 + drift))) & mask;
    };
    const auto read = [&](uint64_t& ticks) {
        ++reads;
        pass(microseconds(20));
        ticks = device;  // the device's clock is read in the middle of the call
        pass(microseconds(20));
        return works;
    };
    const auto now = [&] { return host; };
    const auto near = [](double a, double b) { return a > b - 1e-6 && a < b + 1e-6; };

    // a tick that the device finishes 0.3 ms after its submission and the host sees 5 ms after
    DeviceClock clock;
    const auto tick = [&](double drift = 0, double gpu_ms = 0.1) {
        clock.update(read, now);
        const auto submitted = host;
        pass(microseconds(300), drift);
        const uint64_t end = device;
        pass(microseconds(4700), drift);
        return clock.ms(submitted, end, mask, period, gpu_ms, 5.0);
    };
    CHECK(!clock.known());
    CHECK(near(tick(), 0.3) && clock.known() && reads == 1);
    for (int t = 0; t < 40; ++t) CHECK(near(tick(), 0.3));  // 200 ms, over the wrap: no new read
    CHECK(reads == 1);
    for (int t = 0; t < 100; ++t) CHECK(near(tick(), 0.3));  // 500 ms more: read every 250
    CHECK(reads == 3);
    for (int t = 0; t < 200; ++t) {  // drifting by 100 ppm: under 0.03 ms off
        const double ms = tick(1e-4);
        CHECK(ms > 0.27 && ms < 0.33);
    }
    CHECK(near(tick(0, 0.4), 0.4));  // never less than the stages took
    // the device's clock behind where it should be, and started again: bounded, never a wrapped huge number
    device = (device - 4000000) & mask;
    CHECK(tick(0, 0.1) >= 0.1 && tick(0, 0.1) <= 5.0);
    device = 5;
    CHECK(tick(0, 0.1) >= 0.1 && tick(0, 0.1) <= 5.0);

    // a clock that cannot be read is not tried at every tick
    DeviceClock none;
    works = false;
    reads = 0;
    for (int t = 0; t < 100; ++t) none.update(read, now), pass(microseconds(1000));
    CHECK(!none.known() && reads == 1);
}

const char* GROW = R"(
void main() {
    FOR_EACH_PLANET(p) {
        planet_seen[p] = planet_mod_growth(p);
        planet_pop[p] += fx_mul(planet_pop[p], D.base + planet_mod_growth(p)) + planet_stock[p * PLANET_STOCK_SIZE + 1];
        planet_owner[p] = (planet_owner[p] + p + terni.tick) % terni.empire_count;
    }
}
)";

struct World {
    std::unique_ptr<Game> game;
    uint32_t empire = 0, planet = 0, growth = 0, boost = 0;
};
constexpr uint32_t EMPIRES = 4, PLANETS = 48;

static World build() {
    World w;
    WorldBuilder b;
    w.empire = b.entity("empire", 8);
    w.planet = b.entity("planet", 64);
    b.field(w.planet, "owner", FieldType::Ref, "empire");
    b.field(w.planet, "pop", FieldType::Quantity);
    b.field(w.planet, "seen", FieldType::Rate);
    b.field(w.planet, "stock", FieldType::Quantity, "", 3);
    b.define("base", FieldType::Rate);
    b.set_define("base", fx::rate(1, 100));
    w.growth = b.modifier_key("growth", w.planet, FieldType::Rate, "owner");
    w.boost = b.bundle("boost", {{w.growth, fx::rate(1, 100)}});
    b.stage("grow", "planet", GROW);
    for (uint32_t e = 0; e < EMPIRES; ++e) b.create(w.empire);
    for (uint32_t p = 0; p < PLANETS; ++p) {
        const uint32_t row = b.create(w.planet);
        b.set(w.planet, row, "owner", p % EMPIRES);
        b.set(w.planet, row, "pop", fx::qty(100 + int32_t(p)));
    }
    w.game = b.finish();
    for (const auto& e : w.game->errors) std::printf("  %s\n", e.text().c_str());
    for (const auto& x : w.game->warnings) std::printf("  warning: %s\n", x.text().c_str());
    return w;
}

// One scripted session. With `to` >= 0 the simulation moves there at the worst moment:
// host writes and modifier changes queued, and a tick in flight. Returns everything a
// host could observe on the way, and the final state.
struct Seen {
    std::vector<uint64_t> values;
    StateSnapshot end;
};
static Seen session(const World& w, uint32_t from, int to) {
    Seen seen;
    auto note = [&](uint64_t v) { seen.values.push_back(v); };
    GameSim sim(*w.game, from);
    const uint32_t pops = sim.read_each_tick(w.planet, "pop");
    const uint32_t owners = sim.read_each_tick(w.planet, "owner", 4, 8);
    auto results = [&] {
        for (uint32_t id : {pops, owners})
            for (int32_t v : sim.tick_result(id)) note(uint32_t(v));
    };
    auto totals = [&] {
        for (uint32_t p = 0; p < PLANETS; ++p) note(uint32_t(sim.modifier_total(w.growth, w.planet, p)));
    };
    for (uint32_t e = 0; e < EMPIRES; ++e) sim.add_modifier(w.boost, w.empire, e, fx::rate(int32_t(e) + 1));
    const ModifierHandle timed = sim.add_modifier(w.boost, w.planet, 2, fx::rate(5), 9);  // expires after the move
    const ModifierHandle kept = sim.add_modifier(w.boost, w.planet, 3, fx::rate(7));
    for (int t = 0; t < 6; ++t) sim.tick();
    totals();  // the owner mirror is in use from here on
    results();

    // changes for the next tick, then a tick in flight, then more changes for the one after
    sim.write_field(w.planet, "pop", 5, {fx::qty(5000)});
    sim.write_field(w.planet, "stock", 6, 1, std::vector<int32_t>{fx::qty(3)});
    const ModifierHandle before = sim.add_modifier(w.boost, w.planet, 10, fx::rate(2), 4);
    sim.begin_tick();
    sim.write_field(w.planet, "owner", 7, {2});
    const ModifierHandle during = sim.add_modifier(w.boost, w.empire, 1, fx::rate(-3));
    const uint64_t reads_before = sim.sync_reads();
    if (to >= 0) {
        sim.move_to_device(uint32_t(to));
        CHECK(!sim.tick_in_flight() && sim.ticks() == 7);
        CHECK(sim.device_health().ticks == 0);
        CHECK(sim.sync_reads() > reads_before);  // counted on, not started again
    } else {
        sim.end_tick();
    }
    // everything a host holds is as it was
    results();
    totals();
    note(combine(hash_fields(sim.snapshot())));  // modifier changes since the tick are not on the device yet
    CHECK(sim.modifier_alive(timed) && sim.modifier_alive(kept) && sim.modifier_alive(before) && sim.modifier_alive(during));
    const uint64_t reads = sim.sync_reads();
    for (int t = 0; t < 5; ++t) {
        sim.tick();
        results();
        totals();
        note(sim.modifier_count());
    }
    CHECK(sim.sync_reads() == reads);  // per-tick results and inherited totals still need no GPU read
    CHECK(!sim.modifier_alive(timed) && !sim.modifier_alive(before));  // both ran out on time
    CHECK(sim.remove_modifier(kept) && sim.set_modifier_scale(during, fx::rate(4)));
    const std::vector<uint8_t> save = sim.save();
    for (int t = 0; t < 3; ++t) sim.tick();
    note(combine(hash_fields(sim.snapshot())));
    sim.load(save);
    for (int t = 0; t < 3; ++t) sim.tick();
    results();
    seen.end = sim.snapshot();
    return seen;
}

int main() {
    test_monitor();
    test_clock();

    const World w = build();
    if (!w.game->ok()) return 1;
    CHECK(w.game->warnings.empty());
    std::vector<DeviceInfo> devices;
    for (const auto& d : VulkanContext::list_devices())
        if (d.usable) devices.push_back(d);
    if (devices.empty()) {
        std::puts("no usable Vulkan device");
        return 1;
    }

    const Seen reference = session(w, devices[0].index, -1);
    for (const auto& a : devices)
        for (const auto& b : devices) {  // every pair, and a device to itself
            const Seen moved = session(w, a.index, int(b.index));
            const bool same = moved.values == reference.values && first_difference(reference.end, moved.end).empty();
            CHECK(same);
            if (!same) {
                size_t i = 0;
                while (i < moved.values.size() && i < reference.values.size() && moved.values[i] == reference.values[i]) ++i;
                std::printf("  first difference: value %zu of %zu/%zu, final state field '%s'\n", i, moved.values.size(),
                            reference.values.size(), first_difference(reference.end, moved.end).c_str());
            }
            std::printf("%-40.40s -> %-40.40s %s\n", a.name.c_str(), b.name.c_str(), same ? "identical" : "DIFFERS");
        }

    // measuring: every collected tick counts, a checked one does not, load() starts again
    {
        GameSim sim(*w.game, devices[0].index);
        CHECK(sim.device_health().ticks == 0 && sim.device_health().usual_ms == 0);
        for (int t = 0; t < 40; ++t) sim.tick();
        DeviceHealth h = sim.device_health();
        CHECK(h.ticks == 40 && h.usual_ms > 0 && h.recent_ms == h.usual_ms);
        sim.begin_tick();
        CHECK(sim.device_health().ticks == 40);  // not until it is collected
        sim.end_tick();
        (void)sim.tick_checked();
        CHECK(sim.device_health().ticks == 41 && sim.ticks() == 42);
        const std::vector<uint8_t> save = sim.save();
        sim.load(save);
        CHECK(sim.device_health().ticks == 0);

        // a device that cannot be used: an error, and the simulation goes on where it was
        const std::string name = sim.device_name();
        bool thrown = false;
        try {
            sim.move_to_device(1000);
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        CHECK(thrown && sim.device_name() == name);
        sim.tick();
        CHECK(sim.ticks() == 43);
        sim.move_to_device(devices.back().index);
        CHECK(sim.device_name() == devices.back().name);
    }

    // A tick() counts the wall time the host waited, in the same median as polled ticks.
    for (const auto& d : devices) {
        GameSim sim(*w.game, d.index);
        (void)sim.read_each_tick(w.planet, "pop");
        sim.begin_tick();  // a polled tick first, so the device's clock is known
        while (!sim.tick_ready()) std::this_thread::yield();
        sim.end_tick();
        sim.load(sim.save());  // starts the health again; the clock stays known
        std::vector<double> waited;
        for (size_t t = 0; t < detail::DeviceHealthMonitor::WINDOW; ++t) {
            sim.tick();
            waited.push_back(sim.last_tick_ms());
        }
        std::nth_element(waited.begin(), waited.begin() + std::ptrdiff_t(waited.size() / 2), waited.end());
        CHECK(sim.device_health().recent_ms == waited[waited.size() / 2]);
    }

    // A polled tick ends when its results are back: here a copy of 32 MiB, next to a stage that does
    // hardly anything.
    {
        constexpr uint32_t ROWS = 8u << 20;
        WorldBuilder b;
        const uint32_t probe = b.entity("probe", 1), heap = b.entity("heap", ROWS);
        b.field(probe, "x", FieldType::Int);
        b.field(heap, "v", FieldType::Int);
        b.stage("idle", "probe", "void main() { FOR_EACH_PROBE(p) { probe_x[p] += 1; } }");
        b.create_rows(probe, 1);
        b.create_rows(heap, ROWS);
        const auto game = b.finish();
        CHECK(game->ok());
        for (const auto& d : devices) {
            GameSim sim(*game, d.index);
            (void)sim.read_each_tick(heap, "v");
            for (size_t t = 0; t < detail::DeviceHealthMonitor::WINDOW; ++t) {
                sim.begin_tick();
                while (!sim.tick_ready()) std::this_thread::yield();
                sim.end_tick();
            }
            const double ms = sim.device_health().recent_ms;
            CHECK(ms > sim.last_tick_gpu_ms() + 0.2);  // 0.2 ms: 160 GB/s
            std::printf("a 32 MiB readback on %-40.40s %.3f ms by the device, stages %.3f ms\n", d.name.c_str(), ms, sim.last_tick_gpu_ms());
        }
    }

    // Both kinds of tick in turns, with readbacks added on the way: the copies are recorded with and
    // without a timestamp at their end, and neither may use what is gone.
    for (const auto& d : devices) {
        GameSim a(*w.game, d.index), b(*w.game, d.index);
        uint32_t ids_a[3] = {}, ids_b[3] = {};
        const char* const fields[3] = {"pop", "owner", "stock"};
        size_t n = 0;
        bool same = true;
        for (int t = 0; t < 24; ++t) {
            if (t % 8 == 0 && n < 3) ids_a[n] = a.read_each_tick(w.planet, fields[n]), ids_b[n] = b.read_each_tick(w.planet, fields[n]), ++n;
            a.tick();
            if (t % 3 == 1) b.tick();
            else b.begin_tick(), b.end_tick();
            for (size_t k = 0; k < n; ++k) {
                const auto x = a.tick_result(ids_a[k]), y = b.tick_result(ids_b[k]);
                same &= x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin());
            }
            same &= a.field_hashes() == b.field_hashes();
        }
        CHECK(same && n == 3);
    }

    // A host that polls a background tick once a frame: the device is judged by what it
    // did, not by how often the host looked. Here the host looks every 15 ms, far longer
    // than this world's tick takes on any device (a fraction of a millisecond).
    for (const auto& d : devices) {
        GameSim sim(*w.game, d.index);
        const uint32_t pops = sim.read_each_tick(w.planet, "pop");  // the tick ends with its readback
        (void)pops;
        for (int t = 0; t < int(detail::DeviceHealthMonitor::WINDOW); ++t) {
            sim.begin_tick();
            do std::this_thread::sleep_for(std::chrono::milliseconds(15));
            while (!sim.tick_ready());
            sim.end_tick();
        }
        const DeviceHealth h = sim.device_health();
        const bool ok = sim.last_tick_ms() >= 14.0 && h.ticks == detail::DeviceHealthMonitor::WINDOW &&
                        h.recent_ms > 0 && h.recent_ms < 10.0 && !h.slow;
        CHECK(ok);
        std::printf("polled every 15 ms on %-40.40s tick %.3f ms by the device, %.1f ms seen %s\n", d.name.c_str(),
                    h.recent_ms, sim.last_tick_ms(), ok ? "ok" : "WRONG");
    }

    std::printf("test_device_health: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
