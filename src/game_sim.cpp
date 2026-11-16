// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
// Copyright (C) 2026 Rafał Zając
// GameSim: GLSL prelude generation, the SPIR-V cache (rules are compiled by
// rule_compiler_*.cpp), the modifier store, and running a world on a Vulkan device.
//
// Binding layout (also the order of the generated GLSL):
//   0      defines                       D.<name>
//   1      state                         terni.tick, terni.<entity>_count
//   2 ...  one buffer per field          <entity>_<field>[]       (entities and fields in schema order)
//   then   one buffer per modifier key and entity it can sit on:
//                                        <entity>_modown_<key>[]  (key order; applies_to entity, then inherited one)
//
// Specialization constants: 0, 1 are the runner's (compute.glsl); from 16 on, one capacity
// per entity type, <ENTITY>_CAPACITY, so the SPIR-V does not depend on capacities.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>

#include "terni/compute_runner.hpp"
#include "terni/fixed.hpp"
#include "terni/game.hpp"
#include "embedded_glsl.hpp"
#include "rule_compiler.hpp"

namespace terni {

namespace {

constexpr uint32_t CAPACITY_CONSTANT0 = 16;  // constant_id of the first entity type's capacity

const char* glsl_type(FieldType t) {
    switch (t) {
        case FieldType::Count:
        case FieldType::Ref: return "uint";
        case FieldType::Money64: return "uvec2";
        default: return "int";
    }
}

// ASCII only: std::toupper follows the process locale (under a Turkish one 'i' does not
// become 'I'), and the result is a GLSL identifier.
std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// ---- explicit little-endian binary writer / reader (saves, checksums) ----------
struct Writer {
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void words(const std::vector<int32_t>& w) {
        u32(static_cast<uint32_t>(w.size()));
        for (int32_t v : w) i32(v);
    }
};

struct Reader {
    const uint8_t* data;
    size_t size, pos = 0;
    void need(size_t n) const {
        if (pos + n > size) throw std::runtime_error("save: truncated data");
    }
    uint8_t u8() {
        need(1);
        return data[pos++];
    }
    uint32_t u32() {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(data[pos++]) << (8 * i);
        return v;
    }
    uint64_t u64() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(data[pos++]) << (8 * i);
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    std::string str() {
        const uint32_t n = u32();
        need(n);
        std::string s(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return s;
    }
    std::vector<int32_t> words() {
        const uint32_t n = u32();
        need(size_t(n) * 4);
        std::vector<int32_t> w(n);
        for (auto& v : w) v = i32();
        return w;
    }
};

uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

constexpr char SAVE_MAGIC[8] = {'T', 'E', 'R', 'N', 'I', 'S', 'A', 'V'};
constexpr uint32_t SAVE_VERSION = 1;

// Modifier total columns: for each key, the entity it applies to and (if it
// inherits) the entity it inherits from. Shared by the prelude and the store.
struct ModColumn {
    uint32_t key, entity;
};
std::vector<ModColumn> modifier_columns(const Game& g) {
    std::vector<ModColumn> cols;
    for (uint32_t k = 0; k < g.modifier_keys.size(); ++k) {
        const auto& key = g.modifier_keys[k];
        cols.push_back({k, key.entity});
        if (key.inherit_entity >= 0 && uint32_t(key.inherit_entity) != key.entity)
            cols.push_back({k, uint32_t(key.inherit_entity)});
    }
    return cols;
}

// ---------------------------------------------------------------------------
// Modifier store (host side). Instances live in slots with a version; totals per
// (key, entity, row) are updated incrementally — exact, because they're integer
// sums. Durations use a discrete-event queue ordered by (due tick, sequence) with
// validate-on-wake: removing or rescaling an instance never touches the queue;
// a due entry whose (slot, version) no longer matches is dropped when it pops.
// ---------------------------------------------------------------------------
class ModifierStore {
public:
    struct Column {
        uint32_t key, entity;
        std::vector<int32_t> totals;
        uint32_t dirty_lo = UINT32_MAX, dirty_hi = 0;  // [lo, hi) rows changed since the last upload
    };

    explicit ModifierStore(const Game& g) : g_(g) {
        col_of_.assign(g.modifier_keys.size(), std::vector<int>(g.entities.size(), -1));
        for (const auto& c : modifier_columns(g)) {
            col_of_[c.key][c.entity] = static_cast<int>(columns.size());
            columns.push_back({c.key, c.entity, std::vector<int32_t>(g.entities[c.entity].capacity, 0)});
        }
    }

    ModifierHandle add(uint32_t bundle, uint32_t entity, uint32_t row, int32_t scale, uint32_t duration, uint64_t now) {
        if (bundle >= g_.bundles.size() || entity >= g_.entities.size() || row >= g_.entities[entity].capacity)
            throw std::out_of_range("add_modifier: bad bundle, entity or row");
        uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();  // LIFO reuse: deterministic slot (and handle) assignment
            free_.pop_back();
        } else {
            if (inst_.size() >= g_.modifier_capacity) return {};
            slot = static_cast<uint32_t>(inst_.size());
            inst_.push_back({});
        }
        Instance& in = inst_[slot];
        in.bundle = bundle;
        in.entity = entity;
        in.row = row;
        in.scale = scale;
        in.alive = true;
        apply(in, +1);
        ++alive_;
        if (duration > 0) {
            due_.push_back({now + duration, seq_++, slot, in.version});
            std::push_heap(due_.begin(), due_.end(), std::greater<Due>());
        }
        return {slot, in.version};
    }

    bool alive(ModifierHandle h) const {
        return h.slot < inst_.size() && inst_[h.slot].alive && inst_[h.slot].version == h.version;
    }

    bool set_scale(ModifierHandle h, int32_t scale) {
        if (!alive(h)) return false;
        Instance& in = inst_[h.slot];
        apply(in, -1);
        in.scale = scale;
        apply(in, +1);
        return true;
    }

    bool remove(ModifierHandle h) {
        if (!alive(h)) return false;
        Instance& in = inst_[h.slot];
        apply(in, -1);
        in.alive = false;
        ++in.version;  // every outstanding handle and queue entry for this slot is now stale
        free_.push_back(h.slot);
        --alive_;
        return true;
    }

