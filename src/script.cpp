// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#include "terni/script.hpp"
#include "utf8_path.hpp"

#include <fstream>
#include <sstream>

namespace terni::script {

const char* op_text(Op op) {
    switch (op) {
        case Op::None: return "";
        case Op::Eq: return "=";
        case Op::EqEq: return "==";
        case Op::NotEq: return "!=";
        case Op::Less: return "<";
        case Op::LessEq: return "<=";
        case Op::Greater: return ">";
        case Op::GreaterEq: return ">=";
        case Op::ExistsEq: return "?=";
    }
    return "?";
}

const Item* Block::find(std::string_view key) const {
    for (const Item& it : items)
        if (it.has_key && it.key == key) return &it;
    return nullptr;
}

std::vector<const Item*> Block::find_all(std::string_view key) const {
    std::vector<const Item*> out;
    for (const Item& it : items)
        if (it.has_key && it.key == key) out.push_back(&it);
    return out;
}

namespace {

enum class Tok : uint8_t { End, Scalar, Quoted, Op, LBrace, RBrace, CondOpen, RBracket };

struct Token {
    Tok type = Tok::End;
    std::string_view text;
    Op op = Op::None;
    uint32_t line = 0, column = 0;
};

constexpr bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
// '=', '<', '>' always start an operator; '!' and '?' only in "!=" / "?=" — otherwise
// they are part of a word (HOI4 `constant?default`, Stellaris `solar_system? = { }`).
constexpr bool always_op(char c) { return c == '=' || c == '<' || c == '>'; }

class Parser {
public:
    explicit Parser(Document& doc) : doc_(doc), s_(doc.text) {
        if (s_.size() >= 3 && (unsigned char)s_[0] == 0xEF && (unsigned char)s_[1] == 0xBB && (unsigned char)s_[2] == 0xBF)
            pos_ = 3;  // UTF-8 BOM
    }

    void run() {
        parse_block(doc_.root, End::File, 0);
    }

private:
    enum class End { File, Brace, Bracket };

    // ---- lexer -------------------------------------------------------------
    void advance(size_t n = 1) {
        for (size_t i = 0; i < n && pos_ < s_.size(); ++i) {
            if (s_[pos_] == '\n') ++line_, col_ = 1;
            else ++col_;
            ++pos_;
        }
    }

    void skip_space_and_comments() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (is_space(static_cast<unsigned char>(c))) {
                advance();
            } else if (c == '#') {
                while (pos_ < s_.size() && s_[pos_] != '\n') advance();
            } else {
                break;
            }
        }
    }

    bool op_starts_at(size_t i) const {
        if (i >= s_.size()) return false;
        const char c = s_[i];
        return always_op(c) || ((c == '!' || c == '?') && i + 1 < s_.size() && s_[i + 1] == '=');
    }

    // A word ends at whitespace, an operator, a brace, a comment, a quote — and at
    // ']' while inside a [[PARAM] ...] block.
    bool word_ends_at(size_t i) const {
        const char c = s_[i];
        if (is_space(static_cast<unsigned char>(c)) || op_starts_at(i)) return true;
        if (c == '{' || c == '}' || c == '#' || c == '"') return true;
        return c == ']' && bracket_depth_ > 0;
    }

    Token lex() {
        skip_space_and_comments();
        Token t;
        t.line = line_;
        t.column = col_;
        if (pos_ >= s_.size()) return t;
        const size_t start = pos_;
        const char c = s_[pos_];

        if (c == '{') { advance(); t.type = Tok::LBrace; return t; }
        if (c == '}') { advance(); t.type = Tok::RBrace; return t; }
        if (c == ']' && bracket_depth_ > 0) { advance(); t.type = Tok::RBracket; return t; }

        if (c == '"') {
            advance();
            const size_t body = pos_;
            while (pos_ < s_.size() && s_[pos_] != '"') {
                if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) advance();
                advance();
            }
            if (pos_ >= s_.size()) {
                error(t.line, t.column, "unterminated quoted string");
                t.type = Tok::Quoted;
                t.text = s_.substr(body);
                return t;
            }
            t.type = Tok::Quoted;
            t.text = s_.substr(body, pos_ - body);
            advance();  // closing quote
            return t;
        }

