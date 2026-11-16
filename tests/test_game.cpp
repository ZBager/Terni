// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Data-driven game loading: error reporting, value conversion, rule compile errors.
#include <cstdio>
#include <string>

#include "terni/fixed.hpp"
#include "terni/game.hpp"

using namespace terni;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static bool has_error(const Game& g, const std::string& text) {
    for (const auto& e : g.errors)
        if (e.text().find(text) != std::string::npos) return true;
    std::printf("  missing expected error: %s\n", text.c_str());
    return false;
}

int main() {
    {  // exact decimal -> fixed point, no floats involved
        int64_t v = 0;
        CHECK(parse_value("0.1", FieldType::Rate, v) && v == 6553);
        CHECK(parse_value("-1.5", FieldType::Quantity, v) && v == -384);
        CHECK(parse_value("2500.5", FieldType::Quantity, v) && v == fx::qty(25005, 10));
        CHECK(parse_value("3000000000", FieldType::Money64, v) && v == 3000000000LL);
        CHECK(parse_value("yes", FieldType::Int, v) && v == 1);
        CHECK(!parse_value("0.5", FieldType::Agg, v));        // integers only
        CHECK(!parse_value("1e5", FieldType::Rate, v));       // no float syntax
        CHECK(!parse_value("40000", FieldType::Rate, v));     // out of Q16.16 range
        CHECK(!parse_value("-1", FieldType::Count, v));
    }
    {  // every kind of mistake is reported with file:line
        auto g = load_game(TERNI_TEST_DATA "/bad_game");
        CHECK(!g->ok());
        CHECK(has_error(*g, "schema/entities.txt:8: unknown field type 'flotsam'"));
        CHECK(has_error(*g, "schema/entities.txt:10: field 'pop' defined twice"));
        CHECK(has_error(*g, "schema/entities.txt:11: reference to unknown entity 'sector'"));
        CHECK(has_error(*g, "schema/entities.txt:13: entity 'nocap' needs `capacity = N`"));
        CHECK(has_error(*g, "common/defines.txt:1: invalid value 'abc' for define 'growth'"));
        CHECK(has_error(*g, "common/defines.txt:2: 'speed' is not declared in schema `defines`"));
        CHECK(has_error(*g, "history/all.txt:1: invalid value '12.5' for field 'gold'"));
        CHECK(has_error(*g, "history/all.txt:2: duplicate id 'ROM'"));
        CHECK(has_error(*g, "history/all.txt:3: unknown empire id 'XXX'"));
        CHECK(has_error(*g, "history/all.txt:4: 'planet' has no field 'weight'"));
        CHECK(has_error(*g, "history/all.txt:5: more 'planet' entities than capacity 2"));
        CHECK(has_error(*g, "history/all.txt:6: unknown entity type 'moon'"));
        CHECK(has_error(*g, "rules/stages.txt:1: stage 'grow': unknown for_each entity 'asteroid'"));
        CHECK(has_error(*g, "rules/stages.txt:1: missing rule source rules/grow.glsl"));
        CHECK(has_error(*g, "rules/stages.txt:2: clear: unknown field 'planet.nothing'"));
        CHECK(has_error(*g, "schema/entities.txt:20: modifier key 'tax': unknown applies_to entity 'galaxy'"));
        CHECK(has_error(*g, "schema/entities.txt:21: modifier key 'pop_bonus': inherit field 'pop' is not a ref field of 'planet'"));
        CHECK(has_error(*g, "common/modifiers/m.txt:1: unknown modifier key 'nonexistent'"));
        CHECK(has_error(*g, "common/modifiers/m.txt:1: invalid value 'abc' for modifier key 'pop_bonus'"));
        CHECK(has_error(*g, "history/all.txt:7: unknown modifier 'nope'"));
    }
    {  // GLSL errors in rules point at the rule file and line
        auto g = load_game(TERNI_TEST_DATA "/bad_rule");
        CHECK(g->ok());
        std::string message;
        try {
            GameSim sim(*g, 0);
        } catch (const std::exception& e) {
            message = e.what();
        }
        CHECK(message.find("rules/broken.glsl:3") != std::string::npos);
        CHECK(message.find("undefined_thing") != std::string::npos);
        if (failures) std::printf("  compile message was: %s\n", message.c_str());
    }
    {  // the example game loads cleanly
        auto g = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
        for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
        CHECK(g->ok());
        CHECK(g->tables.size() == 2 && g->tables[1].count == 5);
        CHECK(g->modifier_keys.size() == 2 && g->bundles.size() == 2 && g->initial_modifiers.size() == 2);
        CHECK(g->modifier_capacity == 1024);
    }
    std::printf("test_game: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