    // Tick boundary: expire everything due at or before `now`, in (due, sequence) order.
    void expire_due(uint64_t now) {
        while (!due_.empty() && due_.front().tick <= now) {
            std::pop_heap(due_.begin(), due_.end(), std::greater<Due>());
            const Due d = due_.back();
            due_.pop_back();
            remove({d.slot, d.version});  // validate on wake: stale entries are no-ops
        }
    }

    int32_t own_total(uint32_t key, uint32_t entity, uint32_t row) const {
        const int c = col_of_.at(key).at(entity);
        return c < 0 || row >= columns[size_t(c)].totals.size() ? 0 : columns[size_t(c)].totals[row];
    }

    void own_breakdown(uint32_t key, uint32_t entity, uint32_t row, std::vector<ModifierContribution>& out) const {
        for (uint32_t s = 0; s < inst_.size(); ++s) {
            const Instance& in = inst_[s];
            if (!in.alive || in.entity != entity || in.row != row) continue;
            for (const auto& [k, v] : g_.bundles[in.bundle].values)
                if (k == key) out.push_back({{s, in.version}, in.bundle, entity, row, fx::mul(v, in.scale)});
        }
    }

    size_t count() const { return alive_; }
    std::vector<Column> columns;

    void restore_from(ModifierStore&& o) {
        columns = std::move(o.columns);
        col_of_ = std::move(o.col_of_);
        inst_ = std::move(o.inst_);
        free_ = std::move(o.free_);
        due_ = std::move(o.due_);
        seq_ = o.seq_;
        alive_ = o.alive_;
    }

    // ---- save / load: instances, free-slot order, pending expiries, sequence.
    // Totals are not stored; restore() recomputes them exactly from the instances.
    template <class W>
    void save(W& w) const {
        w.u64(seq_);
        w.u32(static_cast<uint32_t>(inst_.size()));
        for (const Instance& in : inst_) {
            w.u32(in.bundle), w.u32(in.entity), w.u32(in.row), w.i32(in.scale), w.u32(in.version);
            w.u8(in.alive ? 1 : 0);
        }
        w.u32(static_cast<uint32_t>(free_.size()));
        for (uint32_t s : free_) w.u32(s);
        std::vector<Due> pending = due_;  // write in dispatch order so equal saves are byte-identical
        std::sort(pending.begin(), pending.end(), [](const Due& a, const Due& b) { return b > a; });
        w.u32(static_cast<uint32_t>(pending.size()));
        for (const Due& d : pending) w.u64(d.tick), w.u64(d.seq), w.u32(d.slot), w.u32(d.version);
    }

    template <class R>
    void restore(R& r) {
        seq_ = r.u64();
        const uint32_t n = r.u32();
        if (n > g_.modifier_capacity) throw std::runtime_error("save: more modifier instances than capacity");
        inst_.assign(n, {});
        alive_ = 0;
        for (Instance& in : inst_) {
            in.bundle = r.u32(), in.entity = r.u32(), in.row = r.u32(), in.scale = r.i32(), in.version = r.u32();
            in.alive = r.u8() != 0;
            if (in.alive && (in.bundle >= g_.bundles.size() || in.entity >= g_.entities.size() ||
                             in.row >= g_.entities[in.entity].capacity))
                throw std::runtime_error("save: modifier instance out of range");
            alive_ += in.alive;
        }
        free_.resize(r.u32());
        for (uint32_t& s : free_) {
            s = r.u32();
            if (s >= inst_.size() || inst_[s].alive) throw std::runtime_error("save: bad modifier free list");
        }
        due_.resize(r.u32());
        for (Due& d : due_) d.tick = r.u64(), d.seq = r.u64(), d.slot = r.u32(), d.version = r.u32();
        std::make_heap(due_.begin(), due_.end(), std::greater<Due>());
        for (Column& c : columns) {
            std::fill(c.totals.begin(), c.totals.end(), 0);
            c.dirty_lo = 0;
            c.dirty_hi = static_cast<uint32_t>(c.totals.size());  // upload everything after a load
        }
        for (const Instance& in : inst_)
            if (in.alive) apply(in, +1);
    }

private:
    struct Instance {
        uint32_t bundle = 0, entity = 0, row = 0;
        int32_t scale = 0;
        uint32_t version = 0;
        bool alive = false;
    };
    struct Due {
        uint64_t tick, seq;
        uint32_t slot, version;
        bool operator>(const Due& o) const { return tick != o.tick ? tick > o.tick : seq > o.seq; }
    };

    // Add (sign = +1) or subtract (sign = -1) an instance's contributions. The
    // contribution is recomputed identically each time, so subtraction is exact.
    void apply(const Instance& in, int sign) {
        for (const auto& [k, v] : g_.bundles[in.bundle].values) {
            const int c = col_of_[k][in.entity];
            if (c < 0) continue;  // key doesn't apply to this entity type: ignored, like Paradox
            Column& col = columns[size_t(c)];
            const int32_t contribution = fx::mul(v, in.scale);
            col.totals[in.row] = static_cast<int32_t>(static_cast<uint32_t>(col.totals[in.row]) +
                                                      static_cast<uint32_t>(sign * contribution));
            col.dirty_lo = std::min(col.dirty_lo, in.row);
            col.dirty_hi = std::max(col.dirty_hi, in.row + 1);
        }
    }

