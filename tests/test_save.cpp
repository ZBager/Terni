// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Save / load: replay equivalence across devices, byte-identical round trips,
// surviving modifier handles, and rejection of foreign / corrupt saves.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

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

static uint64_t state_hash(GameSim& sim) { return combine(hash_fields(sim.snapshot())); }

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

int main() {
    auto game = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    for (const auto& e : game->errors) std::printf("  %s\n", e.text().c_str());
    if (!game->ok()) return 1;
    const uint32_t empire = uint32_t(game->entity_index("empire")), planet = uint32_t(game->entity_index("planet"));
    const uint32_t war = uint32_t(game->bundle_index("war_economy"));

    std::vector<uint32_t> devices;
    std::vector<std::string> names;
    for (const auto& d : VulkanContext::list_devices())
        if (d.usable) devices.push_back(d.index), names.push_back(d.name);
    CHECK(!devices.empty());

    // ---- reference run on the first device: 10 ticks, host changes, save, 10 more ticks
    GameSim a(*game, devices[0]);
    for (int t = 0; t < 10; ++t) a.tick();
    const ModifierHandle h = a.add_modifier(war, empire, 0, fx::rate(3, 2), 15);  // expires after the save point
    a.write_field(planet, "pop", 3, {fx::qty(3333)});                           // queued, not yet applied
    const std::vector<uint8_t> save = a.save();
    for (int t = 0; t < 10; ++t) a.tick();
    const uint64_t reference = state_hash(a);
    std::printf("save: %zu bytes, checksum %016llx\n", save.size(), static_cast<unsigned long long>(a.checksum()));

    // ---- replay from the save on every device (including the CPU via lavapipe)
    for (size_t i = 0; i < devices.size(); ++i) {
        GameSim b(*game, devices[i]);
        b.load(save);
        CHECK(b.ticks() == 10);
        CHECK(b.save() == save);  // round trip is byte-identical
        CHECK(b.modifier_alive(h));  // handles survive a load
        for (int t = 0; t < 10; ++t) b.tick();
        const uint64_t replay = state_hash(b);
        std::printf("  replay on %-45s %s\n", names[i].c_str(), replay == reference ? "identical" : "DIFFERS");
        CHECK(replay == reference);
        CHECK(b.modifier_alive(h) && a.modifier_alive(h));  // due at tick 25: alive at 20 in both runs
    }

    // ---- the handle from before the save behaves the same as in the original run
    {
        GameSim b(*game, devices.back());
        b.load(save);
        CHECK(b.set_modifier_scale(h, fx::ONE));
        CHECK(b.remove_modifier(h));
        CHECK(!b.modifier_alive(h));
    }

    // ---- rejected saves leave the simulation untouched
    {
        GameSim c(*game, devices[0]);
        for (int t = 0; t < 3; ++t) c.tick();
        const uint64_t before = state_hash(c);

        std::vector<uint8_t> corrupt = save;
        corrupt[corrupt.size() / 2] ^= 0x40;
        CHECK(throws_with([&] { c.load(corrupt); }, "corrupt"));

        std::vector<uint8_t> truncated(save.begin(), save.begin() + std::ptrdiff_t(save.size() / 3));
        CHECK(throws_with([&] { c.load(truncated); }, "save:"));

        CHECK(throws_with([&] { c.load(std::vector<uint8_t>{1, 2, 3}); }, "not a Terni save"));
        CHECK(state_hash(c) == before && c.ticks() == 3);
    }

    // ---- a different game (one define changed, like a mod would) can't load it
    {
        auto modded = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
        modded->define_values[0] += 1;
        CHECK(game_checksum(*modded) != game_checksum(*game));
        GameSim m(*modded, devices[0]);
        CHECK(throws_with([&] { m.load(save); }, "different game"));
    }

    std::printf("test_save: %s (%zu device(s))\n", failures == 0 ? "PASS" : "FAIL", devices.size());
    return failures == 0 ? 0 : 1;
}
