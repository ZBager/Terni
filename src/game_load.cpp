// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Loading a Paradox-syntax game folder through WorldBuilder:
//   schema/  common/  common/modifiers/  history/  rules/

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "terni/fixed.hpp"
#include "terni/game.hpp"
#include "terni/script.hpp"

namespace fs = std::filesystem;

namespace terni {

// ---- exact decimal -> fixed point -------------------------------------------
namespace {

// Unsigned 128-bit integer as four 32-bit limbs, least significant first. Standard C++
// on every compiler; parse_value needs at most 30 decimal digits (< 2^100) shifted
// left by 16 bits, so nothing here can overflow.
struct U128 {
    uint32_t limb[4] = {};

    void mul_add(uint32_t m, uint32_t add) {  // *this = *this * m + add
        uint64_t carry = add;
        for (uint32_t& l : limb) {
            const uint64_t t = uint64_t(l) * m + carry;
            l = uint32_t(t);
            carry = t >> 32;
        }
    }
    void div(uint32_t d) {  // *this = floor(*this / d)
        uint64_t rem = 0;
        for (int i = 3; i >= 0; --i) {
            const uint64_t cur = (rem << 32) | limb[i];
            limb[i] = uint32_t(cur / d);
            rem = cur % d;
        }
    }
    bool fits_u64() const { return limb[2] == 0 && limb[3] == 0; }
    uint64_t low64() const { return (uint64_t(limb[1]) << 32) | limb[0]; }
};

}  // namespace

bool parse_value(const std::string& text, FieldType type, int64_t& out) {
    if (type == FieldType::Ref) return false;
    if (text == "yes" || text == "no") {
        if (type == FieldType::Quantity || type == FieldType::Rate) return false;
        out = text == "yes" ? 1 : 0;
        return true;
    }
    size_t i = 0;
    bool neg = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) neg = text[i++] == '-';
    U128 mag;  // the magnitude; the sign is applied at the end
    bool dot = false;
    int digits = 0, frac_digits = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        if (++digits > 30) return false;  // far beyond any representable value
        mag.mul_add(10, uint32_t(c - '0'));
        if (dot) ++frac_digits;
    }
    if (digits == 0) return false;
    const int frac_bits = type == FieldType::Quantity ? fx::QF : type == FieldType::Rate ? fx::RF : 0;
    if (frac_bits == 0 && frac_digits > 0) return false;  // integers only for agg/int/count/money64
    // (mant << frac_bits) / 10^frac_digits, truncated toward zero: identical on every
    // machine. Dividing by 10 one step at a time gives the same floor as one division.
    mag.mul_add(1u << frac_bits, 0);
    for (int k = 0; k < frac_digits; ++k) mag.div(10);
    if (!mag.fits_u64()) return false;
    const uint64_t m = mag.low64();
    const uint64_t limit = type == FieldType::Money64 ? (neg ? uint64_t(1) << 63 : uint64_t(INT64_MAX))
                           : type == FieldType::Count ? (neg ? 0 : uint64_t(UINT32_MAX))
                                                      : (neg ? uint64_t(1) << 31 : uint64_t(INT32_MAX));
    if (m > limit) return false;
    out = neg ? static_cast<int64_t>(0 - m) : static_cast<int64_t>(m);  // modular conversion (C++20): 2^63 -> INT64_MIN
    return true;
}

namespace {

bool parse_type(const std::string& s, FieldType& t, std::string& ref) {
    if (s == "quantity") t = FieldType::Quantity;
    else if (s == "rate") t = FieldType::Rate;
    else if (s == "agg") t = FieldType::Agg;
    else if (s == "int") t = FieldType::Int;
    else if (s == "count") t = FieldType::Count;
    else if (s == "money64") t = FieldType::Money64;
    else if (s.rfind("ref:", 0) == 0) {
        t = FieldType::Ref;
        ref = s.substr(4);
    } else
        return false;
    return true;
}

using Docs = std::vector<std::unique_ptr<script::Document>>;

class Loader {
public:
    // The game's folder, then mods: a file with the same path in a later folder replaces it.
    Loader(WorldBuilder& b, std::vector<std::string> roots) : b_(b), roots_(std::move(roots)) {}

