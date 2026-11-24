// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// GameSim: GLSL prelude generation, the SPIR-V cache (rules are compiled by
// rule_compiler_*.cpp), the modifier store, and running a world on a Vulkan device.
//
// Binding layout (also the order of the generated GLSL):
//   0      defines                       D.<name>
//   1      state                         terni.tick, terni.<entity>_count
//   2 ...  one buffer per field          <entity>_<field>[]       (entities and fields in schema order)
//   then   per dynamic entity type:      <entity>_alive_bits[]    (one bit per row), <entity>_generation[],
//                                        and what giving out new rows needs (spawn_stage_sources)
//   then   one buffer per modifier key and entity it can sit on:
//                                        <entity>_modown_<key>[]  (key order; applies_to entity, then those it inherits from, nearest first)
//
// Specialization constants: 0, 1 are the runner's (compute.glsl); from 16 on, one capacity
// per entity type, <ENTITY>_CAPACITY, so the SPIR-V does not depend on capacities.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "terni/compute_runner.hpp"
#include "terni/fixed.hpp"
#include "terni/game.hpp"
#include "device_health.hpp"
#include "embedded_glsl.hpp"
#include "rule_compiler.hpp"
#include "utf8_path.hpp"

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
// 2: entity types, field types and bundle names by name, so a save can load into a
// patched game (SaveMatch::ByName). 1 is still read, into the game that made it.
constexpr uint32_t SAVE_VERSION = 2;

// The modifier store as a save holds it: instances by bundle and entity index, the free
// slots in order, pending expiries and the sequence counter.
struct SavedModifiers {
    struct Instance {
        uint32_t bundle = 0, entity = 0, row = 0;
        int32_t scale = 0;
        uint32_t version = 0;
        bool alive = false;
    };
    struct Due {
        uint64_t tick, seq;
        uint32_t slot, version;
    };
    uint64_t seq = 0;
    std::vector<Instance> instances;
    std::vector<uint32_t> free;
    std::vector<Due> due;

    void read(Reader& r) {
        seq = r.u64();
        instances.resize(r.u32());
        for (Instance& in : instances) {
            in.bundle = r.u32(), in.entity = r.u32(), in.row = r.u32(), in.scale = r.i32(), in.version = r.u32();
            in.alive = r.u8() != 0;
        }
        free.resize(r.u32());
        for (uint32_t& f : free) f = r.u32();
        due.resize(r.u32());
        for (Due& d : due) d.tick = r.u64(), d.seq = r.u64(), d.slot = r.u32(), d.version = r.u32();
    }
};

