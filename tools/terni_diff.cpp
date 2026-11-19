// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// terni_diff -- where two saves differ: the entity types, fields and rows, with the values.
// For a desync in a lockstep game: two peers save at the same tick, and the saves are
// compared here. Needs no game folder and no GPU.
//
//   terni_diff <a.sav> <b.sav> [--rows N]
//
// Exit code: 0 when the states are the same, 1 when they differ, 2 on an error.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "terni/game.hpp"
#include "utf8_args.hpp"

using namespace terni;

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(terni::detail::utf8_path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

const std::vector<int64_t>* values(const StateSnapshot& s, const std::string& name, uint32_t& per_row) {
    for (size_t i = 0; i < s.fields.size(); ++i)
        if (s.fields[i].first == name) {
            per_row = s.values_per_row(i);
            return &s.fields[i].second;
        }
    return nullptr;
}

std::string row_text(const std::vector<int64_t>* v, size_t row, uint32_t per_row) {
    if (!v || row * per_row >= v->size()) return "(none)";
    std::string text;
    for (size_t k = row * per_row; k < (row + 1) * per_row && k < v->size(); ++k)
        text += (text.empty() ? "" : " ") + std::to_string((*v)[k]);
    return per_row > 1 ? "[" + text + "]" : text;
}

}  // namespace

int main(int argc, char** argv) {
    terni::tools::Utf8Args utf8(argc, argv);
    argv = utf8.argv();
    std::vector<std::string> paths;
    size_t show = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rows" && i + 1 < argc) show = std::stoul(argv[++i]);
        else paths.push_back(a);
    }
    if (paths.size() != 2) {
        std::puts("usage: terni_diff <a.sav> <b.sav> [--rows N]");
        return 2;
    }
    try {
        const std::vector<uint8_t> da = read_file(paths[0]), db = read_file(paths[1]);
        const StateSnapshot a = save_snapshot(da), b = save_snapshot(db);
        std::printf("a: %s, game %016llx\nb: %s, game %016llx%s\n", paths[0].c_str(),
                    static_cast<unsigned long long>(save_checksum(da)), paths[1].c_str(),
                    static_cast<unsigned long long>(save_checksum(db)),
                    save_checksum(da) == save_checksum(db) ? "" : "  (another game: fields are matched by name)");

        // per entity type first, then each field that differs, with its first rows
        const auto ea = entity_hashes(field_hashes(a)), eb = entity_hashes(field_hashes(b));
        for (const auto& e : ea) {
            const char* verdict = "only in a";
            for (const auto& o : eb)
                if (o.name == e.name) verdict = o.hash == e.hash ? "same" : "DIFFERS";
            std::printf("  %-24s %s\n", e.name.c_str(), verdict);
        }
        for (const auto& o : eb) {
            bool in_a = false;
            for (const auto& e : ea) in_a |= e.name == o.name;
            if (!in_a) std::printf("  %-24s only in b\n", o.name.c_str());
        }
        const std::vector<FieldDifference> diffs = differences(a, b);
        if (diffs.empty()) {
            std::puts("the states are the same");
            return 0;
        }
        std::puts("");
        for (const auto& d : diffs) {
            if (d.only_in) {
                std::printf("%s: only in %s\n", d.field.c_str(), d.only_in == 1 ? "a" : "b");
                continue;
            }
            std::printf("%s: %zu %s, the first is row %zu\n", d.field.c_str(), d.rows,
                        d.rows == 1 ? "row differs" : "rows differ", d.first_row);
            uint32_t per_a = 1, per_b = 1;
            const auto* va = values(a, d.field, per_a);
            const auto* vb = values(b, d.field, per_b);
            size_t shown = 0;
            const size_t rows = std::max(va->size() / per_a, vb->size() / per_b);
            for (size_t row = d.first_row; row < rows && shown < show; ++row) {
                const std::string ta = row_text(va, row, per_a), tb = row_text(vb, row, per_b);
                if (ta == tb) continue;
                std::printf("  row %-8zu a: %s\n  %-12s b: %s\n", row, ta.c_str(), "", tb.c_str());
                ++shown;
            }
        }
        return 1;
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 2;
    }
}