        if (op_starts_at(pos_)) {
            advance();
            if (pos_ < s_.size() && s_[pos_] == '=') advance();  // two-char operators all end in '='
            const std::string_view o = s_.substr(start, pos_ - start);
            t.type = Tok::Op;
            t.text = o;
            if (o == "=") t.op = Op::Eq;
            else if (o == "==") t.op = Op::EqEq;
            else if (o == "!=") t.op = Op::NotEq;
            else if (o == "<") t.op = Op::Less;
            else if (o == "<=") t.op = Op::LessEq;
            else if (o == ">") t.op = Op::Greater;
            else if (o == ">=") t.op = Op::GreaterEq;
            else if (o == "?=") t.op = Op::ExistsEq;
            else error(t.line, t.column, "unknown operator '" + std::string(o) + "'");
            return t;
        }

        if (c == '[' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '[') {
            // [[PARAM] / [[!PARAM]  — script parameter condition (name, then ']')
            size_t e = pos_ + 2;
            while (e < s_.size() && s_[e] != ']' && !is_space(static_cast<unsigned char>(s_[e])) && s_[e] != '[') ++e;
            if (e > pos_ + 2 && e < s_.size() && s_[e] == ']') {
                advance(2);
                t.type = Tok::CondOpen;
                t.text = s_.substr(pos_, e - pos_);
                advance(e - pos_ + 1);
                return t;
            }
            // [[ raw text ]] — verbatim block (EU5 map data: `code = [[ ... ]]`)
            const size_t close = s_.find("]]", pos_ + 2);
            if (close == std::string_view::npos) {
                error(t.line, t.column, "unterminated [[ ]] raw block");
                advance(s_.size() - pos_);
                t.type = Tok::Quoted;
                t.text = s_.substr(start + 2);
                return t;
            }
            t.type = Tok::Quoted;
            t.text = s_.substr(pos_ + 2, close - pos_ - 2);
            advance(close + 2 - pos_);
            return t;
        }

        // @[ inline math ] / @\[ ... ] (escaped form used inside inline scripts)
        if (c == '@' && pos_ + 1 < s_.size() &&
            (s_[pos_ + 1] == '[' || (s_[pos_ + 1] == '\\' && pos_ + 2 < s_.size() && s_[pos_ + 2] == '['))) {
            int depth = 0;
            while (pos_ < s_.size()) {
                const char d = s_[pos_];
                advance();
                if (d == '[') ++depth;
                else if (d == ']' && --depth == 0) break;
            }
            if (depth != 0) error(t.line, t.column, "unterminated @[ ] expression");
            t.type = Tok::Scalar;
            t.text = s_.substr(start, pos_ - start);
            return t;
        }