// Modifier total columns: for each key, the entity it applies to and (if it
// inherits) the entities it inherits from, nearest first. Shared by the prelude and the store.
struct ModColumn {
    uint32_t key, entity;
};
std::vector<ModColumn> modifier_columns(const Game& g) {
    std::vector<ModColumn> cols;
    for (uint32_t k = 0; k < g.modifier_keys.size(); ++k) {
        const auto& key = g.modifier_keys[k];
        cols.push_back({k, key.entity});
        for (const auto& step : key.inherit_steps) cols.push_back({k, step.entity});
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

    // Remove every instance on rows of `entity` that `gone(row)` names (rows that were
    // destroyed), in slot order.
    template <class F>
    void remove_on(uint32_t entity, F&& gone) {
        for (uint32_t s = 0; s < inst_.size(); ++s)
            if (inst_[s].alive && inst_[s].entity == entity && gone(inst_[s].row)) remove({s, inst_[s].version});
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

    void restore(const SavedModifiers& saved) {
        seq_ = saved.seq;
        if (saved.instances.size() > g_.modifier_capacity) throw std::runtime_error("save: more modifier instances than capacity");
        inst_.assign(saved.instances.size(), {});
        alive_ = 0;
        for (size_t i = 0; i < inst_.size(); ++i) {
            const auto& from = saved.instances[i];
            Instance& in = inst_[i];
            in.bundle = from.bundle, in.entity = from.entity, in.row = from.row, in.scale = from.scale;
            in.version = from.version, in.alive = from.alive;
            if (in.alive && (in.bundle >= g_.bundles.size() || in.entity >= g_.entities.size() ||
                             in.row >= g_.entities[in.entity].capacity))
                throw std::runtime_error("save: modifier instance out of range");
            alive_ += in.alive;
        }
        free_ = saved.free;
        for (uint32_t s : free_)
            if (s >= inst_.size() || inst_[s].alive) throw std::runtime_error("save: bad modifier free list");
        due_.clear();
        for (const auto& d : saved.due) due_.push_back({d.tick, d.seq, d.slot, d.version});
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
            for (const auto& e : std::filesystem::directory_iterator(detail::utf8_path(dir_) / "terni"))
                if (e.is_regular_file()) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const auto& f : files) {
                std::string content;
                const std::string name = detail::utf8_string(f.filename());
                if (read(("terni/" + name).c_str(), content)) w.str(name), w.str(content);
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
        std::ifstream f(detail::utf8_path(dir_) / requested, std::ios::binary);
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

// ---- new rows of dynamic entity types -------------------------------------------
// Rules ask for rows through count fields; four built-in stages at the end of every tick
// give them out, so that the result is the one a single thread would get going through
// the entity types, their spawn lines and the asking rows in order, each request taking
// the lowest free row:
//   wanted  per word of alive bits: which rows are free; per asking row: the rows it
//           wants, copied aside and added up per block of 256 rows
//   blocks  per 256 words: the free rows before each word; per 256 blocks of asking rows:
//           the rows wanted before each block
//   scan    (one thread) running totals over those, so each knows where it starts
//   assign  per asking row: its request k, k + 1, ... take the k-th, (k + 1)-th, ... free rows
// No stage reads what another thread of it writes, and none needs a barrier (software
// drivers run shaders with barriers far more slowly).
// A row is free if it is not alive now and was not alive when the last tick ended: one
// that is destroyed stays out of use for a whole tick, so that every death and every new
// row shows in the alive bits the host reads back, and rules holding a ref see it dead.
// A row the host destroys between ticks is dead for the whole of the next tick already,
// so it loses its prev bit and that tick's spawning may give it out again.
//
// Per dynamic entity type (capacity C, W = ceil(C / 32) words, B = ceil(W / 256) blocks),
// after its alive bits and generations:
//   terni_prev_<e>      alive when the last tick ended, plus rows the host created since
//                       and minus rows the host destroyed since
//   terni_cool_<e>      rows that died in the last tick or that the host destroyed since
//                       (the host must not take them yet)
//   terni_eligible_<e>  free rows, fixed before assign changes the alive bits
//   terni_scratch_<e>   [0, B) free rows before each block; [B] free rows in all; [B + 1, B + 1 + W)
//                       free rows before each word within its block; then per spawn line (R
//                       blocks of 256 asking rows at the asking type's capacity): the asking
//                       rows in use; per 256 blocks the rows wanted before them; per block
//                       the rows wanted before it within its 256. Zeroed when a tick starts.
//   terni_wanted_<e>_<i>  per spawn line: what each asking row wants, at most C
// Capacities are specialization constants, so the layout is computed in the shaders.
// Sums of wanted rows saturate at the capacity: more than that is never given out, and
// with capacities under 2^24 (checked when the world is built) nothing overflows on the way.
constexpr uint32_t SPAWN_BLOCK = 256;
const char* const DYNAMIC_OWN[] = {"prev", "cool", "eligible", "scratch"};  // after alive_bits and generation

uint32_t spawn_scratch_words(const Game& g, const EntityDef& e) {
    const uint32_t bit_words = (e.capacity + 31) / 32;
    uint32_t words = (bit_words + SPAWN_BLOCK - 1) / SPAWN_BLOCK + 1 + bit_words;
    for (const auto& s : e.spawns) {
        const uint32_t blocks = (g.entities[size_t(s.from_entity)].capacity + SPAWN_BLOCK - 1) / SPAWN_BLOCK;
        words += 1 + (blocks + SPAWN_BLOCK - 1) / SPAWN_BLOCK + blocks;
    }
    return words;
}

struct BuiltInStage {
    std::string name, body;
    uint32_t elements;  // threads to launch
};

// A field's default as GLSL, for one value.
std::string glsl_default(const FieldDef& f) {
    const uint64_t raw = static_cast<uint64_t>(f.default_raw);
    const std::string lo = std::to_string(uint32_t(raw)) + "u", hi = std::to_string(uint32_t(raw >> 32)) + "u";
    if (f.type == FieldType::Money64) return "uvec2(" + lo + ", " + hi + ")";
    return f.type == FieldType::Count || f.type == FieldType::Ref ? lo : "int(" + lo + ")";
}

std::vector<BuiltInStage> spawn_stage_sources(const Game& game) {
    std::ostringstream bits, wanted, blocks, scan, assign, helpers;
    for (auto* o : {&bits, &wanted, &blocks, &scan, &assign, &helpers}) o->imbue(std::locale::classic());
    uint32_t bit_elements = 1, wanted_elements = 1, block_elements = 1, assign_elements = 1;
    bool any = false;
    for (const auto& e : game.entities) {
        if (!e.dynamic) continue;
        any = true;
        const std::string& n = e.name;
        const std::string cap = upper(n) + "_CAPACITY", S = "terni_scratch_" + n, eligible = "terni_eligible_" + n,
                          prev = "terni_prev_" + n;
        const std::string layout = "    uint words = (" + cap + " + 31u) / 32u, blocks = (words + 255u) / 256u;\n"
                                   "    uint word_base = blocks + 1u, base = word_base + words;\n";
        const uint32_t bit_words = (e.capacity + 31) / 32;
        bit_elements = std::max(bit_elements, bit_words);
        block_elements = std::max(block_elements, (bit_words + SPAWN_BLOCK - 1) / SPAWN_BLOCK);

        // every tick, spawn lines or not: which rows are free and which died, and who is
        // alive as the tick ends, so that a destroyed row rests exactly one tick whenever the
        // lines run. Counting the free rows is for the lines' ticks only.
        bits << "  {  // " << n << "\n" << layout
             << "    FOR_EACH(w, words) {\n"
                "        uint rows = w + 1u == words && (" << cap << " & 31u) != 0u ? (1u << (" << cap << " & 31u)) - 1u : 0xFFFFFFFFu;\n"
                "        uint alive = " << n << "_alive_bits[w], before = " << prev << "[w];\n"
                "        " << eligible << "[w] = ~alive & ~before & rows;\n"
                "        terni_cool_" << n << "[w] = before & ~alive;\n"
                "        " << prev << "[w] = alive;  // rows made later in the tick add their bits\n"
                "    }\n"
                "  }\n";
        wanted << "  {  // " << n << "\n" << layout;
        blocks << "  {  // " << n << "\n" << layout
               << "    FOR_EACH(b, blocks) {\n"
                  "        uint free = 0u, end = min(words, b * 256u + 256u);\n"
                  "        for (uint w = b * 256u; w < end; ++w) {\n"
                  "            " << S << "[word_base + w] = free;\n"
                  "            free += uint(bitCount(" << eligible << "[w]));\n"
                  "        }\n"
                  "        " << S << "[b] = free;\n"
                  "    }\n";
        scan << "  {  // " << n << "\n" << layout
             << "    uint free = 0u, wanted = 0u;\n"
                "    for (uint b = 0u; b < blocks; ++b) { uint v = " << S << "[b]; " << S << "[b] = free; free += v; }\n"
                "    " << S << "[blocks] = free;\n";
        assign << "  {  // " << n << "\n" << layout << "    uint free = " << S << "[blocks];\n";

        // the k-th free row (k < free rows in all), and what makes a row a new entity
        helpers << "uint terni_free_" << n << "(uint k) {\n"
                << "    uint words = (" << cap << " + 31u) / 32u, blocks = (words + 255u) / 256u, word_base = blocks + 1u;\n"
                << "    uint lo = 0u, hi = blocks;\n"
                << "    while (hi - lo > 1u) { uint mid = (lo + hi) / 2u; if (" << S << "[mid] <= k) lo = mid; else hi = mid; }\n"
                << "    uint left = k - " << S << "[lo];\n"
                << "    hi = min(words, lo * 256u + 256u);\n"
                << "    lo *= 256u;\n"
                << "    while (hi - lo > 1u) { uint mid = (lo + hi) / 2u; if (" << S << "[word_base + mid] <= left) lo = mid; else hi = mid; }\n"
                << "    uint bits = " << eligible << "[lo];\n"
                << "    for (left -= " << S << "[word_base + lo]; left > 0u; --left) bits &= bits - 1u;\n"
                << "    return lo * 32u + uint(findLSB(bits));\n"
                << "}\n"
                << "void terni_new_" << n << "(uint row) {\n"
                << "    atomicOr(" << n << "_alive_bits[row >> 5], 1u << (row & 31u));\n"
                << "    atomicOr(" << prev << "[row >> 5], 1u << (row & 31u));\n"
                << "    atomicMax(terni." << n << "_count, row + 1u);\n"
                << "    " << n << "_generation[row] += 1u;\n";
        for (const auto& f : e.fields) {
            const std::string at = f.size > 1 ? "row * " + std::to_string(f.size) + "u + k" : "row";
            helpers << "    ";
            if (f.size > 1) helpers << "for (uint k = 0u; k < " << f.size << "u; ++k) ";
            helpers << n << "_" << f.name << "[" << at << "] = " << glsl_default(f) << ";\n";
        }
        helpers << "}\n";

        for (size_t i = 0; i < e.spawns.size(); ++i) {
            const SpawnDef& s = e.spawns[i];
            const EntityDef& from = game.entities[size_t(s.from_entity)];
            const std::string count = from.name + "_" + s.count_field;
            const std::string copy = "terni_wanted_" + n + "_" + std::to_string(i);
            const std::string lines = "        uint r0 = (" + upper(from.name) + "_CAPACITY + 255u) / 256u, r1 = (r0 + 255u) / 256u;\n"
                                      "        uint l1 = base + 1u, l0 = l1 + r1;\n";
            const std::string title = "    {  // from " + from.name + "." + s.count_field + "\n" + lines;
            // a line with a schedule works only in its ticks; the layout moves on regardless
            const std::string open = s.every == 1 ? "" : "        if (terni.tick % " + std::to_string(s.every) + "u == " +
                                                             std::to_string(s.offset) + "u) {\n";
            const std::string close = s.every == 1 ? "" : "        }\n";
            const uint32_t from_blocks = (from.capacity + SPAWN_BLOCK - 1) / SPAWN_BLOCK;
            wanted_elements = std::max(wanted_elements, from.capacity);
            block_elements = std::max(block_elements, (from_blocks + SPAWN_BLOCK - 1) / SPAWN_BLOCK);
            assign_elements = std::max(assign_elements, from.capacity);
            wanted << title << open
                   << "        FOR_EACH(p, terni." << from.name << "_count) {\n"
                      "            uint want = " << (from.dynamic ? from.name + "_alive(p)" : "true") << " ? min(" << count << "[p], "
                   << cap << ") : 0u;\n"
                      "            " << copy << "[p] = want;\n"
                      "            if (want != 0u) atomicAdd(" << S << "[l0 + (p >> 8)], want);\n"
                      "        }\n"
                   << close << "        base = l0 + r0;\n"
                      "    }\n";
            blocks << title << open
                   << "        uint used = (terni." << from.name << "_count + 255u) / 256u;\n"
                      "        FOR_EACH(g, (used + 255u) / 256u) {\n"
                      "            uint sum = 0u, end = min(used, g * 256u + 256u);\n"
                      "            for (uint b = g * 256u; b < end; ++b) {\n"
                      "                uint v = min(" << S << "[l0 + b], " << cap << ");\n"
                      "                " << S << "[l0 + b] = sum;\n"
                      "                sum += v;\n"
                      "            }\n"
                      "            " << S << "[l1 + g] = min(sum, " << cap << ");\n"
                      "        }\n"
                   << close << "        base = l0 + r0;\n"
                      "    }\n";
            scan << title << open
                 << "        uint asking = terni." << from.name << "_count;\n"
                    "        " << S << "[base] = asking;\n"
                    "        for (uint g = 0u; g < (asking + 65535u) / 65536u; ++g) {\n"
                    "            uint v = " << S << "[l1 + g];\n"
                    "            " << S << "[l1 + g] = wanted;\n"
                    "            wanted = min(wanted + v, " << cap << ");\n"
                    "        }\n"
                 << close << "        base = l0 + r0;\n"
                    "    }\n";
            assign << title << open
                   << "        FOR_EACH(p, " << S << "[base]) {\n"
                      "            if (" << copy << "[p] == 0u) continue;  // most rows ask for nothing\n"
                      "            uint at = min(" << S << "[l1 + (p >> 16)] + " << S << "[l0 + (p >> 8)], " << cap << ");\n"
                      "            if (at < free) {  // rows are left when its block starts\n"
                      "                for (uint q = p & ~255u; q < p; ++q) at += " << copy << "[q];\n"
                      "                uint want = " << count << "[p];\n"
                      "                uint made = at < free ? min(want, free - at) : 0u;\n"
                      "                for (uint j = 0u; j < made; ++j) {\n"
                      "                    uint row = terni_free_" << n << "(at + j);\n"
                      "                    terni_new_" << n << "(row);\n";
            if (!s.parent_field.empty()) assign << "                    " << n << "_" << s.parent_field << "[row] = p;\n";
            if (!s.ordinal_field.empty()) {
                const bool is_int = e.fields[size_t(e.field_index(s.ordinal_field))].type == FieldType::Int;
                assign << "                    " << n << "_" << s.ordinal_field << "[row] = " << (is_int ? "int(j)" : "j") << ";\n";
            }
            assign << "                }\n"
                      "                if (made != 0u) " << count << "[p] = want - made;\n"
                      "            }\n"
                      "        }\n"
                   << close << "        base = l0 + r0;\n"
                      "    }\n";
        }
        for (auto* o : {&wanted, &blocks, &scan, &assign}) *o << "  }\n";
    }
    if (!any) return {};
    // terni_spawn_bits comes first: the tick's scratch is zeroed before it (make_runner).
    // It is the only one a tick needs when no spawn line runs in it; the others are left
    // out of such ticks (stages_now).
    return {{"terni_spawn_bits", "void main() {\n" + bits.str() + "}\n", bit_elements},
            {"terni_spawn_wanted", "void main() {\n" + wanted.str() + "}\n", wanted_elements},
            {"terni_spawn_blocks", "void main() {\n" + blocks.str() + "}\n", block_elements},
            {"terni_spawn_scan", "void main() {\n  if (gl_GlobalInvocationID.x != 0u) return;\n" + scan.str() + "}\n", 1},
            {"terni_spawn_assign", helpers.str() + "void main() {\n" + assign.str() + "}\n", assign_elements}};
}

// The shader behind GameSim::field_hashes(): for the state and every column (in the order
// snapshot() lists them), the two sums of hash_word() (state_hash.hpp) over its words.
// Each thread sums its share and adds it with two atomics: integer sums do not depend on
// the order threads run in. Its output buffer comes after the modifier totals.
std::string hash_stage_source(const Game& game) {
    std::vector<std::pair<std::string, bool>> arrays;  // the GLSL array, and whether its elements are uvec2
    for (const auto& e : game.entities)
        for (const auto& f : e.fields) arrays.push_back({e.name + "_" + f.name, f.type == FieldType::Money64});
    for (const auto& e : game.entities) {
        if (!e.dynamic) continue;
        arrays.push_back({e.name + "_alive_bits", false});
        arrays.push_back({e.name + "_generation", false});
        for (const char* part : DYNAMIC_OWN) arrays.push_back({std::string("terni_") + part + "_" + e.name, false});
        for (size_t i = 0; i < e.spawns.size(); ++i) arrays.push_back({"terni_wanted_" + e.name + "_" + std::to_string(i), false});
    }
    for (const auto& c : modifier_columns(game))
        arrays.push_back({game.entities[c.entity].name + "_modown_" + game.modifier_keys[c.key].name, false});

    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << "layout(std430, binding = " << 2 + arrays.size() << ") buffer TerniHash { uint terni_hash[]; };\n"
         "uint terni_hash_mix(uint x) {\n"
         "    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;\n"
         "    return x;\n"
         "}\n"
         "void terni_hash_word(inout uint a, inout uint b, uint i, uint v) {\n"
         "    a += terni_hash_mix(v ^ terni_hash_mix(i + 0x9e3779b9u));\n"
         "    b += terni_hash_mix(((v + 0x632be5abu) * 0x85ebca6bu) ^ terni_hash_mix(i ^ 0x5bd1e995u));\n"
         "}\n"
         "void main() {\n"
         "    uint gid = gl_GlobalInvocationID.x, stride = gl_NumWorkGroups.x * gl_WorkGroupSize.x, a = 0u, b = 0u;\n"
         "    if (gid == 0u) {  // the state: the tick, then the counts\n"
         "        terni_hash_word(a, b, 0u, terni.tick);\n";
    for (size_t e = 0; e < game.entities.size(); ++e)
        o << "        terni_hash_word(a, b, " << e + 1 << "u, terni." << game.entities[e].name << "_count);\n";
    o << "        terni_hash[0] = a;\n        terni_hash[1] = b;\n    }\n";
    for (size_t k = 0; k < arrays.size(); ++k) {
        const std::string& n = arrays[k].first;
        o << "    a = 0u; b = 0u;\n";
        if (arrays[k].second)
            o << "    for (uint i = gid; i < uint(" << n << ".length()); i += stride) {\n"
              << "        terni_hash_word(a, b, 2u * i, " << n << "[i].x);\n"
              << "        terni_hash_word(a, b, 2u * i + 1u, " << n << "[i].y);\n    }\n";
        else
            o << "    for (uint i = gid; i < uint(" << n << ".length()); i += stride) terni_hash_word(a, b, i, uint(" << n
              << "[i]));\n";
        o << "    if (a != 0u) atomicAdd(terni_hash[" << 2 * (k + 1) << "], a);\n"
          << "    if (b != 0u) atomicAdd(terni_hash[" << 2 * (k + 1) + 1 << "], b);\n";
    }
    o << "}\n";
    return o.str();
}

// Every shader a world runs, in tick order: its rules, then the built-in stage that
// advances the tick counter (seeds the rng rolls), and last terni_hash, which is no
// stage but runs when the host asks (GameSim::field_hashes). GameSim and compile_rules() both
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
    for (const auto& s : spawn_stage_sources(game)) out.push_back({s.name, full("<terni>", s.body)});
    out.push_back({"terni_tick", full("<terni>", "void main() { if (gl_GlobalInvocationID.x == 0u) terni.tick += 1u; }")});
    out.push_back({"terni_hash", full("<terni>", hash_stage_source(game))});
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
    return detail::utf8_path(dir) / (std::string(hex) + ".spv");
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
// write (a read-only folder, say) only costs a compile at the next start. Several writers
// at once (two worlds starting, two programs compiling) are safe: a file that already
// holds the right shader is left alone (on Windows a file another process has open cannot
// be replaced), each writer has its own temporary file, and a rename that loses to another
// writer of the same shader is fine.
void cache_store(const std::string& dir, uint64_t key, const std::vector<uint32_t>& spirv) {
    std::vector<uint32_t> existing;
    if (cache_load(dir, key, existing) && existing == spirv) return;
    std::error_code ec;
    std::filesystem::create_directories(detail::utf8_path(dir), ec);
    Writer body;
    for (uint32_t v : spirv) body.u32(v);
    Writer w;
    w.buf = {'T', 'S', 'P', 'V'};
    w.u64(key), w.u32(static_cast<uint32_t>(spirv.size())), w.u64(fnv64(body.buf.data(), body.buf.size()));
    w.buf.insert(w.buf.end(), body.buf.begin(), body.buf.end());
    const std::filesystem::path file = cache_file(dir, key);
    static std::atomic<uint64_t> writes{0};
    std::ostringstream unique;
    unique.imbue(std::locale::classic());
    unique << std::hex << std::hash<std::thread::id>{}(std::this_thread::get_id()) << '-'
           << std::chrono::steady_clock::now().time_since_epoch().count() << '-' << writes++;
    std::filesystem::path tmp = file;
    tmp += "." + unique.str() + ".tmp";
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
    detail::RuleCompiler compiler;  // one for all of them
    std::vector<std::vector<uint32_t>> out(rules.size());
    std::string missing;
    size_t missing_count = 0;
    for (size_t i = 0; i < rules.size(); ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t key = cache_key(rules[i].source, library);
        if (!cache_dir.empty() && cache_load(cache_dir, key, out[i])) {
            ++stats.cached;
        } else if (may_compile) {
            out[i] = compiler.compile(rules[i].name, rules[i].source, include);
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

namespace {

// What DIV_DEFINE (shaders/terni/fixed.glsl) needs to divide by a define's magnitude d:
// with l = ceil(log2 d), the multiplier floor(2^32 * (2^l - d) / d) + 1 and the shifts
// min(l, 1) and max(l - 1, 0). Zeros for a define of 0, which rules must not divide by.
std::array<uint32_t, 3> define_reciprocal(int32_t define) {
    const uint32_t d = define < 0 ? 0u - static_cast<uint32_t>(define) : static_cast<uint32_t>(define);
    if (d == 0) return {0, 0, 0};
    const uint32_t l = static_cast<uint32_t>(std::bit_width(d - 1));
    const uint64_t excess = (uint64_t(1) << l) - d;  // under d, so the quotient has 32 bits
    return {static_cast<uint32_t>((excess << 32) / d + 1), std::min(l, 1u), l > 0 ? l - 1 : 0};
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
    for (const auto& d : game.defines)  // for DIV_DEFINE, in define_reciprocal()'s order
        o << "    uint terni_mul_" << d.name << ", terni_shift1_" << d.name << ", terni_shift2_" << d.name << ";\n";
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
        // a dynamic entity type's loops leave out the rows that are not alive
        const std::string alive = e.dynamic ? " if (" + e.name + "_alive(i))" : "";
        o << "#define FOR_EACH_" << upper(e.name) << "(i) FOR_EACH(i, terni." << e.name << "_count)" << alive << "\n";
        o << "#define FOR_EACH_" << upper(e.name) << "_GROUP(i) FOR_EACH_GROUP(i, terni." << e.name << "_count)" << alive
          << "\n";
        for (const auto& f : e.fields) {
            if (f.size > 1) o << "const uint " << upper(e.name + "_" + f.name) << "_SIZE = " << f.size << "u;\n";
            o << "layout(std430, binding = " << binding++ << ") buffer " << e.name << "_" << f.name << "_buf { "
              << glsl_type(f.type) << " " << e.name << "_" << f.name << "[]; };\n";
        }
    }
    for (const auto& e : game.entities) {
        if (!e.dynamic) continue;
        const std::string& n = e.name;
        o << "\n// " << n << ": rows come and go\n";
        o << "layout(std430, binding = " << binding++ << ") buffer " << n << "_alive_bits_buf { uint " << n
          << "_alive_bits[]; };\n";
        o << "layout(std430, binding = " << binding++ << ") buffer " << n << "_generation_buf { uint " << n
          << "_generation[]; };\n";
        auto own = [&](const std::string& name) {  // for the spawn stages (spawn_stage_sources)
            o << "layout(std430, binding = " << binding++ << ") buffer " << name << "_buf { uint " << name << "[]; };\n";
        };
        for (const char* part : DYNAMIC_OWN) own(std::string("terni_") + part + "_" + n);
        for (size_t i = 0; i < e.spawns.size(); ++i) own("terni_wanted_" + n + "_" + std::to_string(i));
        o << "bool " << n << "_alive(uint i) { return i < " << upper(n) << "_CAPACITY && (" << n
          << "_alive_bits[i >> 5] & (1u << (i & 31u))) != 0u; }\n";
        // an atomic: rows that share a word may be destroyed by different threads
        o << "void " << n << "_destroy(uint i) { if (i < " << upper(n) << "_CAPACITY) atomicAnd(" << n
          << "_alive_bits[i >> 5], ~(1u << (i & 31u))); }\n";
    }
    const auto cols = modifier_columns(game);
    if (!cols.empty()) {
        o << "\n// modifier totals (own instances only; use the accessors below)\n";
        for (const auto& c : cols) {
            const std::string n = game.entities[c.entity].name + "_modown_" + game.modifier_keys[c.key].name;
            o << "layout(std430, binding = " << binding++ << ") buffer " << n << "_buf { int " << n << "[]; };\n";
        }
        o << "\n// modifier accessors: own total + inherited total\n";
        for (const auto& key : game.modifier_keys) {
            // level 0 is the key's own entity; level j inherits from level j + 1
            const size_t top = key.inherit_steps.size();
            auto entity_at = [&](size_t level) -> const EntityDef& {
                return game.entities[level == 0 ? key.entity : key.inherit_steps[level - 1].entity];
            };
            auto accessor = [&](size_t level) {
                const std::string& en = entity_at(level).name;
                o << "int " << en << "_mod_" << key.name << "(uint i) { return " << en << "_modown_" << key.name << "[i]";
                if (level < top) {
                    const std::string& pn = entity_at(level + 1).name;
                    const std::string ref = en + "_" + entity_at(level).fields[key.inherit_steps[level].field].name + "[i]";
                    o << " + (" << ref << " < " << upper(pn) << "_CAPACITY ? " << pn;
                    if (level + 1 < top) o << "_mod_" << key.name << "(" << ref << ")";  // it inherits in turn
                    else o << "_modown_" << key.name << "[" << ref << "]";
                    o << " : 0)";
                }
                o << "; }\n";
            };
            // GLSL wants a function before its callers: the levels that call the next one
            // go from the top down; the top one calls nothing and nothing calls it
            for (size_t level = top; level-- > 0;) accessor(level);
            accessor(top);
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
    // The columns in binding order: the values live on the device only (and the starting
    // ones in the game's tables), the host keeps their sizes.
    std::vector<size_t> column_words;
    std::vector<std::string> column_names;
    // What each column is: a field of an entity type, or one of Terni's own columns of a
    // dynamic type (`own`: alive_bits, generation, terni_prev, ...).
    struct ColumnInfo {
        uint32_t entity = 0;
        int field = -1;
        std::string own;
    };
    std::vector<ColumnInfo> column_info;
    std::vector<std::vector<uint32_t>> field_binding;  // [entity][field] -> binding
    uint32_t mod_binding0 = 0;
    ModifierStore mods;
    std::vector<uint32_t> counts;  // rows in use per entity: the starting counts, or those restored by load()
    struct TickRead {
        uint32_t readback;  // ComputeRunner id
        uint32_t entity, first_row;
        uint32_t rows;       // UINT32_MAX: every row in use, following the entity's count
        uint64_t row_words;  // field size x words per value
        uint32_t binding = 0;
        uint64_t most = 0, offset = 0;  // bytes reserved in the runner, and where the range starts
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
        // host writes made while a tick was in flight: they apply to the tick after it, so
        // they go back on top of that tick's results when it is collected
        std::vector<std::pair<uint32_t, uint32_t>> pending;  // (row, value)
    };
    // A dynamic entity type on the host: which rows are alive and how often each was
    // created, as of the last collected tick plus host calls since. The built-in spawn
    // stages give out the rows rules ask for; the host follows through the alive bits that
    // come back with every tick, and gives out rows itself for create_entity(), by the
    // same rule: a row is free if it is not alive, did not die in the last tick and was
    // not destroyed by the host since (cool); the spawn stages of the next tick may take
    // a row the host destroyed (not prev).
    struct Dynamic {
        uint32_t entity = 0;
        uint32_t alive_binding = 0;        // then generation, and Terni's own (spawn_stage_sources)
        std::vector<uint32_t> alive;       // one bit per row
        std::vector<uint32_t> prev, cool;  // as on the device (spawn_stage_sources)
        std::vector<uint32_t> generation;  // per row: times it was created
        uint32_t alive_count = 0;
        uint32_t free_word = 0;  // no word below this one has a free row
        TickRead alive_read{};   // the bits, back with every tick
        std::vector<std::vector<int32_t>> default_row;  // per field: the words of a new row
        bool is_alive(uint32_t row) const { return row / 32 < alive.size() && (alive[row / 32] >> (row % 32) & 1u) != 0; }
        uint32_t generation_binding() const { return alive_binding + 1; }
        uint32_t prev_binding() const { return alive_binding + 2; }
        uint32_t cool_binding() const { return alive_binding + 3; }
    };
    std::vector<Dynamic> dynamics;
    std::vector<int> dynamic_of;  // per entity: index into `dynamics`, or -1
    std::vector<Parents> parents;
    std::vector<std::vector<uint32_t>> key_parents;  // per modifier key and inherit step: index into `parents`
    std::vector<uint64_t> row_words;  // per binding: words per row (order check reports)
    uint64_t now = 0;      // completed ticks
    bool in_flight = false;  // begin_tick() without end_tick()
    CompileStats stats;
    std::vector<std::vector<uint32_t>> spirv;  // per stage, then the built-in last one; kept for move_to_device()
    std::vector<uint32_t> hash_spirv;          // terni_hash (rule_sources): run on demand, not a stage
    uint32_t hash_binding = 0;                 // its output: two words per field of snapshot()
    std::vector<int32_t> hash_zero;            // what it starts from
    int hash_pipeline = -1;                    // on the current runner, once made
    std::vector<std::string> stage_names;      // the runner's stages: the rules', then the built-in ones
    bool validation = false;
    uint32_t spin_us = 0;
    detail::DeviceHealthMonitor health;
    uint64_t earlier_sync_reads = 0;  // on runners this simulation has moved away from
    std::unique_ptr<ComputeRunner> runner;
    explicit Impl(const Game& g) : game(g), mods(g) {}

    std::vector<uint32_t> bindings_of(const std::vector<std::string>& fields) const {
        std::vector<uint32_t> out;
        for (const auto& f : fields)
            for (size_t k = 0; k < column_names.size(); ++k)
                if (column_names[k] == f) out.push_back(static_cast<uint32_t>(k + 2));
        return out;
    }

    // A runner on `device` that starts from `defines`, `state`, the columns and the modifier
    // totals given (the store's own if null). It uploads them before it returns.
    std::unique_ptr<ComputeRunner> make_runner(uint32_t device, const std::vector<std::span<const int32_t>>& columns,
                                               const std::vector<std::vector<int32_t>>* totals) const {
        std::vector<ComputeBufferSpec> buffers;
        buffers.push_back({"defines", defines.data(), defines.size() * 4});
        buffers.push_back({"state", state.data(), state.size() * 4});
        for (size_t c = 0; c < columns.size(); ++c)
            buffers.push_back({column_names[c], columns[c].data(), columns[c].size() * 4});
        for (size_t c = 0; c < mods.columns.size(); ++c) {
            const std::vector<int32_t>& t = totals ? (*totals)[c] : mods.columns[c].totals;
            buffers.push_back({"mod", t.data(), t.size() * 4});
        }
        buffers.push_back({"terni.hash", hash_zero.data(), hash_zero.size() * 4});

        std::vector<ComputeStageSpec> stages;
        for (size_t i = 0; i < game.stages.size(); ++i) {
            const auto& s = game.stages[i];
            const uint32_t capacity = game.entity(s.for_each)->capacity;
            // workgroup dispatch: one 256-thread group per entity (FOR_EACH_GROUP strides past 65535)
            const uint32_t elements = s.dispatch == Dispatch::Workgroup ? std::min(capacity, 65535u) * 256u : capacity;
            // cleared only in the ticks the stage runs, so a total lasts until its next run
            stages.push_back({s.name, elements, spirv[i], bindings_of(s.clear)});
        }
        const auto built_in = spawn_stage_sources(game);  // after the rules, as in rule_sources
        std::vector<uint32_t> scratch;  // zeroed when a tick starts
        for (size_t k = 0; k < column_names.size(); ++k)
            if (column_names[k].ends_with(".terni_scratch")) scratch.push_back(static_cast<uint32_t>(k + 2));
        for (size_t i = 0; i < built_in.size(); ++i) {
            stages.push_back({built_in[i].name, built_in[i].elements, spirv[game.stages.size() + i],
                              i == 0 ? scratch : std::vector<uint32_t>{}});
        }
        stages.push_back({"terni_tick", 1, spirv.back(), {}});  // built-in last stage (rule_sources)

        std::vector<std::vector<uint32_t>> clears;
        for (const auto& c : game.clears) clears.push_back(bindings_of(c.fields));

        std::vector<ComputeSpecConstant> capacities;
        for (size_t e = 0; e < game.entities.size(); ++e)
            capacities.push_back({CAPACITY_CONSTANT0 + uint32_t(e), game.entities[e].capacity});
        auto made = std::make_unique<ComputeRunner>(device, validation, "", buffers, stages, std::vector<uint32_t>{},
                                                    true, capacities, clears);
        made->set_tick_spin(spin_us);
        return made;
    }

    // The rows that count towards `key` on (entity, row): the row itself, then the rows it
    // inherits from, nearest first. Stops at a ref that points at no row, as the accessor does.
    template <class F>
    void for_each_level(uint32_t key, uint32_t entity, uint32_t row, F&& visit) {
        const auto& steps = game.modifier_keys.at(key).inherit_steps;
        visit(entity, row);
        size_t level = 0;  // where `entity` sits in the chain; other entity types inherit nothing
        if (entity != game.modifier_keys[key].entity) {
            while (level < steps.size() && steps[level].entity != entity) ++level;
            ++level;
        }
        for (; level < steps.size(); ++level) {
            row = parent_row(parents[key_parents[key][level]], row);
            entity = steps[level].entity;
            if (row >= game.entities[entity].capacity) return;
            visit(entity, row);
        }
    }
    // The row that `row` refers to through an inherit field, without touching the GPU after
    // the first call.
    uint32_t parent_row(Parents& p, uint32_t row) {
        if (!p.active) {
            settle();
            runner->flush_queued();  // queued host writes belong to the current state
            p.rows.resize(game.entities[p.entity].capacity);
            runner->read(p.binding, p.rows.data(), p.rows.size() * 4);
            p.read = {0, p.entity, 0, UINT32_MAX, 1, p.binding, p.rows.size() * 4, 0};
            p.read.readback = runner->add_tick_readback(p.binding, p.read.most, 0);
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
    // Which stages run in the coming tick (the built-in last one always does), and after
    // them which of the game's clears.
    std::vector<bool> stages_now() const {
        std::vector<bool> run(spirv.size(), true);
        for (size_t s = 0; s < game.stages.size(); ++s) run[s] = game.stages[s].runs_in(now);
        // the spawn stages only in ticks some line runs in; terni_spawn_bits always
        bool lines = false;
        for (const auto& e : game.entities)
            for (const auto& line : e.spawns) lines |= line.runs_in(now);
        for (size_t s = game.stages.size(); s < stage_names.size(); ++s)
            if (stage_names[s].starts_with("terni_spawn_") && stage_names[s] != "terni_spawn_bits") run[s] = lines;
        for (const auto& c : game.clears) run.push_back(c.runs_in(now));
        return run;
    }
    void read_column(size_t c, std::vector<int32_t>& words) {
        words.resize(column_words[c]);
        runner->read(static_cast<uint32_t>(c + 2), words.data(), words.size() * 4);
    }
    // Column `c` as the game starts it: a field as the game's tables hold it (history
    // values, defaults elsewhere), the rows of a dynamic type alive up to the starting count.
    std::vector<int32_t> starting_column(size_t c) const {
        const ColumnInfo& info = column_info[c];
        const EntityTable& t = game.tables[info.entity];
        if (info.field >= 0) return t.columns[size_t(info.field)];
        std::vector<int32_t> words(column_words[c], 0);
        if (info.own == "alive_bits" || info.own == "terni_prev")
            for (uint32_t row = 0; row < t.count; ++row) words[row / 32] |= int32_t(1u << (row % 32));
        else if (info.own == "generation")
            for (uint32_t row = 0; row < t.count; ++row) words[row] = 1;
        return words;
    }
    // A field's default in every row: what a new field of a dynamic type starts with, whose
    // rows have been reused since the start.
    std::vector<int32_t> default_column(size_t c) const {
        const ColumnInfo& info = column_info[c];
        const FieldDef& f = game.entities[info.entity].fields[size_t(info.field)];
        std::vector<int32_t> words(column_words[c]);
        for (size_t v = 0; v < words.size() / f.words(); ++v) {
            words[v * f.words()] = static_cast<int32_t>(static_cast<uint64_t>(f.default_raw));
            if (f.words() == 2) words[v * 2 + 1] = static_cast<int32_t>(static_cast<uint64_t>(f.default_raw) >> 32);
        }
        return words;
    }

    // ---- dynamic entity types ----
    Dynamic& dynamic(uint32_t entity, const char* call) {
        const int d = dynamic_of.at(entity);
        if (d < 0)
            throw std::invalid_argument(std::string(call) + ": '" + game.entities[entity].name +
                                        "' is not a dynamic entity type");
        return dynamics[size_t(d)];
    }
    // The lowest free row becomes alive; NO_ENTITY if there is none. The GPU hears of it
    // through created().
    uint32_t allocate(Dynamic& d) {
        const uint32_t capacity = game.entities[d.entity].capacity;
        auto taken = [&](uint32_t w) { return d.alive[w] | d.prev[w] | d.cool[w]; };
        while (d.free_word < d.alive.size() && taken(d.free_word) == 0xFFFFFFFFu) ++d.free_word;
        if (d.free_word >= d.alive.size()) return NO_ENTITY;
        const uint32_t row = d.free_word * 32 + uint32_t(std::countr_one(taken(d.free_word)));
        if (row >= capacity) return NO_ENTITY;  // only the bits past the capacity are left
        d.alive[row / 32] |= 1u << (row % 32);
        d.prev[row / 32] |= 1u << (row % 32);  // if it dies in the coming tick, that tick must not reuse it
        ++d.generation[row];
        ++d.alive_count;
        if (row >= counts[d.entity]) {
            in_use(d.entity, row + 1);
            runner->queue_write(1, &state[1 + d.entity], 4, uint64_t(1 + d.entity) * 4);
        }
        return row;
    }
    // The rows in use of an entity type grow (they never shrink).
    void in_use(uint32_t entity, uint32_t count) {
        counts[entity] = count;
        state[1 + entity] = count;
        size_tick_reads();
    }
    // Queue what the GPU needs to know about a row the host just allocated: alive bits,
    // generation and every field at its default.
    void created(Dynamic& d, uint32_t row) {
        const uint64_t w = row / 32;
        runner->queue_write(d.alive_binding, &d.alive[w], 4, w * 4);
        runner->queue_write(d.prev_binding(), &d.prev[w], 4, w * 4);
        runner->queue_write(d.generation_binding(), &d.generation[row], 4, uint64_t(row) * 4);
        for (size_t f = 0; f < d.default_row.size(); ++f) {
            const auto& one = d.default_row[f];
            runner->queue_write(field_binding[d.entity][f], one.data(), one.size() * 4, uint64_t(row) * one.size() * 4);
            mirror_write(d.entity, uint32_t(f), row, one.data(), one.size() == 1 ? 1 : 0);
        }
    }
    // After a tick: follow what the rules and the spawn stages did to the alive bits. A bit
    // that went out is a row destroyed (its modifiers go); one that came on is a new row.
    // Neither can be missed: a destroyed row is not given out again in the same tick.
    void collect_dynamic() {
        for (auto& d : dynamics) {
            const auto* bits = static_cast<const uint32_t*>(runner->tick_readback(d.alive_read.readback));
            bool deaths = false;
            uint32_t top = 0;
            for (size_t w = 0; w < d.alive.size(); ++w) {
                const uint32_t was = d.alive[w], is = bits[w];  // alive before and after the tick
                d.cool[w] = d.prev[w] & ~is;
                d.prev[w] = is;
                if (is == was) continue;
                deaths |= (was & ~is) != 0;
                for (uint32_t born = is & ~was; born != 0; born &= born - 1) {
                    const uint32_t row = uint32_t(w) * 32 + uint32_t(std::countr_zero(born));
                    ++d.generation[row];
                    top = std::max(top, row + 1);
                }
                d.alive_count = d.alive_count - uint32_t(std::popcount(was)) + uint32_t(std::popcount(is));
                d.alive[w] = is;
            }
            d.free_word = 0;  // rows that were cooling are free again
            if (top > counts[d.entity]) in_use(d.entity, top);
            if (deaths) mods.remove_on(d.entity, [&](uint32_t row) { return !d.is_alive(row); });
        }
    }

    void after_tick(bool measured = true) {
        ++now;
        if (measured) health.add(runner->last_tick_device_ms());  // what the device did, not when the host looked
        for (auto& p : parents) {
            p.stale = p.active;
            if (!p.active || p.pending.empty()) continue;
            refresh(p);
            for (const auto& [row, value] : p.pending) p.rows[row] = value;
            p.pending.clear();
        }
        collect_dynamic();
    }
    // The tick host calls belong to: while tick N runs they already act between N and N+1,
    // exactly as if made after end_tick(), so peers that collect at other moments agree.
    uint64_t host_now() const { return now + (in_flight ? 1 : 0); }
    // Collect a tick in flight; for calls that need the GPU state as of the last tick.
    void settle() {
        if (!in_flight) return;
        runner->tick_wait();
        in_flight = false;
        after_tick();
    }
    // A host write to rows of a field: keep the inherit mirrors in step.
    void mirror_write(uint32_t entity, uint32_t field, uint32_t row, const int32_t* values, size_t n) {
        for (auto& p : parents) {
            if (!p.active || p.entity != entity || p.field != field) continue;
            if (p.stale) refresh(p);  // the last tick's values first, then this write on top
            std::memcpy(p.rows.data() + row, values, n * 4);
            if (in_flight)
                for (size_t k = 0; k < n; ++k) p.pending.push_back({uint32_t(row + k), uint32_t(values[k])});
        }
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
    m.hash_spirv = std::move(spirv.back());  // last in rule_sources, and no stage
    spirv.pop_back();
    int device = options.device;
    if (device < 0) {
        const std::array<uint8_t, 16>* uuid = options.device_uuid ? &*options.device_uuid : nullptr;
        const std::array<uint8_t, 8>* luid = options.device_luid ? &*options.device_luid : nullptr;
        device = pick_device(options.preference, uuid, luid);
        if (device < 0) throw std::runtime_error("no usable Vulkan device (Vulkan 1.3 with synchronization2 required)");
    }
    m.defines = game.define_values;
    for (const int32_t d : game.define_values)
        for (const uint32_t word : define_reciprocal(d)) m.defines.push_back(static_cast<int32_t>(word));
    if (m.defines.empty()) m.defines.push_back(0);
    m.state.push_back(0);  // tick
    for (const auto& t : game.tables) m.state.push_back(t.count), m.counts.push_back(t.count);
    for (const auto& im : game.initial_modifiers) m.mods.add(im.bundle, im.entity, im.row, im.scale, im.duration, 0);

    std::vector<std::span<const int32_t>> start;  // the starting columns: fields straight from the game's tables,
    std::vector<std::vector<int32_t>> own_start;  // Terni's own from here (moving a vector keeps its words in place)
    m.field_binding.resize(game.entities.size());
    for (size_t e = 0; e < game.tables.size(); ++e) {
        const auto& t = game.tables[e];
        for (size_t f = 0; f < t.def->fields.size(); ++f) {
            m.field_binding[e].push_back(static_cast<uint32_t>(2 + m.column_words.size()));
            m.column_words.push_back(t.columns[f].size());
            start.push_back(t.columns[f]);
            m.column_names.push_back(t.def->name + "." + t.def->fields[f].name);
            m.column_info.push_back({uint32_t(e), int(f), ""});
        }
    }
    m.dynamic_of.assign(game.entities.size(), -1);
    for (uint32_t e = 0; e < game.entities.size(); ++e) {
        const EntityDef& def = game.entities[e];
        if (!def.dynamic) continue;
        Impl::Dynamic d;
        d.entity = e;
        d.alive.assign((def.capacity + 31) / 32, 0);
        d.generation.assign(def.capacity, 0);
        for (uint32_t row = 0; row < game.tables[e].count; ++row) d.alive[row / 32] |= 1u << (row % 32), d.generation[row] = 1;
        d.alive_count = game.tables[e].count;
        d.prev = d.alive;  // the starting rows count as alive before the first tick
        d.cool.assign(d.alive.size(), 0);
        d.alive_binding = static_cast<uint32_t>(2 + m.column_words.size());
        const std::vector<int32_t> bits(d.alive.begin(), d.alive.end()), none(d.alive.size(), 0);
        auto column = [&](const std::string& name, std::vector<int32_t> words) {
            m.column_words.push_back(words.size());
            own_start.push_back(std::move(words));
            start.push_back(own_start.back());
            m.column_names.push_back(def.name + "." + name);
            m.column_info.push_back({e, -1, name});
        };
        column("alive_bits", bits);
        column("generation", std::vector<int32_t>(d.generation.begin(), d.generation.end()));
        column("terni_prev", bits);
        column("terni_cool", none);
        column("terni_eligible", none);
        column("terni_scratch", std::vector<int32_t>(spawn_scratch_words(game, def), 0));
        for (size_t i = 0; i < def.spawns.size(); ++i)
            column("terni_wanted_" + std::to_string(i),
                   std::vector<int32_t>(game.entities[size_t(def.spawns[i].from_entity)].capacity, 0));
        for (const auto& f : def.fields) {
            std::vector<int32_t> one;
            for (uint32_t k = 0; k < f.size; ++k) {
                one.push_back(static_cast<int32_t>(static_cast<uint64_t>(f.default_raw)));
                if (f.words() == 2) one.push_back(static_cast<int32_t>(static_cast<uint64_t>(f.default_raw) >> 32));
            }
            d.default_row.push_back(std::move(one));
        }
        m.dynamic_of[e] = int(m.dynamics.size());
        m.dynamics.push_back(std::move(d));
    }
    m.mod_binding0 = static_cast<uint32_t>(2 + m.column_words.size());
    for (auto& c : m.mods.columns) {
        c.dirty_lo = UINT32_MAX;  // initial modifiers are part of the upload
        c.dirty_hi = 0;
    }
    m.row_words.assign(2, 1);  // defines, state
    for (const auto& e : game.entities)
        for (const auto& f : e.fields) m.row_words.push_back(uint64_t(f.size) * f.words());
    m.row_words.resize(m.mod_binding0 + m.mods.columns.size(), 1);  // modifier totals
    m.hash_binding = m.mod_binding0 + static_cast<uint32_t>(m.mods.columns.size());
    m.hash_zero.assign(2 * (1 + m.column_words.size() + m.mods.columns.size()), 0);
    m.row_words.push_back(1);  // the hashes
    for (const auto& k : game.modifier_keys) {
        std::vector<uint32_t> indices;
        uint32_t from = k.entity;
        for (const auto& step : k.inherit_steps) {  // keys that share a field share its mirror
            size_t i = 0;
            while (i < m.parents.size() && (m.parents[i].entity != from || m.parents[i].field != step.field)) ++i;
            if (i == m.parents.size()) m.parents.push_back({from, step.field, m.field_binding[from][step.field]});
            indices.push_back(uint32_t(i));
            from = step.entity;
        }
        m.key_parents.push_back(std::move(indices));
    }

    m.spirv = std::move(spirv);
    m.validation = options.validation;
    m.spin_us = options.spin_us;
    m.runner = m.make_runner(static_cast<uint32_t>(device), start, nullptr);
    for (const auto& st : game.stages) m.stage_names.push_back(st.name);
    for (const auto& st : spawn_stage_sources(game)) m.stage_names.push_back(st.name);
    m.stage_names.push_back("terni_tick");
    // what creating rows needs to know comes back with every tick
    for (auto& d : m.dynamics) {
        d.alive_read = {0, d.entity, 0, 0, 1, d.alive_binding, d.alive.size() * 4, 0};
        d.alive_read.readback = m.runner->add_tick_readback(d.alive_binding, d.alive_read.most, 0);
    }
}

void GameSim::move_to_device(uint32_t device) {
    Impl& m = *impl_;
    m.settle();
    // The state as the device has it, so that a snapshot() is the same wherever it is taken.
    // Modifier totals too: the host's own may be ahead of them (changes since the last
    // tick), and reach the new device at the next tick as they would have reached this
    // one. So do the host writes queued for that tick.
    m.runner->read(1, m.state.data(), m.state.size() * 4);
    std::vector<std::vector<int32_t>> columns(m.column_words.size());
    for (size_t c = 0; c < columns.size(); ++c) m.read_column(c, columns[c]);
    std::vector<std::vector<int32_t>> totals;
    for (size_t c = 0; c < m.mods.columns.size(); ++c) {
        totals.emplace_back(m.mods.columns[c].totals.size());
        m.runner->read(m.mod_binding0 + uint32_t(c), totals[c].data(), totals[c].size() * 4);
    }
    std::unique_ptr<ComputeRunner> next = m.make_runner(device, {columns.begin(), columns.end()}, &totals);

    // per-tick readbacks: the same ranges, holding the last tick's results
    auto carry = [&](Impl::TickRead& r) {
        const uint32_t id = next->add_tick_readback(r.binding, r.most, r.offset);
        const uint64_t bytes = m.runner->tick_readback_bytes(r.readback);
        next->set_tick_readback_bytes(id, bytes);
        next->set_tick_readback(id, m.runner->tick_readback(r.readback), bytes);
        r.readback = id;
    };
    for (auto& r : m.tick_reads) carry(r);
    for (auto& p : m.parents)
        if (p.active) carry(p.read);
    for (auto& d : m.dynamics) carry(d.alive_read);
    next->take_queued(*m.runner);
    m.earlier_sync_reads += m.runner->sync_reads();
    m.runner = std::move(next);
    m.hash_pipeline = -1;  // made again on the new device when asked for
    m.health.reset();  // another device has other tick times
}

DeviceHealth GameSim::device_health() const { return impl_->health.health(); }

uint32_t GameSim::create_entity(uint32_t entity) {
    Impl& m = *impl_;
    Impl::Dynamic& d = m.dynamic(entity, "create_entity");
    m.settle();  // which rows are free depends on the tick in flight
    const uint32_t row = m.allocate(d);
    if (row != NO_ENTITY) m.created(d, row);
    return row;
}

bool GameSim::destroy_entity(uint32_t entity, uint32_t row) {
    Impl& m = *impl_;
    Impl::Dynamic& d = m.dynamic(entity, "destroy_entity");
    m.settle();
    if (!d.is_alive(row)) return false;
    // Dead for the rules from the next tick on, as a row a rule destroyed in the tick
    // before: not free for the host until that tick has run (cool), free for its spawn
    // stages (not prev). After it, the host's collect clears cool, as on the device.
    const uint32_t w = row / 32, bit = 1u << (row % 32);
    d.alive[w] &= ~bit;
    d.prev[w] &= ~bit;
    d.cool[w] |= bit;
    --d.alive_count;
    m.runner->queue_write(d.alive_binding, &d.alive[w], 4, uint64_t(w) * 4);
    m.runner->queue_write(d.prev_binding(), &d.prev[w], 4, uint64_t(w) * 4);
    m.runner->queue_write(d.cool_binding(), &d.cool[w], 4, uint64_t(w) * 4);
    m.mods.remove_on(entity, [&](uint32_t r) { return r == row; });
    return true;
}

bool GameSim::entity_alive(uint32_t entity, uint32_t row) const {
    const Impl& m = *impl_;
    const int d = m.dynamic_of.at(entity);
    return d >= 0 ? m.dynamics[size_t(d)].is_alive(row) : row < m.counts[entity];
}

std::span<const uint32_t> GameSim::alive_bits(uint32_t entity) const {
    const Impl& m = *impl_;
    const int d = m.dynamic_of.at(entity);
    if (d < 0)
        throw std::invalid_argument("alive_bits: '" + m.game.entities[entity].name + "' is not a dynamic entity type");
    return m.dynamics[size_t(d)].alive;
}

uint32_t GameSim::entities_free(uint32_t entity) {
    Impl& m = *impl_;
    Impl::Dynamic& d = m.dynamic(entity, "entities_free");
    m.settle();
    const uint32_t capacity = m.game.entities[entity].capacity;
    uint32_t free = 0;
    for (size_t w = 0; w < d.alive.size(); ++w) {
        uint32_t open = ~(d.alive[w] | d.prev[w] | d.cool[w]);  // as allocate() sees them
        if ((w + 1) * 32 > capacity) open &= (1u << (capacity % 32)) - 1u;  // past the capacity
        free += uint32_t(std::popcount(open));
    }
    return free;
}

uint32_t GameSim::entity_generation(uint32_t entity, uint32_t row) const {
    const Impl& m = *impl_;
    const int d = m.dynamic_of.at(entity);
    if (d < 0) return row < m.counts[entity] ? 1 : 0;
    return row < m.dynamics[size_t(d)].generation.size() ? m.dynamics[size_t(d)].generation[row] : 0;
}

uint32_t GameSim::entity_count(uint32_t entity) const { return impl_->counts.at(entity); }

uint32_t GameSim::entities_alive(uint32_t entity) const {
    const Impl& m = *impl_;
    const int d = m.dynamic_of.at(entity);
    return d >= 0 ? m.dynamics[size_t(d)].alive_count : m.counts[entity];
}

GameSim::~GameSim() = default;

void GameSim::tick() {
    begin_tick();
    end_tick();
}

void GameSim::begin_tick() {
    Impl& m = *impl_;
    m.settle();  // one tick in flight at a time
    m.boundary();
    m.runner->tick_submit(m.stages_now());
    m.in_flight = true;
}

bool GameSim::tick_ready() { return !impl_->in_flight || impl_->runner->tick_done(); }

void GameSim::end_tick() { impl_->settle(); }

bool GameSim::tick_in_flight() const { return impl_->in_flight; }

std::vector<OrderIssue> GameSim::tick_checked() {
    Impl& m = *impl_;
    m.settle();
    m.boundary();
    std::vector<bool> check(m.spirv.size(), false);  // not the built-in stages after the rules
    std::fill(check.begin(), check.begin() + std::ptrdiff_t(m.game.stages.size()), true);
    const auto diffs = m.runner->tick_checked(check, m.row_words, m.stages_now());
    m.after_tick(false);  // a checked tick runs every stage three times: no measure of the device
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

uint64_t GameSim::sync_reads() const { return impl_->earlier_sync_reads + impl_->runner->sync_reads(); }

uint64_t GameSim::ticks() const { return impl_->now; }

std::vector<int32_t> GameSim::read_field(uint32_t entity, const std::string& field, uint32_t first_row, uint32_t rows) {
    Impl& m = *impl_;
    m.settle();
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
    m.mirror_write(entity, uint32_t(f), row, words.data(), words.size());
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
    // inherit fields hold one value per row, so a partial write of one is the whole row
    if (!words.empty()) m.mirror_write(entity, uint32_t(f), row, words.data(), 1);
}

uint32_t GameSim::read_each_tick(uint32_t entity, const std::string& field, uint32_t first_row, uint32_t rows) {
    Impl& m = *impl_;
    m.settle();
    const EntityDef& e = m.game.entities.at(entity);
    const int f = e.field_index(field);
    if (f < 0) throw std::invalid_argument("read_each_tick: '" + e.name + "' has no field '" + field + "'");
    if (first_row > e.capacity) throw std::out_of_range("read_each_tick: first row beyond the capacity");
    const FieldDef& fd = e.fields[size_t(f)];
    Impl::TickRead r{0, entity, first_row, rows, uint64_t(fd.size) * fd.words(), m.field_binding[entity][size_t(f)]};
    // room for the most it can ever copy: a range that follows the count may grow up to
    // the capacity (load() restores other counts), but only the rows in use are copied
    const uint64_t most = rows == UINT32_MAX ? e.capacity - first_row : std::min(rows, e.capacity - first_row);
    r.most = most * r.row_words * 4;
    r.offset = uint64_t(first_row) * r.row_words * 4;
    r.readback = m.runner->add_tick_readback(r.binding, r.most, r.offset);
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
    return impl_->mods.add(bundle, entity, row, scale, duration, impl_->host_now());
}
bool GameSim::set_modifier_scale(ModifierHandle h, int32_t scale) { return impl_->mods.set_scale(h, scale); }
bool GameSim::remove_modifier(ModifierHandle h) { return impl_->mods.remove(h); }
bool GameSim::modifier_alive(ModifierHandle h) const { return impl_->mods.alive(h); }
size_t GameSim::modifier_count() const { return impl_->mods.count(); }

int32_t GameSim::modifier_own_total(uint32_t key, uint32_t entity, uint32_t row) const {
    return impl_->mods.own_total(key, entity, row);
}

int32_t GameSim::modifier_total(uint32_t key, uint32_t entity, uint32_t row) {
    uint32_t total = 0;  // wraps like the accessor's int sum
    impl_->for_each_level(key, entity, row,
                          [&](uint32_t e, uint32_t r) { total += uint32_t(impl_->mods.own_total(key, e, r)); });
    return static_cast<int32_t>(total);
}

std::vector<ModifierContribution> GameSim::modifier_breakdown(uint32_t key, uint32_t entity, uint32_t row) {
    std::vector<ModifierContribution> out;
    impl_->for_each_level(key, entity, row,
                          [&](uint32_t e, uint32_t r) { impl_->mods.own_breakdown(key, e, r, out); });
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
    for (const auto& e : game.entities) {  // nothing for a game without them: its checksum stays what it was
        if (!e.dynamic) continue;
        w.str("dynamic"), w.str(e.name), w.u32(static_cast<uint32_t>(e.spawns.size()));
        for (const auto& s : e.spawns) {
            w.str(s.from), w.str(s.count_field), w.str(s.parent_field);
            if (!s.ordinal_field.empty()) w.str("ordinal"), w.str(s.ordinal_field);  // absent when unused
            if (s.every != 1) w.str("schedule"), w.u32(s.every), w.u32(s.offset);    // absent for every tick
        }
    }
    w.u32(static_cast<uint32_t>(game.stages.size()));
    for (const auto& s : game.stages) {
        w.str(s.name), w.str(s.for_each), w.str(s.source), w.u32(static_cast<uint32_t>(s.clear.size()));
        for (const auto& c : s.clear) w.str(c);
        if (s.dispatch == Dispatch::Workgroup) w.str("dispatch=workgroup");  // absent for the default
        if (s.every != 1) w.str("schedule"), w.u32(s.every), w.u32(s.offset);  // absent for every tick
    }
    for (const auto& c : game.clears) {  // nothing for a game without them
        w.str("clear"), w.u32(c.every), w.u32(c.offset), w.u32(static_cast<uint32_t>(c.fields.size()));
        for (const auto& f : c.fields) w.str(f);
    }
    return fnv64(w.buf.data(), w.buf.size());
}

uint64_t GameSim::checksum() const { return game_checksum(impl_->game); }

namespace {

const char* type_name(uint8_t t) {
    static const char* const names[] = {"quantity", "rate", "agg", "int", "count", "ref", "money64"};
    return t < 7 ? names[t] : "Terni's own";
}

// A save as it was read, before it is fitted to a game.
struct SavedState {
    uint32_t version = 0;
    uint64_t checksum = 0, now = 0;
    uint32_t device_tick = 0;
    struct Entity {
        std::string name;
        uint32_t capacity = 0, count = 0;
        bool dynamic = false;
    };
    std::vector<Entity> entities;
    struct Column {
        std::string name;
        uint8_t type = 255;  // a FieldType, or 255 for Terni's own columns
        uint32_t size = 0;   // values per row (fields)
        std::string ref;     // the entity type a ref field points to
        std::vector<int32_t> words;
    };
    std::vector<Column> columns;
    std::vector<std::string> bundles;
    SavedModifiers mods;
    std::vector<uint32_t> state_words;  // format 1 only: the tick and the counts, in the game's entity order
};

// Reads the header and checks the content hash: a damaged save is refused before anything else.
Reader open_save(const std::vector<uint8_t>& data, uint32_t& version, uint64_t& checksum) {
    if (data.size() < 8 + 4 + 8 + 8 || std::memcmp(data.data(), SAVE_MAGIC, 8) != 0)
        throw std::runtime_error("save: not a Terni save");
    const uint64_t stored_hash = Reader{data.data() + data.size() - 8, 8}.u64();
    if (fnv64(data.data(), data.size() - 8) != stored_hash) throw std::runtime_error("save: corrupt (content hash mismatch)");
    Reader r{data.data(), data.size() - 8, 8};
    version = r.u32();
    if (version != 1 && version != SAVE_VERSION)
        throw std::runtime_error("save: format version " + std::to_string(version) + ", expected " + std::to_string(SAVE_VERSION));
    checksum = r.u64();
    return r;
}

// Everything a save holds, without a game: format 1 has no entity types, field types or
// bundles, only its state words and columns by name.
SavedState read_save(const std::vector<uint8_t>& data) {
    SavedState saved;
    Reader r = open_save(data, saved.version, saved.checksum);
    saved.now = r.u64();
    if (saved.version == 1) {
        saved.state_words.resize(r.u32());
        for (auto& v : saved.state_words) v = r.u32();
        saved.columns.resize(r.u32());
        for (auto& col : saved.columns) col.name = r.str(), col.words = r.words();
    } else {
        saved.device_tick = r.u32();
        saved.entities.resize(r.u32());
        for (auto& e : saved.entities) e.name = r.str(), e.capacity = r.u32(), e.dynamic = r.u8() != 0, e.count = r.u32();
        saved.columns.resize(r.u32());
        for (auto& col : saved.columns) col.name = r.str(), col.type = r.u8(), col.size = r.u32(), col.ref = r.str(), col.words = r.words();
        saved.bundles.resize(r.u32());
        for (auto& name : saved.bundles) name = r.str();
    }
    saved.mods.read(r);
    if (r.pos != r.size) throw std::runtime_error("save: trailing data");
    return saved;
}

}  // namespace

StateSnapshot save_snapshot(const std::vector<uint8_t>& data) {
    const SavedState saved = read_save(data);
    StateSnapshot s;
    s.add("terni.ticks", std::vector<uint64_t>{saved.now});
    std::vector<uint32_t> state = saved.state_words;
    if (saved.version != 1) {
        state.push_back(saved.device_tick);
        for (const auto& e : saved.entities) state.push_back(e.count);
    }
    s.add("terni.state", state);
    for (const auto& col : saved.columns) {
        const uint32_t words = col.type == static_cast<uint8_t>(FieldType::Money64) ? 2 : 1;
        s.add(col.name, col.words, col.type == 255 ? 1 : col.size * words);
    }
    // the modifier store, by bundle name where the save has them
    std::vector<int64_t> instances, expiries;
    for (const auto& in : saved.mods.instances)
        for (int64_t v : {int64_t(in.bundle), int64_t(in.entity), int64_t(in.row), int64_t(in.scale), int64_t(in.version),
                          int64_t(in.alive)})
            instances.push_back(v);
    std::vector<SavedModifiers::Due> due = saved.mods.due;
    std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.tick != b.tick ? a.tick < b.tick : a.seq < b.seq; });
    for (const auto& d : due)
        for (int64_t v : {int64_t(d.tick), int64_t(d.seq), int64_t(d.slot), int64_t(d.version)}) expiries.push_back(v);
    s.add("terni.modifiers", instances, 6);
    s.add("terni.modifier_free", std::vector<int64_t>(saved.mods.free.begin(), saved.mods.free.end()));
    s.add("terni.modifier_expiries", expiries, 4);
    return s;
}

uint64_t save_checksum(const std::vector<uint8_t>& data) {
    uint32_t version = 0;
    uint64_t checksum = 0;
    open_save(data, version, checksum);
    return checksum;
}

std::vector<uint8_t> GameSim::save() {
    Impl& m = *impl_;
    m.settle();
    // host writes queued for the next tick are part of the state being saved
    m.runner->flush_queued();
    m.runner->read(1, m.state.data(), m.state.size() * 4);

    Writer w;
    w.buf.insert(w.buf.end(), SAVE_MAGIC, SAVE_MAGIC + 8);
    w.u32(SAVE_VERSION);
    w.u64(game_checksum(m.game));
    w.u64(m.now);
    w.u32(m.state[0]);  // the device's tick counter
    w.u32(static_cast<uint32_t>(m.game.entities.size()));
    for (size_t e = 0; e < m.game.entities.size(); ++e) {
        const EntityDef& def = m.game.entities[e];
        w.str(def.name), w.u32(def.capacity), w.u8(def.dynamic ? 1 : 0), w.u32(m.state[1 + e]);
    }
    w.u32(static_cast<uint32_t>(m.column_words.size()));
    std::vector<int32_t> words;
    for (size_t c = 0; c < m.column_words.size(); ++c) {
        m.read_column(c, words);
        const Impl::ColumnInfo& info = m.column_info[c];
        w.str(m.column_names[c]);
        if (info.field >= 0) {
            const FieldDef& f = m.game.entities[info.entity].fields[size_t(info.field)];
            w.u8(static_cast<uint8_t>(f.type)), w.u32(f.size), w.str(f.ref_entity);
        } else {
            w.u8(255), w.u32(0), w.str("");
        }
        w.words(words);
    }
    w.u32(static_cast<uint32_t>(m.game.bundles.size()));
    for (const auto& bundle : m.game.bundles) w.str(bundle.name);
    m.mods.save(w);
    w.u64(fnv64(w.buf.data(), w.buf.size()));  // content hash: detects corruption
    return std::move(w.buf);
}

std::vector<std::string> GameSim::load(const std::vector<uint8_t>& data, SaveMatch match) {
    Impl& m = *impl_;
    m.settle();  // before the tick count and everything else are replaced
    const Game& g = m.game;
    const uint64_t checksum = game_checksum(g);

    // ---- read everything into temporaries first: a bad save leaves the simulation untouched
    SavedState saved = read_save(data);
    const bool same_game = saved.checksum == checksum;
    if (!same_game && match == SaveMatch::SameGame)
        throw std::runtime_error("save: made with a different game (definitions, defines, rules or mods differ); "
                                 "SaveMatch::ByName loads it into this one by name");
    if (!same_game && saved.version == 1) throw std::runtime_error("save: format version 1 loads only into the game that made it");
    if (saved.version == 1) {  // the same game: its entity types, fields and bundles, in order
        if (saved.state_words.size() != 1 + g.entities.size()) throw std::runtime_error("save: state size mismatch");
        saved.device_tick = saved.state_words[0];
        for (size_t e = 0; e < g.entities.size(); ++e)
            saved.entities.push_back({g.entities[e].name, g.entities[e].capacity, saved.state_words[1 + e], g.entities[e].dynamic});
        if (saved.columns.size() != m.column_words.size()) throw std::runtime_error("save: field count mismatch");
        for (size_t c = 0; c < m.column_words.size(); ++c) {
            SavedState::Column& col = saved.columns[c];
            if (col.name != m.column_names[c]) throw std::runtime_error("save: field " + m.column_names[c] + " missing");
            if (const auto& info = m.column_info[c]; info.field >= 0) {
                const FieldDef& f = g.entities[info.entity].fields[size_t(info.field)];
                col.type = static_cast<uint8_t>(f.type), col.size = f.size, col.ref = f.ref_entity;
            }
        }
        for (const auto& bundle : g.bundles) saved.bundles.push_back(bundle.name);
    }

    // ---- fit it to this game, by name. With the same game nothing may need fitting.
    std::vector<std::string> notes;
    auto note = [&](std::string text) {
        if (same_game) throw std::runtime_error("save: does not match its game: " + text);
        notes.push_back(std::move(text));
    };
    if (!same_game) notes.push_back("the save was made with another version of the game: its rules, defines or "
                                    "definitions differ");
    auto find_entity = [&](const std::string& name) -> const SavedState::Entity* {
        for (const auto& e : saved.entities)
            if (e.name == name) return &e;
        return nullptr;
    };
    std::vector<const SavedState::Entity*> from(g.entities.size());  // per entity type: its saved self, or none
    std::vector<uint32_t> state(1 + g.entities.size());
    state[0] = saved.device_tick;
    for (size_t e = 0; e < g.entities.size(); ++e) {
        const EntityDef& def = g.entities[e];
        from[e] = find_entity(def.name);
        if (!from[e]) {
            note("entity type '" + def.name + "' is new: it starts as the game describes it");
            state[1 + e] = g.tables[e].count;
            continue;
        }
        if (from[e]->dynamic != def.dynamic)
            throw std::runtime_error("save: entity type '" + def.name + "' is " + (def.dynamic ? "" : "not ") +
                                     "dynamic now and was " + (from[e]->dynamic ? "" : "not ") + "in the save");
        if (from[e]->count > def.capacity)
            throw std::runtime_error("save: more '" + def.name + "' entities than its capacity (" +
                                     std::to_string(from[e]->count) + " rows in use, capacity " + std::to_string(def.capacity) + ")");
        if (from[e]->capacity != def.capacity)
            note("entity type '" + def.name + "': capacity " + std::to_string(from[e]->capacity) + ", now " +
                 std::to_string(def.capacity));
        state[1 + e] = from[e]->count;
    }
    for (const auto& e : saved.entities) {
        bool kept = false;
        for (const auto& def : g.entities) kept |= def.name == e.name;
        if (!kept) note("entity type '" + e.name + "' is gone: its rows and their modifiers are dropped");
    }

    std::vector<std::vector<int32_t>> columns(m.column_words.size());
    std::vector<bool> used(saved.columns.size(), false);
    for (size_t c = 0; c < columns.size(); ++c) {
        const Impl::ColumnInfo& info = m.column_info[c];
        const EntityDef& def = g.entities[info.entity];
        const SavedState::Entity* old = from[info.entity];
        const SavedState::Column* col = nullptr;
        if (old)
            for (size_t k = 0; k < saved.columns.size() && !col; ++k)
                if (saved.columns[k].name == m.column_names[c]) col = &saved.columns[k], used[k] = true;
        if (!col) {
            // a new entity type, or a new field: as the game starts it, except a dynamic type's
            // field, whose rows have been reused since the start
            columns[c] = old && info.field >= 0 && def.dynamic ? m.default_column(c) : m.starting_column(c);
            if (old && info.field >= 0)
                note("field '" + m.column_names[c] + "' is new: it starts " +
                     (def.dynamic ? "at its default" : "as the game describes it"));
            continue;
        }
        if (info.field < 0) {
            // Terni's own: bits and generations per row, or what the spawn stages work out
            // anew each tick. What fits is copied; the rest is zero (no row, nothing pending).
            // The scratch columns are the spawn stages' working memory, laid out differently by
            // other Terni versions; only the four that carry state must fit exactly.
            const bool carries_state = info.own == "alive_bits" || info.own == "generation" ||
                                       info.own == "terni_prev" || info.own == "terni_cool";
            if (same_game && carries_state && col->words.size() != m.column_words[c])
                throw std::runtime_error("save: '" + m.column_names[c] + "' has the wrong size");
            columns[c].assign(m.column_words[c], 0);
            std::copy_n(col->words.begin(), std::min(col->words.size(), columns[c].size()), columns[c].begin());
            continue;
        }
        const FieldDef& f = def.fields[size_t(info.field)];
        if (col->type != static_cast<uint8_t>(f.type) || col->ref != f.ref_entity)
            throw std::runtime_error("save: field '" + m.column_names[c] + "' was " + type_name(col->type) +
                                     (col->ref.empty() ? "" : ":" + col->ref) + " and is " +
                                     type_name(static_cast<uint8_t>(f.type)) + (f.ref_entity.empty() ? "" : ":" + f.ref_entity) +
                                     " now; the game must convert it");
        const uint64_t old_row = uint64_t(col->size) * f.words(), new_row = uint64_t(f.size) * f.words();
        if (col->size == 0 || col->words.size() != old_row * old->capacity)
            throw std::runtime_error("save: field '" + m.column_names[c] + "' has the wrong size");
        columns[c] = m.default_column(c);  // values the save has no room for keep their default
        const uint64_t rows = std::min(old->capacity, def.capacity), keep = std::min(old_row, new_row);
        for (uint64_t row = 0; row < rows; ++row)
            std::copy_n(col->words.begin() + std::ptrdiff_t(row * old_row), keep, columns[c].begin() + std::ptrdiff_t(row * new_row));
        if (col->size != f.size)
            note("field '" + m.column_names[c] + "': " + std::to_string(col->size) + " values per row, now " +
                 std::to_string(f.size) + (f.size > col->size ? "; the new ones start at their default" : "; the last are dropped"));
    }
    for (size_t k = 0; k < saved.columns.size(); ++k) {
        const auto& col = saved.columns[k];
        const std::string entity = col.name.substr(0, col.name.find('.'));
        bool type_kept = false;
        for (const auto& def : g.entities) type_kept |= def.name == entity;
        if (!used[k] && col.type != 255 && type_kept) note("field '" + col.name + "' is gone: its values are dropped");
    }

    // modifier instances: their bundle and entity type by name; those that no longer fit are dropped
    SavedModifiers mods = std::move(saved.mods);
    std::vector<int> bundle_of(saved.bundles.size(), -1), entity_of(saved.entities.size(), -1);
    for (size_t b = 0; b < saved.bundles.size(); ++b) bundle_of[b] = g.bundle_index(saved.bundles[b]);
    for (size_t e = 0; e < saved.entities.size(); ++e) entity_of[e] = g.entity_index(saved.entities[e].name);
    std::map<std::string, uint32_t> dropped;  // why -> instances
    for (uint32_t slot = 0; slot < mods.instances.size(); ++slot) {
        auto& in = mods.instances[slot];
        if (!in.alive) continue;
        if (in.bundle >= bundle_of.size() || in.entity >= entity_of.size()) throw std::runtime_error("save: modifier instance out of range");
        std::string why;
        if (bundle_of[in.bundle] < 0) why = "bundle '" + saved.bundles[in.bundle] + "' is gone";
        else if (entity_of[in.entity] < 0) why = "entity type '" + saved.entities[in.entity].name + "' is gone";
        else if (in.row >= g.entities[size_t(entity_of[in.entity])].capacity)
            why = "rows past the capacity of '" + saved.entities[in.entity].name + "'";
        if (why.empty()) {
            in.bundle = uint32_t(bundle_of[in.bundle]), in.entity = uint32_t(entity_of[in.entity]);
            continue;
        }
        in.alive = false;
        ++in.version;  // handles to it are stale, and so is its pending expiry
        mods.free.push_back(slot);
        ++dropped[why];
    }
    for (const auto& [why, n] : dropped) note(std::to_string(n) + " modifier instance" + (n == 1 ? "" : "s") + " dropped: " + why);
    ModifierStore store(g);
    store.restore(mods);

    // ---- commit
    m.now = saved.now;
    m.state = std::move(state);
    m.runner->write(1, m.state.data(), m.state.size() * 4);
    m.counts.assign(m.state.begin() + 1, m.state.end());
    m.size_tick_reads();  // per-tick readbacks follow the restored counts
    for (size_t c = 0; c < columns.size(); ++c)
        m.runner->write(static_cast<uint32_t>(c + 2), columns[c].data(), columns[c].size() * 4);
    for (auto& p : m.parents)
        if (p.active) {
            const auto& col = columns[p.binding - 2];
            std::memcpy(p.rows.data(), col.data(), p.rows.size() * 4);
            p.stale = false;
        }
    for (auto& d : m.dynamics) {  // who is alive is in the save, like every field
        auto words = [&](uint32_t binding, std::vector<uint32_t>& to) {
            to.assign(columns[binding - 2].begin(), columns[binding - 2].end());
        };
        words(d.alive_binding, d.alive);
        words(d.generation_binding(), d.generation);
        words(d.prev_binding(), d.prev);
        words(d.cool_binding(), d.cool);
        d.alive_count = 0;
        for (uint32_t w : d.alive) d.alive_count += uint32_t(std::popcount(w));
        d.free_word = 0;
    }
    m.health.reset();  // other counts, other tick times
    m.mods.restore_from(std::move(store));
    for (size_t c = 0; c < m.mods.columns.size(); ++c) {  // upload recomputed totals now, so reads are consistent
        auto& col = m.mods.columns[c];
        m.runner->write(m.mod_binding0 + uint32_t(c), col.totals.data(), col.totals.size() * 4);
        col.dirty_lo = UINT32_MAX;
        col.dirty_hi = 0;
    }
    m.runner->discard_queued();
    return notes;
}

StateSnapshot GameSim::snapshot() {
    Impl& m = *impl_;
    m.settle();
    m.runner->read(1, m.state.data(), m.state.size() * 4);
    StateSnapshot s;
    s.add("terni.state", m.state);
    std::vector<int32_t> words;
    for (size_t c = 0; c < m.column_words.size(); ++c) {
        m.read_column(c, words);
        const Impl::ColumnInfo& info = m.column_info[c];
        uint32_t per_row = 1;
        if (info.field >= 0) {
            const FieldDef& f = m.game.entities[info.entity].fields[size_t(info.field)];
            per_row = f.size * f.words();
        }
        s.add(m.column_names[c], words, per_row);
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

std::vector<FieldHash> GameSim::field_hashes() {
    Impl& m = *impl_;
    m.settle();
    if (m.hash_pipeline < 0) m.hash_pipeline = int(m.runner->add_extra(m.hash_spirv));
    m.runner->write(m.hash_binding, m.hash_zero.data(), m.hash_zero.size() * 4);
    m.runner->run_extra(uint32_t(m.hash_pipeline), 16384);
    std::vector<uint32_t> sums(m.hash_zero.size());
    m.runner->read(m.hash_binding, sums.data(), sums.size() * 4);
    std::vector<FieldHash> out;
    out.reserve(sums.size() / 2);
    auto field = [&](std::string name, size_t words) {
        const size_t k = out.size();
        out.push_back({std::move(name), hash_finish(sums[2 * k], sums[2 * k + 1], words)});
    };
    field("terni.state", m.state.size());
    for (size_t c = 0; c < m.column_words.size(); ++c) field(m.column_names[c], m.column_words[c]);
    for (const auto& col : m.mods.columns)
        field("mod." + m.game.entities[col.entity].name + "." + m.game.modifier_keys[col.key].name, col.totals.size());
    return out;
}

std::vector<StageTime> GameSim::stage_times() const {
    const Impl& m = *impl_;
    const auto& timings = m.runner->stage_timings();
    std::vector<StageTime> out;
    out.reserve(timings.size());
    for (size_t s = 0; s < timings.size(); ++s) {
        const auto& t = timings[s];
        out.push_back({m.stage_names[s], t.ran, t.last_ms, t.runs, t.total_ms});
    }
    return out;
}
CompileStats GameSim::compile_stats() const { return impl_->stats; }

}  // namespace terni
