// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Stage schedules (`every`, `offset`): a scheduled stage runs only in its ticks, its
// `clear` fields are zeroed only then (so a total lasts until the next run), saves
// continue on the same schedule, tick_checked() follows it, and every device agrees.
// The data-folder syntax and its errors, and the checksum, are checked too.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "terni/game.hpp"
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

const char* DAILY = R"(
void main() {
    FOR_EACH_TOWN(t) {
        town_days[t] += 1;
    }
}
)";

// Runs in ticks 2, 7, 12, ...: counts its runs, notes the tick and sums the towns' days.
const char* MONTHLY = R"(
void main() {
    FOR_EACH_TOWN(t) {
        town_runs[t] += 1;
        town_last[t] = int(terni.tick);
        atomicAdd(realm_days[0], town_days[t]);
    }
}
)";

// Every tick: the realm total must still hold the last run's value on the other ticks.
const char* READER = R"(
void main() {
    FOR_EACH_TOWN(t) {
        town_seen[t] = realm_days[0];
    }
}
)";

constexpr uint32_t TOWNS = 37, EVERY = 5, OFFSET = 2;

std::unique_ptr<Game> make_world(bool scheduled) {
    WorldBuilder b;
    const uint32_t realm = b.entity("realm", 1), town = b.entity("town", 64);
    b.field(realm, "days", FieldType::Int);
    for (const char* f : {"days", "runs", "last", "seen"}) b.field(town, f, FieldType::Int);
    b.stage("daily", "town", DAILY);
    b.stage("monthly", "town", MONTHLY, {"realm.days"});
    b.stage("reader", "town", READER);
    if (scheduled) b.stage_schedule("monthly", EVERY, OFFSET);
    b.create(realm);
    for (uint32_t t = 0; t < TOWNS; ++t) b.create(town);
    return b.finish();
}

// Host model of the world after `ticks` ticks.
struct Expected {
    int32_t days, runs, last, seen;
};
Expected expected(uint64_t ticks) {
    Expected e{0, 0, 0, 0};
    int32_t total = 0;
    for (uint64_t k = 0; k < ticks; ++k) {
        e.days += 1;
        if (k % EVERY == OFFSET) {
            e.runs += 1;
            e.last = int32_t(k);
            total = e.days * int32_t(TOWNS);
        }
        e.seen = total;
    }
    return e;
}

uint64_t state_hash(GameSim& sim) { return combine(hash_fields(sim.snapshot())); }