        while (pos_ < s_.size() && !word_ends_at(pos_)) advance();
        t.type = Tok::Scalar;
        t.text = s_.substr(start, pos_ - start);
        return t;
    }

    const Token& peek() {
        if (!has_peek_) {
            peek_ = lex();
            has_peek_ = true;
        }
        return peek_;
    }
    Token take() {
        peek();
        has_peek_ = false;
        return peek_;
    }

    // ---- parser ------------------------------------------------------------
    void error(uint32_t line, uint32_t col, std::string msg) { doc_.errors.push_back({line, col, std::move(msg)}); }
    void warning(uint32_t line, uint32_t col, std::string msg) { doc_.warnings.push_back({line, col, std::move(msg)}); }

    static bool is_scalar(Tok t) { return t == Tok::Scalar || t == Tok::Quoted; }

    std::unique_ptr<Block> sub_block(End end, int depth) {
        auto b = std::make_unique<Block>();
        parse_block(*b, end, depth + 1);
        return b;
    }

    void parse_block(Block& block, End end, int depth) {
        if (depth > 256) {
            const Token& t = peek();
            error(t.line, t.column, "nesting deeper than 256 levels");
            return;
        }
        for (;;) {
            const Token& t = peek();
            switch (t.type) {
                case Tok::End:
                    if (end == End::Brace) warning(t.line, t.column, "block not closed at end of file");
                    if (end == End::Bracket) error(t.line, t.column, "[[PARAM] block not closed at end of file");
                    return;
                case Tok::RBrace:
                    if (end == End::Brace) { take(); return; }
                    if (end == End::File) { warning(t.line, t.column, "stray '}'"); take(); continue; }
                    error(t.line, t.column, "unexpected '}' inside [[PARAM] block");
                    take();
                    continue;
                case Tok::RBracket:
                    take();
                    if (end == End::Bracket) { --bracket_depth_; return; }
                    continue;  // unreachable: ']' is only a token inside brackets
                case Tok::CondOpen: {
                    Item it;
                    it.line = t.line, it.column = t.column;
                    it.condition = take().text;
                    ++bracket_depth_;
                    it.block = sub_block(End::Bracket, depth);
                    block.items.push_back(std::move(it));
                    ++doc_.item_count;
                    continue;
                }
                case Tok::LBrace: {
                    Item it;
                    it.line = t.line, it.column = t.column;
                    take();
                    it.block = sub_block(End::Brace, depth);
                    block.items.push_back(std::move(it));
                    ++doc_.item_count;
                    continue;
                }
                case Tok::Op: {
                    Token o = take();
                    error(o.line, o.column, "operator '" + std::string(o.text) + "' without a key");
                    continue;
                }
                case Tok::Scalar:
                case Tok::Quoted:
                    parse_item(block, depth);
                    continue;
            }
        }
    }

    void parse_item(Block& block, int depth) {
        Token first = take();
        Item it;
        it.line = first.line, it.column = first.column;
        const Token& next = peek();
        if (next.type == Tok::Op) {
            it.has_key = true;
            it.key = first.text;
            it.key_quoted = first.type == Tok::Quoted;
            Token o = take();
            it.op = o.op;
            parse_value(it, o, depth);
        } else if (next.type == Tok::LBrace) {
            // `key { ... }` with an implicit '=' (GUI: `block "name" { }`, `color { 1 2 3 }`)
            it.has_key = true;
            it.key = first.text;
            it.key_quoted = first.type == Tok::Quoted;
            take();
            it.block = sub_block(End::Brace, depth);
        } else {
            it.scalar = first.text;  // bare value (list element / word)
            it.scalar_quoted = first.type == Tok::Quoted;
        }
        block.items.push_back(std::move(it));
        ++doc_.item_count;
    }

    void parse_value(Item& it, const Token& op, int depth) {
        const Token& v = peek();
        if (v.type == Tok::LBrace) {
            take();
            it.block = sub_block(End::Brace, depth);
        } else if (v.type == Tok::Op) {
            // an operator as a value: CK3 passes them as script parameters (`OPERATOR = <=`)
            Token s = take();
            it.scalar = s.text;
        } else if (is_scalar(v.type)) {
            Token s = take();
            it.scalar = s.text;
            it.scalar_quoted = s.type == Tok::Quoted;
            if (peek().type == Tok::LBrace) {  // tagged block: rgb { ... }, widget { ... }
                take();
                it.block = sub_block(End::Brace, depth);
            }
        } else {
            // `key =` directly followed by '}' or end of file: the games read an empty value
            warning(op.line, op.column, "missing value after '" + std::string(op.text) + "'");
        }
    }

    Document& doc_;
    std::string_view s_;
    size_t pos_ = 0;
    uint32_t line_ = 1, col_ = 1;
    int bracket_depth_ = 0;
    Token peek_;
    bool has_peek_ = false;
};

}  // namespace

std::unique_ptr<Document> parse(std::string text, std::string path) {
    auto doc = std::make_unique<Document>();
    doc->path = std::move(path);
    doc->text = std::move(text);
    Parser(*doc).run();
    return doc;
}

std::unique_ptr<Document> parse_file(const std::string& path) {
    std::ifstream f(detail::utf8_path(path), std::ios::binary);
    if (!f) return nullptr;
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(std::move(ss).str(), path);
}

}  // namespace terni::script
