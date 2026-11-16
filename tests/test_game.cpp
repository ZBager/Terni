// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// Data-driven game loading: error reporting, value conversion, rule compile errors.
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

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

#ifdef __SIZEOF_INT128__
// The __int128 version parse_value had before it became portable: the reference the
// portable one must match exactly (GCC and Clang only).
__extension__ typedef __int128 i128;
static bool reference_parse_value(const std::string& text, FieldType type, int64_t& out) {
    if (type == FieldType::Ref) return false;
    if (text == "yes" || text == "no") {
        if (type == FieldType::Quantity || type == FieldType::Rate) return false;
        out = text == "yes" ? 1 : 0;
        return true;
    }
    size_t i = 0;
    bool neg = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) neg = text[i++] == '-';
    i128 mant = 0, scale = 1;
    bool dot = false;
    int digits = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        if (++digits > 30) return false;
        mant = mant * 10 + (c - '0');
        if (dot) scale *= 10;
    }
    if (digits == 0) return false;
    const int frac_bits = type == FieldType::Quantity ? fx::QF : type == FieldType::Rate ? fx::RF : 0;
    if (frac_bits == 0 && scale != 1) return false;
    i128 raw = (mant << frac_bits) / scale;
    if (neg) raw = -raw;
    if (type == FieldType::Count && raw < 0) return false;
    const i128 lo = type == FieldType::Money64 ? i128(INT64_MIN) : i128(INT32_MIN);
    const i128 hi = type == FieldType::Money64 ? i128(INT64_MAX)
                    : type == FieldType::Count ? i128(UINT32_MAX)
                                               : i128(INT32_MAX);
    if (raw < lo || raw > hi) return false;
    out = static_cast<int64_t>(raw);
    return true;
}

static void compare_with_reference() {
    std::vector<std::string> inputs = {
        "", "-", "+", ".", "-.", "0", "-0", "+0", "0.", ".5", "-.5", "1.2.3", "1e5", "yes", "no", " 1", "1 ",
        "2147483647", "2147483648", "-2147483648", "-2147483649", "4294967295", "4294967296",
        "9223372036854775807", "9223372036854775808", "-9223372036854775808", "-9223372036854775809",
        "8388607.99609375", "8388607.996094", "8388608", "-8388608", "-8388608.00390625",
        "32767.9999847412109375", "32767.99999", "32768", "-32768", "-32768.0000152587890625",
        "0.000015258789", "0.0000152587890625", "-0.0000152587890625", "0.00390625", "-0.0039",
        "999999999999999999999999999999", "9999999999999999999999999999999",
        "0.00000000000000000000000000001", "-0.00000000000000000000000000001",
        "000000000000000000000000000001", "35184372088832", "18446744073709551616",
    };
    uint64_t state = 0x9e3779b97f4a7c15ULL;
    const auto next = [&] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return uint32_t(state >> 33);
    };
    for (int n = 0; n < 200000; ++n) {
        std::string s;
        if (next() % 4 == 0) s += next() % 2 ? '-' : '+';
        const uint32_t len = 1 + next() % 32, dot = next() % (len + 4);
        for (uint32_t k = 0; k < len; ++k) {
            if (k == dot) s += '.';
            s += char('0' + (next() % 3 == 0 ? next() % 2 * 9 : next() % 10));  // many 0s and 9s
        }
        inputs.push_back(std::move(s));
    }
    int mismatches = 0;
    for (const auto& s : inputs)
        for (FieldType t : {FieldType::Quantity, FieldType::Rate, FieldType::Agg, FieldType::Int, FieldType::Count,
                            FieldType::Ref, FieldType::Money64}) {
            int64_t a = 0x5a5a, b = 0x5a5a;
            const bool ok_a = parse_value(s, t, a), ok_b = reference_parse_value(s, t, b);
            if (ok_a != ok_b || (ok_a && a != b)) {
                if (++mismatches <= 10)
                    std::printf("  parse_value(\"%s\", type %d): %d %lld, reference %d %lld\n", s.c_str(), int(t),
                                int(ok_a), (long long)a, int(ok_b), (long long)b);
            }
        }
    CHECK(mismatches == 0);
}
#endif