    void run() {
        Docs schema = read_folder("schema");
        for (auto& d : schema) load_schema_types(*d);   // entities, defines, settings
        for (auto& d : schema) load_modifier_keys(*d);  // keys may name entities from any schema file
        for (auto& d : read_folder("common")) load_defines(*d);
        for (auto& d : read_folder("common/modifiers")) load_bundles(*d);
        Docs history = read_folder("history");
        for (auto& d : history) create_history_rows(*d);  // pass 1: rows + ids (references may point forward)
        for (auto& d : history) fill_history_rows(*d);    // pass 2: values
        bool have_stages = false;
        for (auto& d : read_folder("rules"))
            if (fs::path(d->path).filename() == "stages.txt") {
                load_stages(*d);
                have_stages = true;
            }
        if (!have_stages) warn({"rules/stages.txt", 0}, "no stages defined");
    }

private:
    Game& g() { return b_.game(); }
    // A file's path in messages: relative to its folder, which is named too when there are mods.
    std::string rel(const std::string& path) const {
        const std::string generic = fs::path(path).generic_string();
        for (auto r = roots_.rbegin(); r != roots_.rend(); ++r) {
            const std::string root = fs::path(*r).generic_string();
            if (generic.size() > root.size() && generic.compare(0, root.size(), root) == 0 && generic[root.size()] == '/') {
                const std::string inside = generic.substr(root.size() + 1);
                return roots_.size() > 1 ? fs::path(*r).filename().generic_string() + "/" + inside : inside;
            }
        }
        return generic;
    }
    Location at(const script::Document& d, const script::Item& it) const { return {rel(d.path), it.line}; }
    void error(Location l, std::string m) { g().errors.push_back({std::move(l), std::move(m)}); }
    void warn(Location l, std::string m) { g().warnings.push_back({std::move(l), std::move(m)}); }
    static std::string str(std::string_view s) { return std::string(s); }

    Docs read_folder(const std::string& name) {
        Docs docs;
        std::map<std::string, fs::path> by_name;  // file name -> the last folder's copy; sorted: a fixed load order
        for (const auto& root : roots_) {
            const fs::path dir = fs::path(root) / name;
            if (!fs::is_directory(dir)) continue;
            for (const auto& e : fs::directory_iterator(dir))
                if (e.is_regular_file() && e.path().extension() == ".txt") by_name[e.path().filename().string()] = e.path();
        }
        for (const auto& [file_name, f] : by_name) {
            auto doc = script::parse_file(f.string());
            if (!doc) {
                error({rel(f.string()), 0}, "cannot read file");
                continue;
            }
            for (const auto& e : doc->errors) error({rel(doc->path), e.line}, e.message);
            for (const auto& w : doc->warnings) warn({rel(doc->path), w.line}, w.message);
            docs.push_back(std::move(doc));
        }
        return docs;
    }

