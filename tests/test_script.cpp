// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Paradox script parser: one synthetic case per syntax feature found in
// HOI4 / Stellaris / EU4 / EU5 / CK3 / Victoria 3.
#include <cstdio>
#include <string>

#include "terni/script.hpp"

using namespace terni::script;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static std::unique_ptr<Document> ok(const char* src) {
    auto d = parse(src);
    if (!d->errors.empty())
        std::printf("unexpected error in [%s]: %u:%u %s\n", src, d->errors[0].line, d->errors[0].column,
                    d->errors[0].message.c_str());
    CHECK(d->errors.empty());
    return d;
}

int main() {
    {  // basics, nesting, lists, comments, quotes
        auto d = ok("# comment\nkey = value\nnested = { a = 1 b = { c = \"x # not a comment\" } }\nlist = { 1 2 3 }");
        const Block& r = d->root;
        CHECK(r.items.size() == 3);
        CHECK(r.find("key")->scalar == "value");
        CHECK(r.find("nested")->block->find("b")->block->find("c")->scalar == "x # not a comment");
        CHECK(r.find("list")->block->items.size() == 3 && !r.find("list")->block->items[1].has_key);
    }
    {  // every operator
        auto d = ok("a = 1 b == 2 c != 3 d < 4 e <= 5 f > 6 g >= 7 h ?= 8");
        const Op ops[] = {Op::Eq, Op::EqEq, Op::NotEq, Op::Less, Op::LessEq, Op::Greater, Op::GreaterEq, Op::ExistsEq};
        for (size_t i = 0; i < 8; ++i) CHECK(d->root.items[i].op == ops[i]);
    }
    {  // date and number keys, negative numbers
        auto d = ok("1444.11.11 = { owner = FRA }\n42 = { x = -1.5 }");
        CHECK(d->root.items[0].key == "1444.11.11" && d->root.items[1].key == "42");
        CHECK(d->root.items[1].block->find("x")->scalar == "-1.5");
    }
    {  // tagged blocks and GUI forms
        auto d = ok("color = rgb { 255 0 0 }\nblock \"name\" { a = b }\ntype my_button = button { size = { 1 2 } }");
        CHECK(d->root.items[0].scalar == "rgb" && d->root.items[0].block->items.size() == 3);
        // GUI `block "name" { }` = bare word `block`, then `"name" { ... }` (implicit '=')
        CHECK(d->root.items[1].scalar == "block" && !d->root.items[1].has_key);
        CHECK(d->root.items[2].key == "name" && d->root.items[2].key_quoted && d->root.items[2].op == Op::None);
        // `type my_button = button { }` = bare word `type`, then my_button = button { } (tagged)
        CHECK(d->root.items[3].scalar == "type");
        CHECK(d->root.items[4].key == "my_button" && d->root.items[4].scalar == "button" && d->root.items[4].is_block());
    }
    {  // scripted variables and inline math (incl. the escaped form)
        auto d = ok("@size = 5\nwidth = @size\nheight = @[ size * 2 ]\nother = @\\[ a + b ]");
        CHECK(d->root.items[0].key == "@size");
        CHECK(d->root.items[2].scalar == "@[ size * 2 ]");
        CHECK(d->root.items[3].scalar == "@\\[ a + b ]");
    }
    {  // script parameters and parameter conditions
        auto d = ok("effect = { add_gold = $AMOUNT$ [[CHANCE] chance = $CHANCE$ ] [[!CHANCE] chance = 100 ] }");
        const Block& b = *d->root.find("effect")->block;
        CHECK(b.find("add_gold")->scalar == "$AMOUNT$");
        CHECK(b.items[1].condition == "CHANCE" && b.items[1].block->find("chance")->scalar == "$CHANCE$");
        CHECK(b.items[2].condition == "!CHANCE");
    }
    {  // raw [[ ]] blocks (EU5 map data)
        auto d = ok("code = [[\n  local x = { 1 }\n]]\nafter = 1");
        CHECK(d->root.find("code")->scalar == "\n  local x = { 1 }\n");
        CHECK(d->root.find("after") != nullptr);
    }
    {  // '?' inside names (HOI4 constant?default, Stellaris optional scope)
        auto d = ok("cost = ENG_cost?50\nsolar_system? = { is_capital = yes }");
        CHECK(d->root.find("cost")->scalar == "ENG_cost?50");
        CHECK(d->root.find("solar_system?")->is_block());
    }
    {  // an operator as a value (CK3 script parameters)
        auto d = ok("t = { OPERATOR = <= COUNT = 0 }");
        CHECK(d->root.find("t")->block->find("OPERATOR")->scalar == "<=");
    }
    {  // quoted keys, escapes, multi-line strings, BOM
        auto d = ok("\xEF\xBB\xBF\"quoted key\" = \"say \\\"hi\\\"\nsecond line\"");
        CHECK(d->root.items[0].key == "quoted key" && d->root.items[0].key_quoted);
    }
    {  // tolerated quirks are warnings, not errors
        auto a = ok("a = { b = 1 } }");
        CHECK(a->warnings.size() == 1);
        auto b = ok("a = { b = 1");
        CHECK(b->warnings.size() == 1);
        auto c = ok("a = { force = }");
        CHECK(c->warnings.size() == 1);
    }
    {  // real errors are reported with a position
        auto d = parse("a = \"unterminated");
        CHECK(!d->errors.empty() && d->errors[0].line == 1);
    }
    std::printf("test_script: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