static bool has_error(const Game& g, const std::string& text) {
    for (const auto& e : g.errors)
        if (e.text().find(text) != std::string::npos) return true;
    std::printf("  missing expected error: %s\n", text.c_str());
    return false;
}

static int warnings_with(const Game& g, const std::string& text) {
    int n = 0;
    for (const auto& w : g.warnings) n += w.text().find(text) != std::string::npos;
    return n;
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
        CHECK(parse_value("-0", FieldType::Count, v) && v == 0);
        CHECK(parse_value("-9223372036854775808", FieldType::Money64, v) && v == INT64_MIN);
        CHECK(!parse_value("9223372036854775808", FieldType::Money64, v));
        CHECK(parse_value("-2147483648", FieldType::Int, v) && v == INT32_MIN);
        CHECK(parse_value("4294967295", FieldType::Count, v) && v == UINT32_MAX);
        CHECK(parse_value("0.00000000000000000000000000001", FieldType::Rate, v) && v == 0);
        CHECK(!parse_value("9999999999999999999999999999999", FieldType::Money64, v));  // 31 digits
#ifdef __SIZEOF_INT128__
        compare_with_reference();
#endif
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
        CHECK(warnings_with(*g, "has no value") == 0);  // 'growth = abc' is an error, not also a warning
    }
    {  // a define that is never given a value is 0, with a warning
        WorldBuilder b;
        b.define("given", FieldType::Rate);
        b.define("forgotten", FieldType::Int, {"schema/x.txt", 4});
        b.set_define("given", fx::rate(1, 2));
        auto g = b.finish();
        CHECK(g->ok() && g->define("forgotten") == 0);
        CHECK(warnings_with(*g, "schema/x.txt:4: define 'forgotten' has no value, so it is 0") == 1);
        CHECK(warnings_with(*g, "'given'") == 0);

        namespace fs = std::filesystem;  // the same from a data folder, at the schema line
        const fs::path root = fs::path(TERNI_TEST_BINARY_DIR) / "define_without_value";
        fs::remove_all(root);
        fs::create_directories(root / "schema");
        fs::create_directories(root / "common");
        std::ofstream(root / "schema" / "entities.txt") << "town = { capacity = 4 pop = int }\n"
                                                           "defines = {\n    tax = rate\n    levy = int\n}\n";
        std::ofstream(root / "common" / "defines.txt") << "tax = 0.1\n";
        auto loaded = load_game(root.string());
        CHECK(loaded->ok());
        CHECK(warnings_with(*loaded, "schema/entities.txt:4: define 'levy' has no value, so it is 0") == 1);
        CHECK(warnings_with(*loaded, "'tax'") == 0);
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
    {  // define values must fit their 32-bit type (code-built worlds skip the loader's checks)
        WorldBuilder b;
        b.define("chance", FieldType::Rate);
        b.define("slots", FieldType::Count);
        b.set_define("chance", int64_t(INT32_MAX) + 1);
        b.set_define("slots", -1);
        b.set_define("slots", int64_t(UINT32_MAX));
        auto g = b.finish();
        CHECK(g->errors.size() == 2 && g->errors[0].message.find("'chance'") != std::string::npos);
        CHECK(g->define("chance") == 0 && uint32_t(g->define("slots")) == UINT32_MAX);
    }
    {  // the example game loads cleanly
        auto g = load_game(TERNI_SOURCE_DIR "/examples/mini_galaxy");
        for (const auto& e : g->errors) std::printf("  %s\n", e.text().c_str());
        CHECK(g->ok());
        CHECK(g->tables.size() == 2 && g->tables[1].count == 5);
        CHECK(g->modifier_keys.size() == 2 && g->bundles.size() == 2 && g->initial_modifiers.size() == 2);
        CHECK(g->modifier_capacity == 1024);

        int64_t growth = 0;  // defines by name: the raw word rules read as D.<name>
        CHECK(parse_value("0.02", FieldType::Rate, growth));
        CHECK(g->define("growth_rate") == growth && growth == 1310);
        CHECK(g->define_index("tax_rate") == 1 && g->define_index("no_such_define") == -1);
        bool threw = false;
        try {
            (void)g->define("no_such_define");
        } catch (const std::invalid_argument& e) {
            threw = std::string(e.what()).find("no_such_define") != std::string::npos;
        }
        CHECK(threw);
    }
    std::printf("test_game: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
