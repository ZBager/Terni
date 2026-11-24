// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// GameSim::tick_checked(): stages whose result depends on the order their entities are
// handled in are reported (reading rows other threads of the stage write, non-atomic
// updates of a shared row), order-independent stages are not, and a checked tick ends
// in the same state as tick().
#include <cstdio>
#include <string>
#include <vector>

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

namespace {

constexpr uint32_t CELLS = 3000;

// Order-independent: reads other rows only of fields no thread of the stage writes,
// writes its own row, sums with atomics.
const char* CLEAN = R"(
void main() {
    FOR_EACH_CELL(i) {
        cell_w[i] = cell_v[(i + 1u) % terni.cell_count] * 2 + int(i);
        atomicAdd(world_total[0], cell_v[i]);
    }
}
)";
const char* CLEAN_GROUP = R"(
shared int part[256];
void main() {
    FOR_EACH_CELL_GROUP(i) {
        part[gl_LocalInvocationID.x] = cell_w[i] + int(gl_LocalInvocationID.x);
        workgroup_sync();
        if (gl_LocalInvocationID.x == 0u) cell_u[i] = part[0] + part[255] + cell_w[(i + 7u) % terni.cell_count];
        workgroup_sync();
    }
}
)";
// Reads the next cell's v, which that cell's thread changes in this very stage.
const char* CHAIN = R"(
void main() {
    FOR_EACH_CELL(i) { cell_v[i] = cell_v[(i + 1u) % terni.cell_count] + 1; }
}
)";
// Every thread adds to one shared row without an atomic: updates get lost in parallel.
const char* LOST = R"(
void main() {
    FOR_EACH_CELL(i) { world_count[0] += 1; }
}
)";
// One workgroup per cell, reading the previous cell's u written by its own workgroup.
const char* CHAIN_GROUP = R"(
void main() {
    FOR_EACH_CELL_GROUP(i) {
        if (gl_LocalInvocationID.x == 0u) cell_u[i] = cell_u[(i + CELLS - 1u) % terni.cell_count] * 3 + 1;
    }
}
)";

std::unique_ptr<Game> build(bool with_races) {
    WorldBuilder b;
    const uint32_t world = b.entity("world", 1), cell = b.entity("cell", 4096);
    b.field(world, "total", FieldType::Agg);
    b.field(world, "count", FieldType::Int);
    b.field(cell, "v", FieldType::Int);
    b.field(cell, "w", FieldType::Int);
    b.field(cell, "u", FieldType::Int);
    b.stage("clean", "cell", CLEAN, {"world.total"});
    b.stage("clean_group", "cell", CLEAN_GROUP);
    b.stage_dispatch("clean_group", Dispatch::Workgroup);
    if (with_races) {
        b.stage("chain", "cell", CHAIN);
        b.stage("lost", "cell", LOST);
        b.stage("chain_group", "cell", std::string("const uint CELLS = ") + std::to_string(CELLS) + "u;\n" + CHAIN_GROUP);
        b.stage_dispatch("chain_group", Dispatch::Workgroup);
    }
    b.create(world);
    for (uint32_t i = 0; i < CELLS; ++i) b.set(cell, b.create(cell), "v", int64_t(i % 97) - 40);
    return b.finish();
}

// The static check (warnings when a world is built): which stages it names.
std::vector<std::string> warned(const Game& g) {
    std::vector<std::string> out;
    for (const auto& w : g.warnings) {
        const size_t a = w.message.find("stage '"), b = w.message.find('\'', a + 7);
        if (a != std::string::npos && b != std::string::npos) out.push_back(w.message.substr(a + 7, b - a - 7));
    }
    return out;
}

bool has(const std::vector<OrderIssue>& issues, const std::string& stage, const std::string& field, bool parallel) {
    for (const auto& i : issues)
        if (i.stage == stage && i.field == field && i.parallel == parallel) return true;
    return false;
}

}  // namespace