    const Game& g_;
    std::vector<std::vector<int>> col_of_;  // [key][entity] -> column or -1
    std::vector<Instance> inst_;
    std::vector<uint32_t> free_;
    std::vector<Due> due_;  // min-heap on (tick, seq) via std::push_heap/pop_heap
    uint64_t seq_ = 0;
    size_t alive_ = 0;
};

// The GLSL library a rule can #include: the copy embedded in Terni, or a folder
// (development override). Its fingerprint is part of every SPIR-V cache key.
class ShaderLibrary {
public:
    explicit ShaderLibrary(std::string dir) : dir_(std::move(dir)) {
        Writer w;
        if (dir_.empty()) {
            for (size_t i = 0; i < detail::kEmbeddedGlslCount; ++i)
                w.str(detail::kEmbeddedGlsl[i].path), w.str(detail::kEmbeddedGlsl[i].content);
        } else {
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(dir_) / "terni"))
                if (e.is_regular_file()) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto& f : files) {
                std::string content;
                if (read(("terni/" + f.filename().string()).c_str(), content)) w.str(f.filename().string()), w.str(content);
            }
        }
        fingerprint_ = fnv64(w.buf.data(), w.buf.size());
    }

    bool read(const char* requested, std::string& out) const {
        if (dir_.empty()) {
            for (size_t i = 0; i < detail::kEmbeddedGlslCount; ++i)
                if (std::strcmp(detail::kEmbeddedGlsl[i].path, requested) == 0) {
                    out = detail::kEmbeddedGlsl[i].content;
                    return true;
                }
            return false;
        }
        std::ifstream f(dir_ + "/" + requested, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        out.clear();
        for (size_t i = 0; i < text.size(); ++i)  // CRLF -> LF, as the embedded copy and stage sources
            if (text[i] != '\r' || i + 1 == text.size() || text[i + 1] != '\n') out += text[i];
        return true;
    }

    std::string describe(const char* requested) const {
        return dir_.empty() ? std::string("<terni>/") + requested : dir_ + "/" + requested;
    }
    uint64_t fingerprint() const { return fingerprint_; }

private:
    std::string dir_;
    uint64_t fingerprint_ = 0;
};

// A complete compute shader: the generated prelude followed by one rule's body.
struct RuleSource {
    std::string name;    // stage name; the built-in last stage is "terni_tick"
    std::string source;  // full GLSL
};

// Every shader a world runs, in tick order: its rules, then the built-in stage that
// advances the tick counter (seeds the rng rolls). GameSim and compile_rules() both
// use this, so a cache filled by one is found by the other.
std::vector<RuleSource> rule_sources(const Game& game) {
    const std::string prelude = generate_prelude(game);
    auto full = [&](const std::string& file, const std::string& body) {
        return "#version 460\n"
               "#extension GL_GOOGLE_include_directive : require\n"
               "#extension GL_GOOGLE_cpp_style_line_directive : require\n" +
               prelude + "\n#line 1 \"" + file + "\"\n" + body;
    };
    std::vector<RuleSource> out;
    for (const auto& s : game.stages) out.push_back({s.name, full("rules/" + s.name + ".glsl", s.source)});
    out.push_back({"terni_tick", full("<terni>", "void main() { if (gl_GlobalInvocationID.x == 0u) terni.tick += 1u; }")});
    return out;
}

// ---- SPIR-V cache ---------------------------------------------------------------
// One file per shader, named by a hash of everything that decides its SPIR-V: the full
// source, the GLSL library's contents and the compile settings. The compiler itself is
// not part of the key, so looking a shader up needs no compiler, and a cache filled by
// one build or machine is found by any other.
//
// file: "TSPV" | key u64 | word count u32 | content hash u64 | SPIR-V words. A hit needs
// the key, the size and the content hash to match: a damaged file is recompiled (or
// reported missing), never handed to the driver.
constexpr uint32_t CACHE_FORMAT = 2;  // 1 had the compiler's SPIR-V version in the key
constexpr size_t CACHE_HEADER = 4 + 8 + 4 + 8;

uint64_t cache_key(const std::string& source, const ShaderLibrary& library) {
    Writer w;
    w.u32(CACHE_FORMAT);
    w.str(detail::kCompileSettings);
    w.u64(library.fingerprint());
    w.str(source);
    return fnv64(w.buf.data(), w.buf.size());
}

std::filesystem::path cache_file(const std::string& dir, uint64_t key) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
    return std::filesystem::path(dir) / (std::string(hex) + ".spv");
}

bool cache_load(const std::string& dir, uint64_t key, std::vector<uint32_t>& spirv) {
    std::ifstream f(cache_file(dir, key), std::ios::binary);
    if (!f) return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < CACHE_HEADER || std::memcmp(bytes.data(), "TSPV", 4) != 0) return false;
    Reader r{bytes.data(), bytes.size(), 4};
    const uint64_t stored_key = r.u64();
    const uint32_t n = r.u32();
    const uint64_t content_hash = r.u64();
    if (stored_key != key || n == 0 || bytes.size() != CACHE_HEADER + size_t(n) * 4 ||
        fnv64(bytes.data() + CACHE_HEADER, size_t(n) * 4) != content_hash)
        return false;
    spirv.resize(n);
    for (auto& w : spirv) w = r.u32();
    return spirv[0] == 0x07230203u;  // SPIR-V magic
}

// Written to a temporary file, then renamed: readers never see a partial file. A failed
// write (a read-only folder, say) only costs a compile at the next start.
void cache_store(const std::string& dir, uint64_t key, const std::vector<uint32_t>& spirv) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    Writer body;
    for (uint32_t v : spirv) body.u32(v);
    Writer w;
    w.buf = {'T', 'S', 'P', 'V'};
    w.u64(key), w.u32(static_cast<uint32_t>(spirv.size())), w.u64(fnv64(body.buf.data(), body.buf.size()));
    w.buf.insert(w.buf.end(), body.buf.begin(), body.buf.end());
    const std::filesystem::path file = cache_file(dir, key);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(w.buf.data()), std::streamsize(w.buf.size()));
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