    // ---- schema ------------------------------------------------------------------
    void load_schema_types(const script::Document& d) {
        for (const auto& it : d.root.items) {
            if (!it.has_key || !it.is_block()) {
                error(at(d, it), "expected `name = { ... }`");
                continue;
            }
            if (it.key == "modifier_keys") continue;  // second pass
            if (it.key == "modifiers") {
                if (const auto* c = it.block->find("capacity")) {
                    int64_t n = 0;
                    if (!parse_value(str(c->scalar), FieldType::Count, n) || n == 0) error(at(d, *c), "capacity must be a positive integer");
                    else b_.modifier_capacity(static_cast<uint32_t>(n));
                }
                continue;
            }
            if (it.key == "defines") {
                for (const auto& f : it.block->items) {
                    FieldType t;
                    std::string ref;
                    if (!f.has_key || f.is_block()) error(at(d, f), "expected `name = type` in defines");
                    else if (!parse_type(str(f.scalar), t, ref)) error(at(d, f), "unknown define type '" + str(f.scalar) + "'");
                    else b_.define(str(f.key), t, at(d, f));
                }
                continue;
            }
            // entity: capacity first, then fields
            int64_t capacity = 0;
            if (const auto* c = it.block->find("capacity"))
                if (!parse_value(str(c->scalar), FieldType::Count, capacity)) error(at(d, *c), "capacity must be a positive integer");
            const uint32_t e = b_.entity(str(it.key), static_cast<uint32_t>(capacity), at(d, it));
            for (const auto& f : it.block->items) {
                if (!f.has_key) {
                    error(at(d, f), "expected `field = type` in entity '" + str(it.key) + "'");
                    continue;
                }
                if (f.key == "capacity") continue;
                if (f.key == "dynamic" && !f.is_block()) {  // dynamic = yes
                    if (f.scalar == "yes") b_.entity_dynamic(e, at(d, f));
                    else if (f.scalar != "no") error(at(d, f), "expected `dynamic = yes`");
                    continue;
                }
                if (f.key == "spawn" && f.is_block() && !f.block->find("type")) {  // spawn = { from = .. count = .. }
                    const auto* from = f.block->find("from");
                    const auto* count = f.block->find("count");
                    const auto* parent = f.block->find("parent");
                    const auto* ordinal = f.block->find("ordinal");
                    const auto* ev = f.block->find("every");
                    const auto* of = f.block->find("offset");
                    SpawnOptions options;
                    options.parent_field = parent ? str(parent->scalar) : "";
                    options.ordinal_field = ordinal ? str(ordinal->scalar) : "";
                    int64_t every = 1, offset = 0;
                    bool ok = true;
                    if (ev && (!parse_value(str(ev->scalar), FieldType::Count, every) || every < 1)) {
                        error(at(d, *ev), "every must be a whole number of ticks, at least 1, not '" + str(ev->scalar) + "'");
                        ok = false;
                    }
                    if (of && (!parse_value(str(of->scalar), FieldType::Count, offset) || offset < 0)) {
                        error(at(d, *of), "offset must be a whole number of ticks, not '" + str(of->scalar) + "'");
                        ok = false;
                    }
                    options.every = static_cast<uint32_t>(every);
                    options.offset = static_cast<uint32_t>(offset);
                    if (!from || !count) error(at(d, f), "expected `spawn = { from = entity count = field }`");
                    else if (ok) b_.spawn(e, str(from->scalar), str(count->scalar), options, at(d, f));
                    continue;
                }
                std::string type_text = str(f.scalar), default_text;
                uint32_t size = 1;
                if (f.is_block() && f.scalar.empty()) {
                    const auto* t = f.block->find("type");
                    type_text = t ? str(t->scalar) : "";
                    if (const auto* s = f.block->find("size")) {
                        int64_t n = 0;
                        if (!parse_value(str(s->scalar), FieldType::Count, n) || n == 0) error(at(d, *s), "size must be a positive integer");
                        else size = static_cast<uint32_t>(n);
                    }
                    if (const auto* df = f.block->find("default")) default_text = str(df->scalar);
                }
                FieldType type;
                std::string ref;
                if (!parse_type(type_text, type, ref)) {
                    error(at(d, f), "unknown field type '" + type_text + "'");
                    continue;
                }
                int64_t def = 0;
                if (!default_text.empty() && !parse_value(default_text, type, def))
                    error(at(d, f), "invalid default '" + default_text + "' for " + type_text);
                b_.field(e, str(f.key), type, ref, size, def, at(d, f));
            }
        }
    }

    void load_modifier_keys(const script::Document& d) {
        for (const auto& it : d.root.items) {
            if (!it.has_key || it.key != "modifier_keys" || !it.is_block()) continue;
            for (const auto& k : it.block->items) {
                if (!k.has_key || !k.is_block()) {
                    error(at(d, k), "expected `key = { applies_to = entity type = rate }`");
                    continue;
                }
                const auto* applies = k.block->find("applies_to");
                const auto* type = k.block->find("type");
                const auto* inherit = k.block->find("inherit");
                const int e = applies ? g().entity_index(str(applies->scalar)) : -1;
                if (e < 0) {
                    error(at(d, k), "modifier key '" + str(k.key) + "': unknown applies_to entity '" +
                                        (applies ? str(applies->scalar) : "") + "'");
                    continue;
                }
                FieldType t = FieldType::Rate;
                std::string ref;
                if (type && !parse_type(str(type->scalar), t, ref)) {
                    error(at(d, *type), "unknown modifier key type '" + str(type->scalar) + "'");
                    continue;
                }
                // `inherit = owner`, or `inherit = { district region realm }` for several steps
                std::string path = inherit && !inherit->is_block() ? str(inherit->scalar) : "";
                if (inherit && inherit->is_block())
                    for (const auto& step : inherit->block->items)
                        path += (path.empty() ? "" : ".") + str(step.scalar);
                b_.modifier_key(str(k.key), static_cast<uint32_t>(e), t, path, at(d, k));
            }
        }
    }