bool matches(GameSim& sim, const Game& g, uint64_t ticks) {
    const uint32_t town = uint32_t(g.entity_index("town"));
    const Expected e = expected(ticks);
    bool ok = true;
    const auto days = sim.read_field(town, "days"), runs = sim.read_field(town, "runs");
    const auto last = sim.read_field(town, "last"), seen = sim.read_field(town, "seen");
    for (uint32_t t = 0; t < TOWNS; ++t)
        ok &= days[t] == e.days && runs[t] == e.runs && last[t] == e.last && seen[t] == e.seen;
    return ok;
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A small game folder with the given stages.txt; returns its load errors as text.
std::string load_errors(const std::string& name, const std::string& stages, std::unique_ptr<Game>* out = nullptr) {
    const fs::path root = fs::path(TERNI_TEST_BINARY_DIR) / "schedule_games" / name;
    fs::remove_all(root);
    write(root / "schema" / "entities.txt", "town = { capacity = 4 days = int }\n");
    write(root / "rules" / "stages.txt", stages);
    write(root / "rules" / "grow.glsl", "void main() { FOR_EACH_TOWN(t) { town_days[t] += 1; } }\n");
    auto g = load_game(root.string());
    std::string text;
    for (const auto& e : g->errors) text += e.text() + "\n";
    if (out) *out = std::move(g);
    return text;
}

int main() {
    // ---- description: validation, data-folder syntax, checksum
    {
        WorldBuilder b;
        b.entity("town", 4);
        b.stage("grow", "town", DAILY);
        b.stage_schedule("grow", 0);
        b.stage_schedule("grow", 30, 30);
        b.stage_schedule("nothing", 30);
        auto g = b.finish();
        CHECK(g->errors.size() == 3);
        CHECK(g->stages[0].every == 1 && g->stages[0].offset == 0);
    }
    {
        std::unique_ptr<Game> g;
        CHECK(load_errors("good", "grow = { for_each = town  every = 30  offset = 29 }\n", &g).empty());
        CHECK(g->stages.size() == 1 && g->stages[0].every == 30 && g->stages[0].offset == 29);
        CHECK(g->stages[0].runs_in(29) && g->stages[0].runs_in(59) && !g->stages[0].runs_in(0) && !g->stages[0].runs_in(30));
        CHECK(load_errors("every_only", "grow = { for_each = town  every = 7 }\n", &g).empty());
        CHECK(g->stages[0].every == 7 && g->stages[0].offset == 0);

        std::string e = load_errors("zero", "grow = { for_each = town\n  every = 0 }\n");
        CHECK(e.find("stages.txt:2: every must be a whole number of ticks, at least 1") != std::string::npos);
        e = load_errors("fraction", "grow = { for_each = town  every = 1.5 }\n");
        CHECK(e.find("every must be") != std::string::npos);
        e = load_errors("late", "grow = { for_each = town  every = 30\n  offset = 30 }\n");
        CHECK(e.find("stages.txt:2: stage 'grow': offset 30 must be less than every (30)") != std::string::npos);
        e = load_errors("offset_only", "grow = { for_each = town  offset = 1 }\n");
        CHECK(e.find("offset 1 must be less than every (1)") != std::string::npos);
        e = load_errors("negative", "grow = { for_each = town  every = 5  offset = -1 }\n");
        CHECK(e.find("offset must be a whole number of ticks") != std::string::npos);
    }
    {
        auto plain = make_world(false), scheduled = make_world(true);
        WorldBuilder b;  // every = 1 is the default: the same checksum as no schedule
        const uint32_t realm = b.entity("realm", 1), town = b.entity("town", 64);
        b.field(realm, "days", FieldType::Int);
        for (const char* f : {"days", "runs", "last", "seen"}) b.field(town, f, FieldType::Int);
        b.stage("daily", "town", DAILY);
        b.stage("monthly", "town", MONTHLY, {"realm.days"});
        b.stage("reader", "town", READER);
        b.stage_schedule("monthly", 1, 0);
        auto explicit_daily = b.finish();
        CHECK(game_checksum(*plain) == game_checksum(*explicit_daily));
        CHECK(game_checksum(*plain) != game_checksum(*scheduled));
        auto other_offset = make_world(true);
        other_offset->stages[1].offset = OFFSET + 1;
        CHECK(game_checksum(*scheduled) != game_checksum(*other_offset));
    }

    // ---- simulation on every device
    auto world = make_world(true);
    for (const auto& w : world->warnings) std::printf("  warning: %s\n", w.text().c_str());
    for (const auto& e : world->errors) std::printf("  %s\n", e.text().c_str());
    if (!world->ok()) return 1;
    CHECK(world->warnings.empty());

    constexpr uint64_t TICKS = 23, SAVE_AT = 9;
    std::vector<uint64_t> hashes;
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        std::printf("device %u: %s\n", d.index, d.name.c_str());
        GameSimOptions o;
        o.device = int(d.index);
        GameSim sim(*world, o);
        bool each_tick = true;
        std::vector<uint8_t> save;
        for (uint64_t k = 0; k < TICKS; ++k) {
            sim.tick();
            each_tick &= matches(sim, *world, k + 1);
            if (k + 1 == SAVE_AT) save = sim.save();
        }
        CHECK(each_tick);
        hashes.push_back(state_hash(sim));

        GameSim resumed(*world, o);  // a save continues on the same schedule
        resumed.load(save);
        for (uint64_t k = SAVE_AT; k < TICKS; ++k) resumed.tick();
        CHECK(state_hash(resumed) == state_hash(sim));

        GameSim checked(*world, o);  // tick_checked follows the schedule and finds nothing
        bool same = true;
        for (uint64_t k = 0; k < EVERY + OFFSET + 1; ++k) {
            const auto issues = checked.tick_checked();
            for (const auto& i : issues) std::printf("  order issue: %s %s\n", i.stage.c_str(), i.field.c_str());
            CHECK(issues.empty());
            same &= matches(checked, *world, k + 1);
        }
        CHECK(same);
    }
    CHECK(!hashes.empty());
    for (uint64_t h : hashes) CHECK(h == hashes.front());

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("stage schedules: ok\n");
    return failures ? 1 : 0;
}