// SPIR-V for every shader: from the cache, else compiled (and stored in the cache) if
// `may_compile`. Shaders that are neither cached nor compilable are reported together.
std::vector<std::vector<uint32_t>> rules_spirv(const std::vector<RuleSource>& rules, const ShaderLibrary& library,
                                                const std::string& cache_dir, bool may_compile, CompileStats& stats) {
    const detail::IncludeResolver include = [&library](const char* requested, std::string& content, std::string& shown) {
        if (!library.read(requested, content)) return false;
        shown = library.describe(requested);
        return true;
    };
    std::vector<std::vector<uint32_t>> out(rules.size());
    std::string missing;
    size_t missing_count = 0;
    for (size_t i = 0; i < rules.size(); ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t key = cache_key(rules[i].source, library);
        if (!cache_dir.empty() && cache_load(cache_dir, key, out[i])) {
            ++stats.cached;
        } else if (may_compile) {
            out[i] = detail::compile_glsl(rules[i].name, rules[i].source, include);
            if (!cache_dir.empty()) cache_store(cache_dir, key, out[i]);
            ++stats.compiled;
        } else {
            missing += (missing.empty() ? "" : ", ") + rules[i].name;
            ++missing_count;
        }
        stats.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    if (missing_count > 0) {
        const std::string why = detail::have_rule_compiler() ? "cache-only start"
                                                             : "this build of Terni has no rule compiler (Terni::runtime)";
        const std::string what =
            cache_dir.empty() ? "no spirv_cache_dir to load the " + std::to_string(rules.size()) + " shaders from"
                              : std::to_string(missing_count) + " of " + std::to_string(rules.size()) +
                                    " shaders are not in the SPIR-V cache '" + cache_dir + "' (" + missing + ")";
        throw std::runtime_error(why + ": " + what +
                                 ". Fill the cache with terni::compile_rules() for exactly this game (same schema and "
                                 "rules) in a build that has the rule compiler.");
    }
    return out;
}

}  // namespace

std::string generate_prelude(const Game& game) {
    std::ostringstream o;
    o.imbue(std::locale::classic());  // never "16,384u": the host may have set a global locale with digit grouping
    o << "// ---- generated by Terni from the game schema -- do not edit ----\n"
         "#include \"terni/compute.glsl\"\n"
         "#include \"terni/fixed.glsl\"\n"
         "#include \"terni/rng.glsl\"\n"
         "#include \"terni/wide.glsl\"\n\n";
    o << "layout(std430, binding = 0) readonly buffer TerniDefines {\n";
    for (const auto& d : game.defines) o << "    int " << d.name << ";\n";
    if (game.defines.empty()) o << "    int _terni_no_defines;\n";
    o << "} D;\n\n";
    o << "layout(std430, binding = 1) buffer TerniState {\n    uint tick;\n";
    for (const auto& e : game.entities) o << "    uint " << e.name << "_count;\n";
    o << "} terni;\n";
    uint32_t binding = 2;
    for (size_t i = 0; i < game.entities.size(); ++i) {
        const auto& e = game.entities[i];
        o << "\n// " << e.name << "\n";
        // set when the pipelines are made, so every capacity runs the same SPIR-V
        o << "layout(constant_id = " << CAPACITY_CONSTANT0 + i << ") const uint " << upper(e.name)
          << "_CAPACITY = 1u;\n";
        o << "#define FOR_EACH_" << upper(e.name) << "(i) FOR_EACH(i, terni." << e.name << "_count)\n";
        o << "#define FOR_EACH_" << upper(e.name) << "_GROUP(i) FOR_EACH_GROUP(i, terni." << e.name << "_count)\n";
        for (const auto& f : e.fields) {
            if (f.size > 1) o << "const uint " << upper(e.name + "_" + f.name) << "_SIZE = " << f.size << "u;\n";
            o << "layout(std430, binding = " << binding++ << ") buffer " << e.name << "_" << f.name << "_buf { "
              << glsl_type(f.type) << " " << e.name << "_" << f.name << "[]; };\n";
        }
    }
    const auto cols = modifier_columns(game);
    if (!cols.empty()) {
        o << "\n// modifier totals (own instances only; use the accessors below)\n";
        for (const auto& c : cols) {
            const std::string n = game.entities[c.entity].name + "_modown_" + game.modifier_keys[c.key].name;
            o << "layout(std430, binding = " << binding++ << ") buffer " << n << "_buf { int " << n << "[]; };\n";
        }
        o << "\n// modifier accessors: own total + inherited total\n";
        for (const auto& c : cols) {
            const auto& key = game.modifier_keys[c.key];
            const std::string& en = game.entities[c.entity].name;
            o << "int " << en << "_mod_" << key.name << "(uint i) { return " << en << "_modown_" << key.name << "[i]";
            if (c.entity == key.entity && key.inherit_entity >= 0 && uint32_t(key.inherit_entity) != key.entity) {
                const std::string& pn = game.entities[size_t(key.inherit_entity)].name;
                const std::string ref = en + "_" + key.inherit_field + "[i]";
                o << " + (" << ref << " < " << upper(pn) << "_CAPACITY ? " << pn << "_modown_" << key.name << "[" << ref
                  << "] : 0)";
            }
            o << "; }\n";
        }
    }
    return o.str();
}

bool can_compile_rules() { return detail::have_rule_compiler(); }

CompileStats compile_rules(const Game& game, const std::string& cache_dir, const std::string& shader_library_dir) {
    if (!game.ok()) throw std::runtime_error("game has load errors");
    if (cache_dir.empty()) throw std::invalid_argument("compile_rules: no cache folder given");
    if (!detail::have_rule_compiler())
        throw std::runtime_error("compile_rules: this build of Terni has no rule compiler (Terni::runtime)");
    const ShaderLibrary library(shader_library_dir);
    const auto rules = rule_sources(game);
    CompileStats stats;
    rules_spirv(rules, library, cache_dir, true, stats);
    // the cache is the product here: make sure every shader can be loaded back from it
    std::vector<uint32_t> spirv;
    for (const auto& r : rules)
        if (!cache_load(cache_dir, cache_key(r.source, library), spirv))
            throw std::runtime_error("compile_rules: could not write the SPIR-V cache in '" + cache_dir + "'");
    return stats;
}

struct GameSim::Impl {
    const Game& game;
    std::vector<int32_t> defines;
    std::vector<uint32_t> state;                     // tick, counts
    std::vector<std::vector<int32_t>> columns;       // per field, in binding order (initial upload / readback)
    std::vector<std::string> column_names;
    std::vector<std::vector<uint32_t>> field_binding;  // [entity][field] -> binding
    uint32_t mod_binding0 = 0;
    ModifierStore mods;
    std::vector<uint32_t> counts;  // rows in use per entity: the starting counts, or those restored by load()
    struct TickRead {
        uint32_t readback;  // ComputeRunner id
        uint32_t entity, first_row;
        uint32_t rows;       // UINT32_MAX: every row in use, following the entity's count
        uint64_t row_words;  // field size x words per value
    };
    std::vector<TickRead> tick_reads;
    // Host copy of a ref field that modifier keys inherit through, so modifier_total()
    // and modifier_breakdown() never wait for the GPU. It starts with the first query that
    // needs it (one read); from then on the rows in use come back with every tick, in the
    // same submission, and host writes and load() update it directly.
    struct Parents {
        uint32_t entity, field, binding;
        bool active = false;
        bool stale = false;          // a tick ran since `rows` was refreshed
        TickRead read{};             // the per-tick readback, once active
        std::vector<uint32_t> rows;  // capacity rows
    };
    std::vector<Parents> parents;
    std::vector<int> key_parents;  // per modifier key: index into `parents`, or -1
    std::vector<uint64_t> row_words;  // per binding: words per row (order check reports)
    uint64_t now = 0;
    CompileStats stats;
    std::unique_ptr<ComputeRunner> runner;
    explicit Impl(const Game& g) : game(g), mods(g) {}