    // ---- common --------------------------------------------------------------------
    void load_defines(const script::Document& d) {
        for (const auto& it : d.root.items) {
            const std::string name = str(it.key);
            const DefineDef* def = nullptr;
            for (const auto& x : g().defines)
                if (x.name == name) def = &x;
            if (!it.has_key || !def) {
                b_.set_define(name, 0, at(d, it));  // reports "not declared"
                continue;
            }
            int64_t v = 0;
            if (!parse_value(str(it.scalar), def->type, v)) {
                error(at(d, it), "invalid value '" + str(it.scalar) + "' for define '" + name + "'");
                v = 0;  // still given, so the error is not followed by a "has no value" warning
            }
            b_.set_define(name, v, at(d, it));
        }
    }

    void load_bundles(const script::Document& d) {
        for (const auto& it : d.root.items) {
            if (!it.has_key || !it.is_block()) {
                error(at(d, it), "expected `modifier_name = { key = value ... }`");
                continue;
            }
            std::vector<std::pair<uint32_t, int32_t>> values;
            for (const auto& kv : it.block->items) {
                const int k = kv.has_key ? g().modifier_key_index(str(kv.key)) : -1;
                if (k < 0) {
                    error(at(d, kv), "unknown modifier key '" + str(kv.key) + "'");
                    continue;
                }
                int64_t v = 0;
                if (!parse_value(str(kv.scalar), g().modifier_keys[size_t(k)].type, v))
                    error(at(d, kv), "invalid value '" + str(kv.scalar) + "' for modifier key '" + str(kv.key) + "'");
                else
                    values.push_back({static_cast<uint32_t>(k), static_cast<int32_t>(v)});
            }
            b_.bundle(str(it.key), values, at(d, it));
        }
    }

    // ---- history -------------------------------------------------------------------
    void create_history_rows(const script::Document& d) {
        for (const auto& it : d.root.items) {
            const int e = it.has_key ? g().entity_index(str(it.key)) : -1;
            if (e < 0 || !it.is_block()) {
                error(at(d, it), "unknown entity type '" + str(it.key) + "'");
                rows_.push_back({-1, 0});
                continue;
            }
            const auto* id = it.block->find("id");
            rows_.push_back({e, b_.create(static_cast<uint32_t>(e), id ? str(id->scalar) : "", at(d, it))});
        }
    }

    void fill_history_rows(const script::Document& d) {
        for (const auto& it : d.root.items) {
            const auto [e, row] = rows_[next_row_++];
            if (e < 0) continue;
            const EntityDef& def = g().entities[size_t(e)];
            for (const auto& f : it.block->items) {
                if (f.has_key && f.key == "id") continue;
                if (f.has_key && f.key == "add_modifier" && f.is_block()) {
                    history_modifier(d, f, static_cast<uint32_t>(e), row);
                    continue;
                }
                const FieldDef* fd = f.has_key ? def.field(str(f.key)) : nullptr;
                if (!fd) {
                    b_.set(static_cast<uint32_t>(e), row, str(f.key), 0, 0, at(d, f));  // reports "no field"
                    continue;
                }
                std::vector<std::string> values;
                if (f.is_block() && f.scalar.empty())
                    for (const auto& v : f.block->items) values.push_back(str(v.scalar));
                else
                    values.push_back(str(f.scalar));
                if (values.size() != fd->size) {
                    error(at(d, f), "field '" + fd->name + "' needs " + std::to_string(fd->size) + " value(s)");
                    continue;
                }
                for (uint32_t k = 0; k < fd->size; ++k) {
                    int64_t raw = 0;
                    if (fd->type == FieldType::Ref) {
                        const int target = g().entity_index(fd->ref_entity);
                        const auto* ids = target >= 0 ? &g().tables[size_t(target)].ids : nullptr;
                        const auto found = ids ? ids->find(values[k]) : decltype(ids->find(values[k])){};
                        if (!ids || found == ids->end()) {
                            error(at(d, f), "unknown " + fd->ref_entity + " id '" + values[k] + "'");
                            continue;
                        }
                        raw = found->second;
                    } else if (!parse_value(values[k], fd->type, raw)) {
                        error(at(d, f), "invalid value '" + values[k] + "' for field '" + fd->name + "'");
                        continue;
                    }
                    b_.set(static_cast<uint32_t>(e), row, fd->name, raw, k, at(d, f));
                }
            }
        }
    }