int main() {
    auto racy = build(true), clean = build(false);
    auto galaxy = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
    for (const auto* g : {racy.get(), clean.get(), galaxy.get()}) {
        for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
        if (!g->ok()) return 1;
    }

    // ---- the static check names the racy stages when the world is built, and only them
    for (const auto& w : racy->warnings) std::printf("  warning: %s\n", w.text().c_str());
    CHECK((warned(*racy) == std::vector<std::string>{"chain", "lost", "chain_group"}));
    CHECK(clean->warnings.empty());
    CHECK(galaxy->warnings.empty());
    {
        WorldBuilder b;
        const uint32_t world = b.entity("world", 1), cell = b.entity("cell", 64);
        b.field(world, "total", FieldType::Agg);
        b.field(cell, "m", FieldType::Int, "", 4);
        b.field(cell, "money", FieldType::Money64);
        auto stage = [&](const std::string& name, const std::string& body) {
            b.stage(name, "cell", "void main() {\n    FOR_EACH_CELL(i) {\n" + body + "\n    }\n}\n");
        };
        stage("aliases", "        uint base = i * 4u;\n        const uint q = i;\n"
                         "        cell_m[base + 1u] = cell_m[base] + cell_m[q * 4u + 3u];");
        stage("nested", "        cell_m[(i * 2u + 1u) * 2u] = cell_m[(i * 2u) * 2u + 1u];");
        stage("neighbour", "        cell_m[(i + 1u) * 4u] = 0;");
        stage("allowed", "        cell_m[(i + 1u) * 4u] = 0;  // terni: order ok (disjoint by design)");
        stage("mixed", "        atomicAdd(world_total[0], 1);\n        cell_m[i * 4u] = world_total[0];");
        stage("member", "        cell_money[(i + 1u) % terni.cell_count].x = 1u;");
        stage("sums", "        atomicAdd(world_total[0], cell_m[(i + 5u) % terni.cell_count]);");
        // several declarators in one declaration
        stage("comma", "        const uint n = 4u, base = i * n;\n        uint k, q = i, r = min(1u, 2u), b2 = q * n;\n"
                       "        cell_m[base + 1u] = cell_m[b2 + r];");
        stage("comma_other", "        const uint base = i * 4u, other = (i + 1u) * 4u;\n"
                             "        cell_m[base] = cell_m[other];");
        // helpers: only those main() reaches, directly or not, count
        const std::string own = "void main() {\n    FOR_EACH_CELL(i) { cell_m[i * 4u] = 1; }\n}\n";
        const std::string helper = "uint far(uint j) { return cell_m[j] > 0 ? 1u : 0u; }\n";
        auto raw = [&](const std::string& name, const std::string& glsl) { b.stage(name, "cell", glsl); };
        raw("unused", helper + own);
        raw("unused_pair", "uint far(uint j);\nuint near(uint j) { return far(j); }\n" + helper + own);
        raw("unused_after", own + helper);
        raw("unused_row", "void f(uint j) { uint base = j; FOR_EACH_CELL(j) { cell_m[j] = 2; } }\n"
                          "void main() {\n    FOR_EACH_CELL(i) { uint j = i + 1u; cell_m[j] = 1; }\n}\n");
        raw("used", helper + "void main() {\n    FOR_EACH_CELL(i) { cell_m[i * 4u] = int(far(i)); }\n}\n");
        raw("used_chain", "uint far(uint j);\nuint near(uint j) { return far(j); }\n"
                          "void main() {\n    FOR_EACH_CELL(i) { cell_m[i * 4u] = int(near(i)); }\n}\n" + helper);
        raw("used_overload", "uint far(int j) { return 0u; }\n" + helper +
                             "void main() {\n    FOR_EACH_CELL(i) { cell_m[i * 4u] = int(far(i)); }\n}\n");
        raw("used_by_macro", helper + "#define FAR(j) \\\n    int(far(j))\n"
                             "void main() {\n    FOR_EACH_CELL(i) { cell_m[i * 4u] = FAR(i); }\n}\n");
        raw("made_by_macro", helper + "#define RULE(name) void name()\nRULE(body) { FOR_EACH_CELL(i) { if (i > 3u) return;"
                             " else far(i); cell_m[i * 4u] = 1; } }\nvoid main() { body(); }\n");
        raw("if_braces", "#if 1\nuint far(uint j) {\n#else\nuint far(int j) {\n#endif\n    return cell_m[j] > 0 ? 1u : 0u;\n}\n" + own);
        raw("macro_main", "#define RULE void main()\n" + helper + "RULE {\n    FOR_EACH_CELL(i) { cell_m[i * 4u] = int(far(i)); }\n}\n");
        b.create(world);
        auto g = b.finish();
        for (const auto& w : g->warnings) std::printf("  warning: %s\n", w.text().c_str());
        CHECK(g->ok());
        CHECK((warned(*g) == std::vector<std::string>{"neighbour", "mixed", "member", "comma_other", "unused_row", "used",
                                                      "used_chain", "used_overload", "used_by_macro", "made_by_macro",
                                                      "if_braces", "macro_main"}));
        CHECK(!g->warnings.empty() && g->warnings[0].where.file == "neighbour.glsl" && g->warnings[0].where.line == 3);
    }
    for (const auto& d : VulkanContext::list_devices()) {
        if (!d.usable) continue;
        GameSimOptions o;
        o.device = int(d.index);

        // the races are found, by stage, field and kind
        GameSim sim(*racy, o);
        const auto issues = sim.tick_checked();
        for (const auto& i : issues)
            std::printf("    %s: %s differs in %llu row(s) from row %llu (%s)\n", i.stage.c_str(), i.field.c_str(),
                        static_cast<unsigned long long>(i.rows), static_cast<unsigned long long>(i.first_row),
                        i.parallel ? "parallel" : "order");
        CHECK(has(issues, "chain", "cell.v", false));
        CHECK(has(issues, "lost", "world.count", true));
        CHECK(has(issues, "chain_group", "cell.u", false));
        bool only_races = true;
        for (const auto& i : issues) only_races &= i.stage == "chain" || i.stage == "lost" || i.stage == "chain_group";
        CHECK(only_races);
        CHECK(sim.ticks() == 1);

        // order-independent stages: nothing reported, and the same state as tick()
        GameSim checked(*clean, o), normal(*clean, o);
        bool quiet = true;
        for (int t = 0; t < 3; ++t) {
            quiet &= checked.tick_checked().empty();
            normal.tick();
        }
        CHECK(quiet);
        CHECK(combine(hash_fields(checked.snapshot())) == combine(hash_fields(normal.snapshot())));

        // the example game is order-independent too, and per-tick results arrive as usual
        GameSim a(*galaxy, o), b(*galaxy, o);
        const uint32_t planet = uint32_t(galaxy->entity_index("planet"));
        const uint32_t pops = a.read_each_tick(planet, "pop");
        bool galaxy_quiet = true;
        for (int t = 0; t < 5; ++t) {
            const auto found = a.tick_checked();
            for (const auto& i : found) std::printf("    mini_galaxy %s: %s\n", i.stage.c_str(), i.field.c_str());
            galaxy_quiet &= found.empty();
            b.tick();
        }
        CHECK(galaxy_quiet);
        CHECK(combine(hash_fields(a.snapshot())) == combine(hash_fields(b.snapshot())));
        CHECK(std::vector<int32_t>(a.tick_result(pops).begin(), a.tick_result(pops).end()) ==
              b.read_field(planet, "pop", 0, galaxy->tables[planet].count));
        std::printf("order check on %-45s %zu issue(s) in the racy world, %s\n", d.name.c_str(), issues.size(),
                    quiet && galaxy_quiet ? "none in clean worlds" : "FALSE ALARMS");
    }
    std::printf("test_order_check: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