    // The row a key's entity inherits from, without touching the GPU after the first call.
    uint32_t parent_row(uint32_t key, uint32_t row) {
        Parents& p = parents[size_t(key_parents.at(key))];
        if (!p.active) {
            runner->flush_queued();  // queued host writes belong to the current state
            p.rows.resize(game.entities[p.entity].capacity);
            runner->read(p.binding, p.rows.data(), p.rows.size() * 4);
            p.read = {runner->add_tick_readback(p.binding, p.rows.size() * 4, 0), p.entity, 0, UINT32_MAX, 1};
            runner->set_tick_readback_bytes(p.read.readback, tick_read_bytes(p.read));
            p.active = true;
        } else if (p.stale) {
            refresh(p);
        }
        return row < p.rows.size() ? p.rows[row] : UINT32_MAX;
    }
    void refresh(Parents& p) {
        std::memcpy(p.rows.data(), runner->tick_readback(p.read.readback), runner->tick_readback_bytes(p.read.readback));
        p.stale = false;
    }
    // Which stages run in the coming tick (the built-in last one always does).
    std::vector<bool> stages_now() const {
        std::vector<bool> run(game.stages.size() + 1, true);
        for (size_t s = 0; s < game.stages.size(); ++s) run[s] = game.stages[s].runs_in(now);
        return run;
    }
    void after_tick() {
        ++now;
        for (auto& p : parents) p.stale = p.active;
    }

    // Bytes a per-tick readback copies with the current counts.
    uint64_t tick_read_bytes(const TickRead& r) const {
        const uint32_t capacity = game.entities[r.entity].capacity;
        const uint32_t end = r.rows == UINT32_MAX ? std::min(counts[r.entity], capacity) : capacity;
        uint64_t rows = end > r.first_row ? end - r.first_row : 0;
        if (r.rows != UINT32_MAX) rows = std::min<uint64_t>(rows, r.rows);
        return rows * r.row_words * 4;
    }
    void size_tick_reads() {
        for (const auto& r : tick_reads) runner->set_tick_readback_bytes(r.readback, tick_read_bytes(r));
        for (const auto& p : parents)
            if (p.active) runner->set_tick_readback_bytes(p.read.readback, tick_read_bytes(p.read));
    }

    // Tick boundary: expire modifiers, push host changes to the GPU.
    void boundary() {
        mods.expire_due(now);
        for (size_t c = 0; c < mods.columns.size(); ++c) {
            auto& col = mods.columns[c];
            if (col.dirty_lo >= col.dirty_hi) continue;
            runner->queue_write(mod_binding0 + uint32_t(c), col.totals.data() + col.dirty_lo,
                          uint64_t(col.dirty_hi - col.dirty_lo) * 4, uint64_t(col.dirty_lo) * 4);
            col.dirty_lo = UINT32_MAX;
            col.dirty_hi = 0;
        }
    }
};

GameSim::GameSim(const Game& game, uint32_t device, const std::string& shader_library_dir, bool validation)
    : GameSim(game, [&] {
          GameSimOptions o;
          o.device = static_cast<int>(device);
          o.shader_library_dir = shader_library_dir;
          o.validation = validation;
          return o;
      }()) {}

