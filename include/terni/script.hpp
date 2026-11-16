// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
#pragma once
// Parser for Paradox script ("Clausewitz"/"Jomini" syntax) — the format of
// common/, history/, events/, gui/ ... files in HOI4, Stellaris, EU4, EU5, CK3.
//
// The parser is structural and lossless about meaning: it does not interpret
// keys, it only builds a tree that later layers (schema, history, defines)
// read. Supported syntax:
//
//   key = value            key = { ... }          { a b c }      (lists)
//   key < value  >  <=  >=  !=  ==  ?=                           (operators)
//   1444.11.11 = { }       42 = { }                              (date / number keys)
//   color = rgb { 1 2 3 }  block "name" { }  type x = widget { } (tagged blocks)
//   @var = 5   key = @var   key = @[ var * 2 ]                   (scripted values / inline math)
//   $PARAM$   [[PARAM] ... ]   [[!PARAM] ... ]                   (script parameters)
//   "quoted \"strings\"" spanning lines, # comments, UTF-8 BOM, any 8-bit encoding
//
//   [[ raw text ]]   OPERATOR = <=   constant?default   scope? = { }  (raw blocks, operator values, optional names)
//
// Like the games, it tolerates a stray '}', blocks left open at end of file and
// `key =` without a value (reported as warnings). Everything else malformed is an
// error with line:column.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace terni::script {

enum class Op : uint8_t { None, Eq, EqEq, NotEq, Less, LessEq, Greater, GreaterEq, ExistsEq };
const char* op_text(Op op);

struct Block;

// One item of a block. Exactly one of these shapes:
//   key op value          (has_key, op != None)
//   key { ... }           (has_key, op == None, block) — implicit '=' (e.g. GUI `block "x" { }`)
//   value                 (!has_key)                   — list element, bare word or bare block
// A value is a scalar, a block, or a scalar tag followed by a block (`rgb { }`).
// Conditional parameter blocks `[[PARAM] ...]` are items with `condition` set.
struct Item {
    std::string_view key;      // raw text (quotes stripped, escapes kept) — valid while the Document lives
    bool has_key = false;
    bool key_quoted = false;
    Op op = Op::None;
    std::string_view scalar;   // scalar value or tag of a tagged block
    bool scalar_quoted = false;
    std::unique_ptr<Block> block;  // block value (also for tagged blocks and conditionals)
    std::string_view condition;    // "[[PARAM]" -> "PARAM", "[[!PARAM]" -> "!PARAM"
    uint32_t line = 0, column = 0;

    bool is_block() const { return block != nullptr; }
};

struct Block {
    std::vector<Item> items;
    // convenience lookups (linear; files are small)
    const Item* find(std::string_view key) const;
    std::vector<const Item*> find_all(std::string_view key) const;
};

struct Diagnostic {
    uint32_t line, column;
    std::string message;
};

// A parsed file. Owns the source text; all string_views point into it.
struct Document {
    std::string path;
    std::string text;
    Block root;
    std::vector<Diagnostic> errors;    // the file did not parse as the game would read it
    std::vector<Diagnostic> warnings;  // tolerated quirks (stray '}', unclosed block at EOF)
    size_t item_count = 0;
};

// Parse text (path is only used in messages). Never throws on bad input.
std::unique_ptr<Document> parse(std::string text, std::string path = "<memory>");
// Read and parse a file. Returns nullptr only if the file can't be read.
std::unique_ptr<Document> parse_file(const std::string& path);

}  // namespace terni::script
