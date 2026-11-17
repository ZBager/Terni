// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Order lint: when a world is built, every stage's GLSL is scanned for fields whose final
// values can depend on the order the stage's threads run in. The GPU runs the threads of
// a stage in no fixed order, so such a stage can give different results on different
// devices, and lockstep peers drift apart even though each device is consistent with
// itself (a cross-device parity test can pass anyway).
//
// For each field buffer (<entity>_<field>[index]) the scan notes, per access, whether it
// writes (assignment, ++/--) or is an atomic*() operand, and whether the index is the
// thread's own row: the FOR_EACH variable, `v * size + k`, or a local set from those
// (`uint base = p * N;` ... `field[base + k]`). Every other index is "another row".
// A field the stage writes is reported when the stage also reads it at another row,
// writes another row without an atomic, or mixes atomic and plain accesses.
//
// It reads text, not a compiled program: accesses hidden in macros are missed, and a
// helper function's parameters count as other rows. GameSim::tick_checked() confirms
// (or rules out) a warning at run time. A line containing the comment
// `terni: order ok` is left out.

#include "order_lint.hpp"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace terni::detail {

namespace {

struct Token {
    std::string text;
    uint32_t line;
    size_t pos;  // offset in the source
};

bool id_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool id_char(char c) { return id_start(c) || (c >= '0' && c <= '9'); }

// GLSL tokens without comments and preprocessor lines; `quiet` gets the lines whose
// comments say `terni: order ok`.
std::vector<Token> lex(const std::string& s, std::set<uint32_t>& quiet) {
    static const char* const OPS[] = {"<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=",
                                      "|=",  "^=",  "==", "!=", "<=", ">=", "&&", "||", "<<", ">>"};
    std::vector<Token> out;
    uint32_t line = 1;
    bool line_start = true;
    size_t i = 0;
    const size_t n = s.size();
    auto note = [&](size_t from, size_t to, uint32_t at) {
        if (s.substr(from, to - from).find("terni: order ok") != std::string::npos) quiet.insert(at);
    };
    while (i < n) {
        const char c = s[i];
        if (c == '\n') {
            ++line, ++i, line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            const size_t from = i;
            while (i < n && s[i] != '\n') ++i;
            note(from, i, line);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const size_t from = i;
            const uint32_t at = line;
            for (i += 2; i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/'); ++i)
                if (s[i] == '\n') ++line;
            i = std::min(n, i + 2);
            note(from, i, at);
            continue;
        }
        if (c == '#' && line_start) {  // preprocessor line, with continuations
            while (i < n && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') ++line, ++i;
                ++i;
            }
            continue;
        }
        line_start = false;
        size_t j = i + 1;
        if (id_start(c) || (c >= '0' && c <= '9')) {
            while (j < n && (id_char(s[j]) || (!id_start(c) && s[j] == '.'))) ++j;
        } else {
            for (const char* op : OPS)
                if (s.compare(i, std::strlen(op), op) == 0) {
                    j = i + std::strlen(op);
                    break;
                }
        }
        out.push_back({s.substr(i, j - i), line, i});
        i = j;
    }
    return out;
}

bool is_assignment(const std::string& t) {
    return t == "=" || t == "+=" || t == "-=" || t == "*=" || t == "/=" || t == "%=" || t == "&=" || t == "|=" ||
           t == "^=" || t == "<<=" || t == ">>=";
}

struct Access {
    bool write = false, atomic = false, own = false;
    uint32_t line = 0;
    std::string index;
};

class StageScan {
public:
    StageScan(const std::string& source, const std::map<std::string, std::string>& buffers) : src_(source) {
        t_ = lex(source, quiet_);
        find_rows();
        for (size_t t = 0; t + 1 < t_.size(); ++t) {
            const auto b = buffers.find(t_[t].text);
            if (b == buffers.end() || t_[t + 1].text != "[") continue;
            const size_t close = match(t + 1);
            if (close >= t_.size()) continue;
            Access a;
            a.line = t_[t].line;
            a.own = own(t + 2, close);
            a.index = src_.substr(t_[t + 1].pos + 1, t_[close].pos - t_[t + 1].pos - 1);
            size_t after = close + 1;  // skip .x / [k] after the element
            while (after < t_.size() && (t_[after].text == "." || t_[after].text == "[")) {
                if (t_[after].text == ".") after += 2;
                else after = match(after) + 1;
            }
            const std::string next = after < t_.size() ? t_[after].text : "";
            const std::string prev = t > 0 ? t_[t - 1].text : "";
            a.write = is_assignment(next) || next == "++" || next == "--" || prev == "++" || prev == "--";
            a.atomic = t >= 2 && prev == "(" && t_[t - 2].text.rfind("atomic", 0) == 0;
            if (!quiet_.count(a.line)) accesses[b->second].push_back(a);
        }
    }

    std::map<std::string, std::vector<Access>> accesses;  // "entity.field" -> accesses, in source order

private:
    // matching ')' or ']' for the bracket at `open`
    size_t match(size_t open) const {
        int depth = 0;
        for (size_t t = open; t < t_.size(); ++t) {
            const std::string& x = t_[t].text;
            if (x == "(" || x == "[") ++depth;
            else if ((x == ")" || x == "]") && --depth == 0) return t;
        }
        return t_.size();
    }

    // Is [b, e) an index into the thread's own row? `v`, `v * n + k`, `n * v + k`,
    // `base`, `base + k`, `(own) * n + k`.
    bool own(size_t b, size_t e) const {
        while (b < e && t_[b].text == "(" && match(b) == e - 1) ++b, --e;
        if (b >= e) return false;
        const std::string& first = t_[b].text;
        const bool id = ids_.count(first) > 0, base = bases_.count(first) > 0;
        if (e - b == 1) return id || base;
        const std::string& second = t_[b + 1].text;
        if ((id && second == "*") || (base && second == "+")) return true;
        if (e - b >= 3 && second == "*" && ids_.count(t_[b + 2].text)) return true;
        if (first == "(") {
            const size_t m = match(b);
            return m + 1 < e && t_[m + 1].text == "*" && own(b + 1, m);
        }
        return false;
    }

    // Loop variables (FOR_EACH... (v)), copies of them (`uint q = v;`) and row bases
    // (`uint base = v * n;`), in source order.
    void find_rows() {
        for (size_t t = 0; t + 2 < t_.size(); ++t) {
            if (t_[t].text.rfind("FOR_EACH", 0) == 0 && t_[t + 1].text == "(" && id_start(t_[t + 2].text[0])) {
                ids_.insert(t_[t + 2].text);
                continue;
            }
            if ((t_[t].text != "uint" && t_[t].text != "int") || !id_start(t_[t + 1].text[0]) || t_[t + 2].text != "=")
                continue;
            size_t e = t + 3;  // the initializer ends at a top-level ',' or ';'
            int depth = 0;
            for (; e < t_.size(); ++e) {
                const std::string& x = t_[e].text;
                if (x == "(" || x == "[") ++depth;
                else if (x == ")" || x == "]") --depth;
                else if (depth == 0 && (x == "," || x == ";")) break;
            }
            if (!own(t + 3, e)) continue;
            if (e == t + 4 && ids_.count(t_[t + 3].text)) ids_.insert(t_[t + 1].text);
            else bases_.insert(t_[t + 1].text);
        }
    }

    const std::string& src_;
    std::vector<Token> t_;
    std::set<uint32_t> quiet_;
    std::set<std::string> ids_, bases_;
};

}  // namespace

void lint_stage_order(Game& game) {
    std::map<std::string, std::string> buffers;  // GLSL buffer name -> "entity.field"
    for (const auto& e : game.entities)
        for (const auto& f : e.fields) buffers[e.name + "_" + f.name] = e.name + "." + f.name;
    for (const auto& stage : game.stages) {
        // rules/<stage>.glsl next to stages.txt; for worlds built in code, <stage>.glsl
        std::string file = stage.name + ".glsl";
        if (const size_t slash = stage.where.file.find_last_of('/'); slash != std::string::npos)
            file = stage.where.file.substr(0, slash + 1) + file;
        const StageScan scan(stage.source, buffers);
        for (const auto& [field, list] : scan.accesses) {
            const Access* write = nullptr;
            const Access *other_read = nullptr, *other_write = nullptr, *atomic = nullptr, *plain = nullptr;
            for (const auto& a : list) {
                if ((a.write || a.atomic) && !write) write = &a;
                if (a.atomic && !atomic) atomic = &a;
                if (!a.atomic && !plain) plain = &a;
                if (!a.atomic && !a.own && !a.write && !other_read) other_read = &a;
                if (!a.atomic && !a.own && a.write && !other_write) other_write = &a;
            }
            if (!write) continue;
            const std::string where = "stage '" + stage.name + "' ";
            const std::string tail = ": the result can depend on the order the GPU runs the threads in, and differ "
                                     "between devices (confirm with GameSim::tick_checked)";
            if (other_read)
                game.warnings.push_back(
                    {{file, other_read->line},
                     where + "reads " + field + "[" + other_read->index + "], a row other threads may write in this stage (" +
                         field + " is written on line " + std::to_string(write->line) + ")" + tail});
            else if (other_write)
                game.warnings.push_back({{file, other_write->line},
                                         where + "writes " + field + "[" + other_write->index +
                                             "], a row other threads may also use, without an atomic" + tail});
            else if (atomic && plain)
                game.warnings.push_back({{file, plain->line},
                                         where + "uses " + field + " both with atomics (line " +
                                             std::to_string(atomic->line) + ") and plainly" + tail});
        }
    }
}

}  // namespace terni::detail