GameSim::GameSim(const Game& game, const GameSimOptions& options) : impl_(std::make_unique<Impl>(game)) {
    Impl& m = *impl_;
    if (!game.ok()) throw std::runtime_error("game has load errors");
    // SPIR-V first: a cache-only start that lacks a rule fails before any GPU work
    const bool may_compile = !options.cache_only && detail::have_rule_compiler();
    auto spirv = rules_spirv(rule_sources(game), ShaderLibrary(options.shader_library_dir), options.spirv_cache_dir,
                             may_compile, m.stats);
    int device = options.device;
    if (device < 0) {
        const std::array<uint8_t, 16>* uuid = options.device_uuid ? &*options.device_uuid : nullptr;
        device = pick_device(options.preference, uuid);
        if (device < 0) throw std::runtime_error("no usable Vulkan device (Vulkan 1.3 with synchronization2 required)");
    }
    m.defines = game.define_values;
    if (m.defines.empty()) m.defines.push_back(0);
    m.state.push_back(0);  // tick
    for (const auto& t : game.tables) m.state.push_back(t.count), m.counts.push_back(t.count);
    for (const auto& im : game.initial_modifiers) m.mods.add(im.bundle, im.entity, im.row, im.scale, im.duration, 0);

    std::vector<ComputeBufferSpec> buffers;
    buffers.push_back({"defines", m.defines.data(), m.defines.size() * 4});
    buffers.push_back({"state", m.state.data(), m.state.size() * 4});
    m.field_binding.resize(game.entities.size());
    for (size_t e = 0; e < game.tables.size(); ++e) {
        const auto& t = game.tables[e];
        for (size_t f = 0; f < t.def->fields.size(); ++f) {
            m.field_binding[e].push_back(static_cast<uint32_t>(2 + m.columns.size()));
            m.columns.push_back(t.columns[f]);
            m.column_names.push_back(t.def->name + "." + t.def->fields[f].name);
        }
    }
    for (size_t c = 0; c < m.columns.size(); ++c)
        buffers.push_back({m.column_names[c], m.columns[c].data(), m.columns[c].size() * 4});
    m.mod_binding0 = static_cast<uint32_t>(buffers.size());
    for (auto& c : m.mods.columns) {
        buffers.push_back({"mod", c.totals.data(), c.totals.size() * 4});
        c.dirty_lo = UINT32_MAX;  // initial modifiers are part of the upload
        c.dirty_hi = 0;
    }
    m.row_words.assign(2, 1);  // defines, state
    for (const auto& e : game.entities)
        for (const auto& f : e.fields) m.row_words.push_back(uint64_t(f.size) * f.words());
    m.row_words.resize(buffers.size(), 1);  // modifier totals
    for (const auto& k : game.modifier_keys) {
        int index = -1;
        if (k.inherit_entity >= 0) {
            const uint32_t f = static_cast<uint32_t>(game.entities[k.entity].field_index(k.inherit_field));
            for (size_t i = 0; i < m.parents.size(); ++i)
                if (m.parents[i].entity == k.entity && m.parents[i].field == f) index = int(i);
            if (index < 0) {
                m.parents.push_back({k.entity, f, m.field_binding[k.entity][f]});
                index = int(m.parents.size() - 1);
            }
        }
        m.key_parents.push_back(index);
    }

    std::vector<ComputeStageSpec> stages;
    for (size_t i = 0; i < game.stages.size(); ++i) {
        const auto& s = game.stages[i];
        const uint32_t capacity = game.entity(s.for_each)->capacity;
        // workgroup dispatch: one 256-thread group per entity (FOR_EACH_GROUP strides past 65535)
        const uint32_t elements = s.dispatch == Dispatch::Workgroup ? std::min(capacity, 65535u) * 256u : capacity;
        std::vector<uint32_t> zero;  // cleared only in the ticks the stage runs, so a total lasts until its next run
        for (const auto& c : s.clear)
            for (size_t k = 0; k < m.column_names.size(); ++k)
                if (m.column_names[k] == c) zero.push_back(static_cast<uint32_t>(k + 2));
        stages.push_back({s.name, elements, std::move(spirv[i]), std::move(zero)});
    }
    stages.push_back({"terni_tick", 1, std::move(spirv.back()), {}});  // built-in last stage (rule_sources)

    std::vector<ComputeSpecConstant> capacities;
    for (size_t e = 0; e < game.entities.size(); ++e)
        capacities.push_back({CAPACITY_CONSTANT0 + uint32_t(e), game.entities[e].capacity});
    m.runner = std::make_unique<ComputeRunner>(static_cast<uint32_t>(device), options.validation, "", buffers, stages,
                                               std::vector<uint32_t>{}, true, capacities);
}

GameSim::~GameSim() = default;

void GameSim::tick() {
    impl_->boundary();
    impl_->runner->tick(impl_->stages_now());
    impl_->after_tick();
}

std::vector<OrderIssue> GameSim::tick_checked() {
    Impl& m = *impl_;
    m.boundary();
    std::vector<bool> check(m.game.stages.size() + 1, true);
    check.back() = false;  // the built-in tick counter stage
    const auto diffs = m.runner->tick_checked(check, m.row_words, m.stages_now());
    m.after_tick();
    std::vector<OrderIssue> out;
    for (const auto& d : diffs) {
        OrderIssue issue;
        issue.stage = m.game.stages[d.stage].name;
        if (d.binding == 0) issue.field = "defines";
        else if (d.binding == 1) issue.field = "terni.state";
        else if (d.binding < m.mod_binding0) issue.field = m.column_names[d.binding - 2];
        else {
            const auto& col = m.mods.columns[d.binding - m.mod_binding0];
            issue.field = "mod." + m.game.entities[col.entity].name + "." + m.game.modifier_keys[col.key].name;
        }
        issue.parallel = d.parallel;
        issue.first_row = d.first_row;
        issue.rows = d.rows;
        out.push_back(std::move(issue));
    }
    return out;
}

uint64_t GameSim::sync_reads() const { return impl_->runner->sync_reads(); }

uint64_t GameSim::ticks() const { return impl_->now; }

std::vector<int32_t> GameSim::read_field(uint32_t entity, const std::string& field, uint32_t first_row, uint32_t rows) {
    Impl& m = *impl_;
    const EntityDef& e = m.game.entities.at(entity);
    const int f = e.field_index(field);
    if (f < 0) throw std::invalid_argument("read_field: '" + e.name + "' has no field '" + field + "'");
    const FieldDef& fd = e.fields[size_t(f)];
    rows = std::min(rows, e.capacity - std::min(first_row, e.capacity));
    const uint64_t per_row = uint64_t(fd.size) * fd.words();
    std::vector<int32_t> out(rows * per_row);
    if (!out.empty())
        m.runner->read(m.field_binding[entity][size_t(f)], out.data(), out.size() * 4, uint64_t(first_row) * per_row * 4);
    return out;
}

void GameSim::write_field(uint32_t entity, const std::string& field, uint32_t row, const std::vector<int32_t>& words) {
    write_field(entity, field, row, std::span<const int32_t>(words));
}

void GameSim::write_field(uint32_t entity, const std::string& field, uint32_t row, std::span<const int32_t> words) {
    Impl& m = *impl_;
    const EntityDef& e = m.game.entities.at(entity);
    const int f = e.field_index(field);
    if (f < 0) throw std::invalid_argument("write_field: '" + e.name + "' has no field '" + field + "'");
    const FieldDef& fd = e.fields[size_t(f)];
    const uint64_t per_row = uint64_t(fd.size) * fd.words();
    if (row >= e.capacity || words.size() % per_row != 0 || row + words.size() / per_row > e.capacity)
        throw std::out_of_range("write_field: rows out of range");
    m.runner->queue_write(m.field_binding[entity][size_t(f)], words.data(), words.size() * 4, uint64_t(row) * per_row * 4);
    for (auto& p : m.parents)
        if (p.active && p.entity == entity && p.field == uint32_t(f)) {
            if (p.stale) m.refresh(p);  // the last tick's values first, then this write on top
            std::memcpy(p.rows.data() + row, words.data(), words.size() * 4);
        }
}

void GameSim::write_field(uint32_t entity, const std::string& field, uint32_t row, uint32_t index,
                          const std::vector<int32_t>& words) {
    write_field(entity, field, row, index, std::span<const int32_t>(words));
}