    // add_modifier = { name = war_economy scale = 1 duration = 12 }   (Paradox: add_country_modifier)
    void history_modifier(const script::Document& d, const script::Item& f, uint32_t e, uint32_t row) {
        const auto* name = f.block->find("name");
        const int bundle = name ? g().bundle_index(str(name->scalar)) : -1;
        if (bundle < 0) {
            error(at(d, f), "unknown modifier '" + (name ? str(name->scalar) : "") + "'");
            return;
        }
        int64_t scale = fx::ONE, duration = 0;
        if (const auto* s = f.block->find("scale"); s && !parse_value(str(s->scalar), FieldType::Rate, scale))
            error(at(d, *s), "invalid scale '" + str(s->scalar) + "'");
        if (const auto* du = f.block->find("duration"); du && !parse_value(str(du->scalar), FieldType::Count, duration))
            error(at(d, *du), "invalid duration '" + str(du->scalar) + "' (whole ticks, 0 = permanent)");
        b_.add_modifier(static_cast<uint32_t>(bundle), e, row, static_cast<int32_t>(scale), static_cast<uint32_t>(duration));
    }

    // ---- rules ---------------------------------------------------------------------
    void load_stages(const script::Document& d) {
        for (const auto& it : d.root.items) {
            if (!it.has_key || !it.is_block()) {
                error(at(d, it), "expected `stage_name = { for_each = entity }`");
                continue;
            }
            const std::string name = str(it.key);
            const auto* fe = it.block->find("for_each");
            std::vector<std::string> clear;
            if (const auto* cl = it.block->find("clear"); cl && cl->is_block())
                for (const auto& c : cl->block->items) clear.push_back(str(c.scalar));
            std::string source;
            fs::path src = fs::path(roots_.front()) / "rules" / (name + ".glsl");
            for (const auto& root : roots_)  // the last folder that has it
                if (fs::is_regular_file(fs::path(root) / "rules" / (name + ".glsl"))) src = fs::path(root) / "rules" / (name + ".glsl");
            std::ifstream f(src, std::ios::binary);
            if (!f) {
                error(at(d, it), "missing rule source rules/" + name + ".glsl");
            } else {
                std::ostringstream ss;
                ss << f.rdbuf();
                source = ss.str();
            }
            b_.stage(name, fe ? str(fe->scalar) : "", source, clear, at(d, it));
            for (auto& st : g().stages)
                if (st.name == name && st.source_file.empty()) st.source_file = rel(src.string());
            if (const auto* dp = it.block->find("dispatch")) {
                const std::string v = str(dp->scalar);
                if (v == "workgroup") b_.stage_dispatch(name, Dispatch::Workgroup, at(d, *dp));
                else if (v != "thread") error(at(d, *dp), "dispatch must be `thread` or `workgroup`, not '" + v + "'");
            }
            const auto* ev = it.block->find("every");
            const auto* of = it.block->find("offset");
            if (ev || of) {
                int64_t every = 1, offset = 0;
                bool good = true;
                if (ev && (!parse_value(str(ev->scalar), FieldType::Count, every) || every < 1)) {
                    error(at(d, *ev), "every must be a whole number of ticks, at least 1, not '" + str(ev->scalar) + "'");
                    good = false;
                }
                if (of && (!parse_value(str(of->scalar), FieldType::Count, offset) || offset < 0)) {
                    error(at(d, *of), "offset must be a whole number of ticks, not '" + str(of->scalar) + "'");
                    good = false;
                }
                if (good)
                    b_.stage_schedule(name, static_cast<uint32_t>(every), static_cast<uint32_t>(offset),
                                      at(d, of ? *of : *ev));
            }
        }
    }

    WorldBuilder& b_;
    std::vector<std::string> roots_;
    std::vector<std::pair<int, uint32_t>> rows_;  // history items -> (entity, row)
    size_t next_row_ = 0;
};

}  // namespace

std::unique_ptr<Game> load_game(const std::string& root) { return load_game(std::vector<std::string>{root}); }

std::unique_ptr<Game> load_game(std::initializer_list<std::string> roots) {
    return load_game(std::vector<std::string>(roots));
}

std::unique_ptr<Game> load_game(const std::vector<std::string>& roots) {
    if (roots.empty()) throw std::invalid_argument("load_game: no folder");
    std::vector<std::string> abs;
    for (const auto& r : roots) {
        fs::path p = fs::absolute(r).lexically_normal();
        if (p.filename().empty()) p = p.parent_path();  // "game/" -> "game"
        abs.push_back(p.string());
    }
    WorldBuilder b(abs.front());
    Loader(b, abs).run();
    return b.finish();
}

}  // namespace terni