void GameSim::write_field(uint32_t entity, const std::string& field, uint32_t row, uint32_t index,
                          std::span<const int32_t> words) {
    Impl& m = *impl_;
    const EntityDef& e = m.game.entities.at(entity);
    const int f = e.field_index(field);
    if (f < 0) throw std::invalid_argument("write_field: '" + e.name + "' has no field '" + field + "'");
    const FieldDef& fd = e.fields[size_t(f)];
    const uint64_t per_value = fd.words();
    if (words.size() % per_value != 0)
        throw std::invalid_argument("write_field: '" + e.name + "." + field + "' takes " + std::to_string(per_value) +
                                    " words per value");
    if (row >= e.capacity || index > fd.size || words.size() / per_value > fd.size - index)
        throw std::out_of_range("write_field: row or values out of range");
    const uint64_t at = (uint64_t(row) * fd.size + index) * per_value;  // in words
    m.runner->queue_write(m.field_binding[entity][size_t(f)], words.data(), words.size() * 4, at * 4);
    for (auto& p : m.parents)  // inherit fields hold one value per row, so a partial write is the whole row
        if (p.active && p.entity == entity && p.field == uint32_t(f) && !words.empty()) {
            if (p.stale) m.refresh(p);
            std::memcpy(p.rows.data() + row, words.data(), 4);
        }
}

uint32_t GameSim::read_each_tick(uint32_t entity, const std::string& field, uint32_t first_row, uint32_t rows) {
    Impl& m = *impl_;
    const EntityDef& e = m.game.entities.at(entity);
    const int f = e.field_index(field);
    if (f < 0) throw std::invalid_argument("read_each_tick: '" + e.name + "' has no field '" + field + "'");
    if (first_row > e.capacity) throw std::out_of_range("read_each_tick: first row beyond the capacity");
    const FieldDef& fd = e.fields[size_t(f)];
    Impl::TickRead r{0, entity, first_row, rows, uint64_t(fd.size) * fd.words()};
    // room for the most it can ever copy: a range that follows the count may grow up to
    // the capacity (load() restores other counts), but only the rows in use are copied
    const uint64_t most = rows == UINT32_MAX ? e.capacity - first_row : std::min(rows, e.capacity - first_row);
    r.readback = m.runner->add_tick_readback(m.field_binding[entity][size_t(f)], most * r.row_words * 4,
                                             uint64_t(first_row) * r.row_words * 4);
    m.runner->set_tick_readback_bytes(r.readback, m.tick_read_bytes(r));
    m.tick_reads.push_back(r);
    return static_cast<uint32_t>(m.tick_reads.size() - 1);
}

std::span<const int32_t> GameSim::tick_result(uint32_t id) const {
    const Impl::TickRead& r = impl_->tick_reads.at(id);
    return {static_cast<const int32_t*>(impl_->runner->tick_readback(r.readback)),
            size_t(impl_->runner->tick_readback_bytes(r.readback) / 4)};
}

double GameSim::last_tick_ms() const { return impl_->runner->last_tick_ms(); }
double GameSim::last_tick_gpu_ms() const { return impl_->runner->last_tick_gpu_ms(); }

ModifierHandle GameSim::add_modifier(uint32_t bundle, uint32_t entity, uint32_t row, int32_t scale, uint32_t duration) {
    return impl_->mods.add(bundle, entity, row, scale, duration, impl_->now);
}
bool GameSim::set_modifier_scale(ModifierHandle h, int32_t scale) { return impl_->mods.set_scale(h, scale); }
bool GameSim::remove_modifier(ModifierHandle h) { return impl_->mods.remove(h); }
bool GameSim::modifier_alive(ModifierHandle h) const { return impl_->mods.alive(h); }
size_t GameSim::modifier_count() const { return impl_->mods.count(); }

int32_t GameSim::modifier_own_total(uint32_t key, uint32_t entity, uint32_t row) const {
    return impl_->mods.own_total(key, entity, row);
}

int32_t GameSim::modifier_total(uint32_t key, uint32_t entity, uint32_t row) {
    int32_t total = modifier_own_total(key, entity, row);
    const auto& k = impl_->game.modifier_keys.at(key);
    if (entity == k.entity && k.inherit_entity >= 0)
        total += modifier_own_total(key, uint32_t(k.inherit_entity), impl_->parent_row(key, row));
    return total;
}

std::vector<ModifierContribution> GameSim::modifier_breakdown(uint32_t key, uint32_t entity, uint32_t row) {
    std::vector<ModifierContribution> out;
    impl_->mods.own_breakdown(key, entity, row, out);
    const auto& k = impl_->game.modifier_keys.at(key);
    if (entity == k.entity && k.inherit_entity >= 0)
        impl_->mods.own_breakdown(key, uint32_t(k.inherit_entity), impl_->parent_row(key, row), out);
    return out;
}

uint64_t game_checksum(const Game& game) {
    Writer w;  // canonical serialization of everything that defines behaviour
    w.u32(static_cast<uint32_t>(game.entities.size()));
    for (const auto& e : game.entities) {
        w.str(e.name), w.u32(e.capacity), w.u32(static_cast<uint32_t>(e.fields.size()));
        for (const auto& f : e.fields) {
            w.str(f.name), w.u8(static_cast<uint8_t>(f.type)), w.str(f.ref_entity), w.u32(f.size);
            w.u64(static_cast<uint64_t>(f.default_raw));
        }
    }
    w.u32(static_cast<uint32_t>(game.defines.size()));
    for (size_t i = 0; i < game.defines.size(); ++i)
        w.str(game.defines[i].name), w.u8(static_cast<uint8_t>(game.defines[i].type)), w.i32(game.define_values[i]);
    w.u32(static_cast<uint32_t>(game.modifier_keys.size()));
    for (const auto& k : game.modifier_keys)
        w.str(k.name), w.u32(k.entity), w.u8(static_cast<uint8_t>(k.type)), w.str(k.inherit_field);
    w.u32(static_cast<uint32_t>(game.bundles.size()));
    for (const auto& b : game.bundles) {
        w.str(b.name), w.u32(static_cast<uint32_t>(b.values.size()));
        for (const auto& [k, v] : b.values) w.u32(k), w.i32(v);
    }
    w.u32(game.modifier_capacity);
    w.u32(static_cast<uint32_t>(game.stages.size()));
    for (const auto& s : game.stages) {
        w.str(s.name), w.str(s.for_each), w.str(s.source), w.u32(static_cast<uint32_t>(s.clear.size()));
        for (const auto& c : s.clear) w.str(c);
        if (s.dispatch == Dispatch::Workgroup) w.str("dispatch=workgroup");  // absent for the default
        if (s.every != 1) w.str("schedule"), w.u32(s.every), w.u32(s.offset);  // absent for every tick
    }
    return fnv64(w.buf.data(), w.buf.size());
}

uint64_t GameSim::checksum() const { return game_checksum(impl_->game); }

std::vector<uint8_t> GameSim::save() {
    Impl& m = *impl_;
    // host writes queued for the next tick are part of the state being saved
    m.runner->flush_queued();

    Writer w;
    w.buf.insert(w.buf.end(), SAVE_MAGIC, SAVE_MAGIC + 8);
    w.u32(SAVE_VERSION);
    w.u64(game_checksum(m.game));
    w.u64(m.now);
    m.runner->read(1, m.state.data(), m.state.size() * 4);
    w.u32(static_cast<uint32_t>(m.state.size()));
    for (uint32_t v : m.state) w.u32(v);
    w.u32(static_cast<uint32_t>(m.columns.size()));
    for (size_t c = 0; c < m.columns.size(); ++c) {
        m.runner->read(static_cast<uint32_t>(c + 2), m.columns[c].data(), m.columns[c].size() * 4);
        w.str(m.column_names[c]);
        w.words(m.columns[c]);
    }
    m.mods.save(w);
    w.u64(fnv64(w.buf.data(), w.buf.size()));  // content hash: detects corruption
    return std::move(w.buf);
}

void GameSim::load(const std::vector<uint8_t>& data) {
    Impl& m = *impl_;
    if (data.size() < 8 + 4 + 8 || std::memcmp(data.data(), SAVE_MAGIC, 8) != 0)
        throw std::runtime_error("save: not a Terni save");
    const uint64_t stored_hash = Reader{data.data() + data.size() - 8, 8}.u64();
    if (fnv64(data.data(), data.size() - 8) != stored_hash) throw std::runtime_error("save: corrupt (content hash mismatch)");
    Reader r{data.data(), data.size() - 8, 8};
    if (const uint32_t v = r.u32(); v != SAVE_VERSION)
        throw std::runtime_error("save: format version " + std::to_string(v) + ", expected " + std::to_string(SAVE_VERSION));
    if (r.u64() != game_checksum(m.game))
        throw std::runtime_error("save: made with a different game (definitions, defines, rules or mods differ)");

    // parse everything into temporaries first: a bad save leaves the simulation untouched
    const uint64_t now = r.u64();
    std::vector<uint32_t> state(r.u32());
    if (state.size() != m.state.size()) throw std::runtime_error("save: state size mismatch");
    for (auto& v : state) v = r.u32();
    for (size_t e = 0; e < m.game.entities.size(); ++e)
        if (state[1 + e] > m.game.entities[e].capacity)
            throw std::runtime_error("save: more '" + m.game.entities[e].name + "' entities than its capacity");
    if (r.u32() != m.columns.size()) throw std::runtime_error("save: field count mismatch");
    std::vector<std::vector<int32_t>> columns(m.columns.size());
    for (size_t c = 0; c < columns.size(); ++c) {
        if (r.str() != m.column_names[c]) throw std::runtime_error("save: field " + m.column_names[c] + " missing");
        columns[c] = r.words();
        if (columns[c].size() != m.columns[c].size()) throw std::runtime_error("save: field " + m.column_names[c] + " has the wrong size");
    }
    ModifierStore mods(m.game);
    mods.restore(r);
    if (r.pos != r.size) throw std::runtime_error("save: trailing data");

    // commit
    m.now = now;
    m.state = std::move(state);
    m.runner->write(1, m.state.data(), m.state.size() * 4);
    m.counts.assign(m.state.begin() + 1, m.state.end());
    m.size_tick_reads();  // per-tick readbacks follow the restored counts
    for (size_t c = 0; c < columns.size(); ++c) {
        m.columns[c] = std::move(columns[c]);
        m.runner->write(static_cast<uint32_t>(c + 2), m.columns[c].data(), m.columns[c].size() * 4);
    }
    for (auto& p : m.parents)
        if (p.active) {
            const auto& col = m.columns[p.binding - 2];
            std::memcpy(p.rows.data(), col.data(), p.rows.size() * 4);
            p.stale = false;
        }
    m.mods.restore_from(std::move(mods));
    for (size_t c = 0; c < m.mods.columns.size(); ++c) {  // upload recomputed totals now, so reads are consistent
        auto& col = m.mods.columns[c];
        m.runner->write(m.mod_binding0 + uint32_t(c), col.totals.data(), col.totals.size() * 4);
        col.dirty_lo = UINT32_MAX;
        col.dirty_hi = 0;
    }
    m.runner->discard_queued();
}

StateSnapshot GameSim::snapshot() {
    Impl& m = *impl_;
    m.runner->read(1, m.state.data(), m.state.size() * 4);
    StateSnapshot s;
    s.add("terni.state", m.state);
    for (size_t c = 0; c < m.columns.size(); ++c) {
        m.runner->read(static_cast<uint32_t>(c + 2), m.columns[c].data(), m.columns[c].size() * 4);
        s.add(m.column_names[c], m.columns[c]);
    }
    for (size_t c = 0; c < m.mods.columns.size(); ++c) {
        auto& col = m.mods.columns[c];
        std::vector<int32_t> gpu(col.totals.size());
        m.runner->read(m.mod_binding0 + uint32_t(c), gpu.data(), gpu.size() * 4);
        s.add("mod." + m.game.entities[col.entity].name + "." + m.game.modifier_keys[col.key].name, gpu);
    }
    return s;
}

std::string GameSim::device_name() const { return impl_->runner->device_name(); }
std::string GameSim::report() const { return impl_->runner->report(); }
CompileStats GameSim::compile_stats() const { return impl_->stats; }

}  // namespace terni
