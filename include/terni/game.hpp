// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#pragma once
// Terni's public API: describe a world (entities, fields, defines, modifier keys,
// bundles, compute stages, starting state), run it on a Vulkan device, and drive
// it from the host (read/write fields, add/rescale/remove modifiers).
//
// Two ways to describe a world:
//   - in code, with WorldBuilder (for embedding Terni into an existing engine), or
//   - from a Paradox-syntax folder with load_game(), which uses WorldBuilder underneath:
//
//   schema/*.txt         entity types and fields          planet = { capacity = 4096  pop = quantity }
//                        rows that come and go            army = { capacity = 4096  dynamic = yes
//                                                                  spawn = { from = province count = raise parent = home } }
//                        constants                        defines = { growth_rate = rate }
//                        modifier keys                    modifier_keys = { pop_growth = { applies_to = planet type = rate inherit = owner } }
//                        modifier settings (optional)     modifiers = { capacity = 65536 }
//   common/*.txt         values of the constants          growth_rate = 0.02
//   common/modifiers/*.txt  bundles                       war_economy = { pop_growth = -0.02 tax_efficiency = 0.15 }
//   history/*.txt        starting entities                planet = { id = earth owner = ROM pop = 8200
//                                                                     add_modifier = { name = good_harvest duration = 12 } }
//   rules/stages.txt     tick order                       planet_growth = { for_each = planet }
//                        one workgroup per entity         province_sums = { for_each = province dispatch = workgroup }
//                        fields zeroed on a schedule      new_month = { clear = { realm.tax } every = 30 }
//                        only some ticks                  monthly_taxes = { for_each = province every = 30 offset = 29 }
//   rules/<stage>.glsl   body of each stage
//
// Field types (all 32-bit on the GPU):
//   quantity  Q24.8   people, goods, money       rate    Q16.16  ratios, prices
//   agg       int32   whole-unit sums            int     int32
//   count     uint32                             ref:<entity>  uint32 index of another entity
//   money64   int64 value, uvec2 (lo, hi) on the GPU (add_i64)
// `{ type = quantity size = 6 }` gives each entity 6 values (index i * 6 + k).
//
// Generated GLSL:  planet_pop[i]   D.growth_rate   terni.tick   terni.planet_count
//                  FOR_EACH_PLANET(i) { ... }      planet_mod_pop_growth(i)  (own + inherited modifier total)
//                  FOR_EACH_PLANET_GROUP(i) { ... }  in `dispatch = workgroup` stages: 256 threads per
//                  planet (gl_LocalInvocationID.x), shared memory and workgroup_sync() allowed
//
// Modifiers (Paradox-style, additive): a key belongs to one entity type and
// optionally inherits through a ref field (`inherit = owner`: a planet's total =
// its own instances + its owner's), or through several in a row (`inherit =
// { district region realm }`: a province's total = its own + its district's + that
// district's region's + that region's realm's). A bundle is a named set of key values. An
// instance applies a bundle to one entity with a scale, optionally for a number
// of ticks. Host calls take effect for the simulation at the next tick.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <array>
#include <optional>
#include <span>

#include "terni/state_hash.hpp"
#include "terni/vk_context.hpp"

namespace terni {

// The last six are small fields: a `count8` takes one byte per value on the device instead
// of four. Hosts see one 32-bit word per value of them too; rules read and write them with
// <entity>_<field>(i) and <entity>_<field>_set(i, v).
enum class FieldType : uint8_t { Quantity, Rate, Agg, Int, Count, Ref, Money64, Int8, Int16, Count8, Count16, Ref8, Ref16 };

// Bits a value takes on the device: 8 or 16 for small fields, else 32 (64 for money64).
constexpr uint32_t field_bits(FieldType t) {
    switch (t) {
        case FieldType::Int8:
        case FieldType::Count8:
        case FieldType::Ref8: return 8;
        case FieldType::Int16:
        case FieldType::Count16:
        case FieldType::Ref16: return 16;
        case FieldType::Money64: return 64;
        default: return 32;
    }
}
constexpr bool is_small(FieldType t) { return field_bits(t) < 32; }
// What a small field holds, whatever its width: Int, Count or Ref; other types are their own.
constexpr FieldType base_type(FieldType t) {
    switch (t) {
        case FieldType::Int8:
        case FieldType::Int16: return FieldType::Int;
        case FieldType::Count8:
        case FieldType::Count16: return FieldType::Count;
        case FieldType::Ref8:
        case FieldType::Ref16: return FieldType::Ref;
        default: return t;
    }
}
// A ref that refers to no row. Small refs hold it too: their largest value means it, so a
// ref8 refers to rows 0 to 254 and a ref16 to rows 0 to 65534.
constexpr uint32_t NO_ROW = 0xFFFFFFFFu;
// Whether a raw value fits a type as it is. In a small field rules store the nearest value
// that fits instead (a ref: NO_ROW); host calls refuse it.
constexpr bool fits(FieldType t, int64_t raw) {
    const int64_t values = int64_t(1) << (field_bits(t) < 32 ? field_bits(t) : 32);
    switch (base_type(t)) {
        case FieldType::Money64: return true;
        case FieldType::Count: return raw >= 0 && raw < values;
        case FieldType::Ref: return (raw >= 0 && raw < values - 1) || raw == -1 || raw == int64_t(NO_ROW);
        default: return raw >= -(values / 2) && raw < values / 2;
    }
}
// The same for a value as hosts pass it, one 32-bit word (not for money64, which has two).
constexpr bool fits_word(FieldType t, int32_t word) {
    const FieldType base = base_type(t);
    return !is_small(t) || fits(t, base == FieldType::Int ? int64_t(word) : int64_t(static_cast<uint32_t>(word)));
}
// The type as data folders name it ("count8"); a ref without its entity type ("ref16").
const char* type_name(FieldType t);

struct Location {
    std::string file;
    uint32_t line = 0;
};

struct GameDiagnostic {
    Location where;
    std::string message;
    std::string text() const;  // "file:line: message"
};

struct FieldDef {
    std::string name;
    FieldType type = FieldType::Int;
    std::string ref_entity;    // for Ref
    uint32_t size = 1;         // values per entity
    int64_t default_raw = 0;   // in the field's raw format
    Location where;
    uint32_t words() const { return type == FieldType::Money64 ? 2 : 1; }  // 32-bit words per value, for the host
    uint32_t row_bytes() const { return size * field_bits(type) / 8; }     // per row, on the device
};

// Rows of a dynamic entity type that rules ask for: at the end of every tick, each row of
// `from` (in row order) gets as many new rows as its count field says, and the field is
// lowered by the number created (to 0 unless the entity type is full).
struct SpawnDef {
    std::string from;          // entity type whose rows ask
    std::string count_field;   // its `count` field: how many new rows that row wants
    std::string parent_field;  // ref:<from> field of the new rows, set to the row that asked ("" = none)
    // int or count field of the new rows, set to 0, 1, 2, ... for the rows one asking row
    // gets in a tick, in the order they are created ("" = none)
    std::string ordinal_field;
    // The line gives out rows only at the end of ticks it runs in, like a stage's schedule;
    // asking rows keep their count until then.
    uint32_t every = 1, offset = 0;
    int from_entity = -1, count_index = -1, parent_index = -1, ordinal_index = -1;  // resolved by finish()
    Location where;

    bool runs_in(uint64_t tick) const { return tick % every == offset; }  // tick = completed ticks before it
};

// What a spawn line may add to `from` and `count` (WorldBuilder::spawn).
struct SpawnOptions {
    std::string parent_field;   // see SpawnDef
    std::string ordinal_field;  // see SpawnDef
    uint32_t every = 1, offset = 0;
};

struct EntityDef {
    std::string name;
    uint32_t capacity = 0;
    std::vector<FieldDef> fields;
    // Rows are created and destroyed while the simulation runs (see GameSim::create_entity).
    bool dynamic = false;
    std::vector<SpawnDef> spawns;  // dynamic entity types only; handled in this order
    Location where;
    const FieldDef* field(const std::string& n) const;
    int field_index(const std::string& n) const;
};

struct DefineDef {
    std::string name;
    FieldType type = FieldType::Int;
    Location where;
};

// One step of a modifier key's inheritance: a ref field of the entity type one level
// below, and the entity type it refers to.
struct InheritStep {
    uint32_t field = 0;   // index of the ref field in the entity one level below
    uint32_t entity = 0;  // entity type the field refers to
};

struct ModifierKeyDef {
    std::string name;
    uint32_t entity = 0;             // entity type the key applies to
    FieldType type = FieldType::Rate;  // Rate (percent-like), or Quantity or Int (flat)
    // Ref fields to inherit through, nearest first, joined by '.' ("" = none): "owner" is
    // one step; "district.region.realm" follows province.district, then district.region,
    // then region.realm.
    std::string inherit_field;
    std::vector<InheritStep> inherit_steps;  // the same, resolved by finish(); empty = none
    Location where;
};

struct BundleDef {
    std::string name;
    std::vector<std::pair<uint32_t, int32_t>> values;  // (key id, raw value)
    Location where;
};

// How a stage is launched: one thread per entity (default), or one 256-thread
// workgroup per entity for work that is wide per entity (many columns or children).
enum class Dispatch : uint8_t { Thread, Workgroup };

struct StageDef {
    std::string name;
    Dispatch dispatch = Dispatch::Thread;
    std::string for_each;            // entity type the stage iterates
    std::vector<std::string> clear;  // "entity.field" buffers zeroed at the start of each tick the stage runs
    std::string source;              // GLSL body
    std::string source_file;         // where it was read from, for messages ("my_mod/rules/x.glsl"); "" in code
    uint32_t every = 1, offset = 0;  // runs in the ticks where terni.tick % every == offset
    Location where;

    bool runs_in(uint64_t tick) const { return tick % every == offset; }  // tick = completed ticks before it
};

// Fields zeroed at the start of the ticks a schedule names, without a stage.
struct ClearDef {
    std::vector<std::string> fields;  // "entity.field"
    uint32_t every = 1, offset = 0;   // the ticks where terni.tick % every == offset
    Location where;

    bool runs_in(uint64_t tick) const { return tick % every == offset; }  // tick = completed ticks before it
};

// Host-side world: one column (int32 words) per field, capacity rows each.
struct EntityTable {
    const EntityDef* def = nullptr;
    uint32_t count = 0;
    std::vector<std::vector<int32_t>> columns;   // [field][row * size * words + ...]
    std::map<std::string, uint32_t> ids;         // history id -> row
};

// A modifier applied at load time (history `add_modifier = { ... }`).
struct InitialModifier {
    uint32_t bundle = 0, entity = 0, row = 0;
    int32_t scale = 1 << 16;
    uint32_t duration = 0;  // ticks, 0 = permanent
};

// A complete world description plus its starting state. Built by WorldBuilder.
struct Game {
    std::string root;
    std::vector<EntityDef> entities;
    std::vector<DefineDef> defines;
    std::vector<int32_t> define_values;          // raw, in declaration order
    std::vector<ModifierKeyDef> modifier_keys;
    std::vector<BundleDef> bundles;
    uint32_t modifier_capacity = 65536;
    std::vector<InitialModifier> initial_modifiers;
    std::vector<StageDef> stages;
    std::vector<ClearDef> clears;
    std::vector<EntityTable> tables;             // same order as entities
    std::vector<GameDiagnostic> errors, warnings;

    const EntityDef* entity(const std::string& n) const;
    int entity_index(const std::string& n) const;
    int modifier_key_index(const std::string& n) const;
    int bundle_index(const std::string& n) const;
    int define_index(const std::string& n) const;
    // A define's raw value: the word rules read as D.<name> (a rate of 0.02 is 1310).
    // Throws std::invalid_argument for a name the game does not declare; define_index()
    // returns -1 instead, for defines that are optional.
    int32_t define(const std::string& n) const;
    // Room for `rows` rows of an entity type, e.g. for the map size a match picks. New rows
    // get the fields' defaults; rows past `rows` are dropped. Call before creating a
    // GameSim. Capacities are not part of the compiled rules, so every capacity runs on the
    // same SPIR-V (one cache entry). They are part of game_checksum(): peers must agree.
    // Throws std::out_of_range for an unknown entity, zero rows, fewer rows than in use, or
    // more rows than a small ref field to the type can refer to.
    void set_capacity(uint32_t entity, uint32_t rows);
    // Whether it has small fields, which devices without DeviceInfo::small_fields store in 32 bits.
    bool small_fields() const;
    bool ok() const { return errors.empty(); }
};

// ---------------------------------------------------------------------------
// Describing a world in code. Every call validates; problems are collected in
// game().errors with the Location passed in (empty for code-built worlds).
// ---------------------------------------------------------------------------
class WorldBuilder {
public:
    WorldBuilder();
    explicit WorldBuilder(std::string root);

    uint32_t entity(const std::string& name, uint32_t capacity, Location where = {});
    void field(uint32_t entity, const std::string& name, FieldType type, const std::string& ref_entity = "",
               uint32_t size = 1, int64_t default_raw = 0, Location where = {});
    // Rows of `entity` come and go during play: rules get <entity>_alive(i),
    // <entity>_destroy(i) and <entity>_generation[i], loops skip rows that are not alive,
    // and the host gets GameSim::create_entity() / destroy_entity(). The capacity must be
    // under 16,777,216.
    void entity_dynamic(uint32_t entity, Location where = {});
    // Rules ask for new rows of the dynamic `entity`: each row of `from_entity` sets its
    // `count_field` (type count) to the number it wants. The new rows exist from the next
    // tick, with every field at its default and `parent_field` (a ref:<from_entity> field
    // of `entity`, optional) set to the row that asked.
    void spawn(uint32_t entity, const std::string& from_entity, const std::string& count_field,
               const std::string& parent_field = "", Location where = {});
    // The same with an ordinal field and a schedule: `every = 30, offset = 29` gives rows
    // out only at the end of the ticks a stage with that schedule runs in.
    void spawn(uint32_t entity, const std::string& from_entity, const std::string& count_field,
               const SpawnOptions& options, Location where = {});
    void define(const std::string& name, FieldType type, Location where = {});
    void set_define(const std::string& name, int64_t raw, Location where = {});
    // `inherit_field`: a ref field of `entity` to inherit through, or several joined by
    // '.', nearest first ("district.region.realm"); every entity type on the way gets
    // the key, and each may appear once.
    uint32_t modifier_key(const std::string& name, uint32_t entity, FieldType type,
                          const std::string& inherit_field = "", Location where = {});
    uint32_t bundle(const std::string& name, const std::vector<std::pair<uint32_t, int32_t>>& values, Location where = {});
    void modifier_capacity(uint32_t capacity);
    void stage(const std::string& name, const std::string& for_each, const std::string& glsl,
               const std::vector<std::string>& clear = {}, Location where = {});
    void stage_dispatch(const std::string& stage, Dispatch dispatch, Location where = {});
    // Run a stage only in the ticks where terni.tick % every == offset (offset < every),
    // e.g. every = 30 for a monthly stage. Skipped ticks do not dispatch it at all.
    void stage_schedule(const std::string& stage, uint32_t every, uint32_t offset = 0, Location where = {});

    // Zero fields ("entity.field") at the start of the ticks where terni.tick % every ==
    // offset, before the first stage, as a stage's `clear` does but without a stage.
    void clear(const std::vector<std::string>& fields, uint32_t every = 1, uint32_t offset = 0, Location where = {});

    // starting state
    uint32_t create(uint32_t entity, const std::string& id = "", Location where = {});  // new row; returns row
    void set(uint32_t entity, uint32_t row, const std::string& field, int64_t raw, uint32_t index = 0,
             Location where = {});
    // The same in bulk. create_rows() adds `count` rows without ids and returns the first
    // (none, with an error, if they do not all fit); set_rows() sets whole rows of a field
    // from `first_row` on to `words`, raw as in GameSim::write_field() (`size` values per
    // row, two words per money64 value, low word first).
    uint32_t create_rows(uint32_t entity, uint32_t count, Location where = {});
    void set_rows(uint32_t entity, uint32_t first_row, const std::string& field, std::span<const int32_t> words,
                  Location where = {});
    void add_modifier(uint32_t bundle, uint32_t entity, uint32_t row, int32_t scale = 1 << 16, uint32_t duration = 0);

    Game& game() { return *game_; }
    // Final validation; check result->ok(). result->warnings names defines that were never
    // given a value (they are 0), and stages whose GLSL writes a field and reads it at a
    // row other threads of the stage may write (or writes such rows without an atomic):
    // their result can depend on thread order. A static look at the text;
    // GameSim::tick_checked() confirms at run time. A line with the comment
    // `terni: order ok` is left out.
    std::unique_ptr<Game> finish();

private:
    void error(const Location& where, std::string message);
    std::vector<std::string> known_fields(const std::vector<std::string>& clear, const Location& where);
    std::unique_ptr<Game> game_;
    std::vector<bool> define_given_;  // per define: set_define() was called for it
};

// Load a Paradox-syntax game folder (uses WorldBuilder). Never throws on bad data.
std::unique_ptr<Game> load_game(const std::string& root);
// The game's folder with mods on top, in order: in each subfolder, a file with the same
// name in a later folder replaces the earlier one, and new files add to the game (rule
// sources too, by stage name). Diagnostics name the folder a file came from
// ("my_mod/rules/growth.glsl:7: ...").
std::unique_ptr<Game> load_game(const std::vector<std::string>& roots);
// load_game({"game", "mod"}): without it, two string literals would also fit std::string's
// iterator-pair constructor, and the call would be ambiguous.
std::unique_ptr<Game> load_game(std::initializer_list<std::string> roots);

// GLSL generated from the schema (buffers, defines, counts, loops, modifier accessors).
// Locale-independent: the same text whatever global locale the host process has set.
// `widened`: as devices run it that store small fields in 32 bits (DeviceInfo::small_fields).
std::string generate_prelude(const Game& game, bool widened = false);

// 64-bit hash of everything that defines how a world behaves: entity types and
// fields, defines and their values, modifier keys and bundles, stage order and
// rule sources. Saves load as they are only into a game with the same checksum
// (SaveMatch::ByName fits them to another), and lockstep multiplayer peers compare it
// before playing together. Starting state (history) is not included -- that is what
// saves are for.
uint64_t game_checksum(const Game& game);

// The checksum of the game a save was made with, to tell before loading whether it
// matches. Throws std::runtime_error if the data is not an intact Terni save.
uint64_t save_checksum(const std::vector<uint8_t>& data);

// What a save holds, as a snapshot, read on the host without a game or a device: the
// completed ticks ("terni.ticks"), "terni.state", every column by name (with its values per
// row), and the modifier store ("terni.modifiers": bundle, entity, row, scale, version,
// alive per slot; "terni.modifier_free"; "terni.modifier_expiries"). differences() between
// two of them names the fields and rows where two peers' saves of the same tick differ.
// Throws std::runtime_error if the data is not an intact Terni save.
StateSnapshot save_snapshot(const std::vector<uint8_t>& data);

// How GameSim::load() treats a save made with another version of the game.
enum class SaveMatch {
    SameGame,  // refuse it: the save must come from a game with the same checksum
    ByName,    // fit it to this game by name (a patched game, or another set of mods)
};

// Exact decimal text -> fixed-point raw value of a type (integer math only).
bool parse_value(const std::string& text, FieldType type, int64_t& out);

constexpr uint32_t NO_ENTITY = 0xFFFFFFFFu;  // GameSim::create_entity(): the entity type is full

// A modifier instance. `version` makes stale handles harmless: once the instance is
// removed (or expires) and its slot reused, old handles no longer match.
struct ModifierHandle {
    uint32_t slot = 0xFFFFFFFFu;
    uint32_t version = 0;
    bool valid() const { return slot != 0xFFFFFFFFu; }
};

struct ModifierContribution {
    ModifierHandle handle;
    uint32_t bundle = 0;
    uint32_t entity = 0, row = 0;  // where the instance sits (the target itself or the entity inherited from)
    int32_t value = 0;             // contribution to the key (raw, already scaled)
};

// A stage whose result depends on the order its entities are handled in, found by
// GameSim::tick_checked(): its threads read or write rows that other threads of the same
// stage write, so devices (or runs) can disagree and lockstep peers drift apart.
struct OrderIssue {
    std::string stage;      // stage name
    std::string field;      // "entity.field" that came out different ("mod.entity.key", "terni.state")
    bool parallel = false;  // false: forwards and backwards differ; true: the normal parallel run
                            // differs from the forwards one (e.g. lost updates of a non-atomic +=)
    uint64_t first_row = 0, rows = 0;  // the first differing row, and how many rows differ
    // No order problem, but values that did not fit a small field: the stage stored the
    // nearest ones instead (a ref: NO_ROW). `first_row` is the first such row and `rows` the
    // number of such writes.
    bool misfit = false;
};

// How the simulation's device is doing, from how long its ticks take. Ticks are timed on
// the device's own clock, from submission until their results are back
// (ComputeRunner::last_tick_device_ms()), so a host that polls a background tick rarely is
// judged by the device, not by how often it looked. Ticks are judged 32 at a time, by
// their median, so a stage that runs once a month or a single late tick does not count.
// Host measurements only: nothing here reaches the simulation, and peers may well see
// different values.
struct DeviceHealth {
    double usual_ms = 0;   // a tick when the device was at its best: the lowest median so far
    double recent_ms = 0;  // median of the last 32 ticks measured (0 until there are 32)
    uint64_t ticks = 0;    // ticks measured since the start, load() or move_to_device()
    // The last 32 ticks took over 4 times the usual and at least 1 ms longer: something
    // else is keeping the device busy. A host can warn the player, or move_to_device().
    bool slow = false;
};

struct GameSimOptions {
    int device = -1;                                      // Vulkan device index; -1 = choose (below)
    DevicePreference preference = DevicePreference::Discrete;
    std::optional<std::array<uint8_t, 16>> device_uuid;   // share the host renderer's GPU (matched first)
    std::optional<std::array<uint8_t, 8>> device_luid;    // the same by adapter LUID, for a DirectX
                                                          // renderer (matched after device_uuid)
    std::string spirv_cache_dir;                          // "" = no cache; else compiled rules are stored
                                                          // there by content hash and reused next start
    bool cache_only = false;                              // never compile: every rule must already be in
                                                          // spirv_cache_dir (see compile_rules()), and a
                                                          // missing one is an error naming it. For processes
                                                          // where the compiler must not run (its libraries
                                                          // clash with the host's own). Always the case with
                                                          // Terni::runtime, which has no compiler.
    std::string shader_library_dir;                       // "" = the GLSL library embedded in Terni;
                                                          // else a folder containing terni/*.glsl
    bool validation = false;                              // Vulkan validation layers
    uint32_t spin_us = 0;                                 // waits for a tick (tick(), end_tick(), and calls that
                                                          // collect one) spin for up to this many microseconds
                                                          // before they sleep: saves the 10 us or so a sleeping
                                                          // thread takes to wake, worth it only for ticks well
                                                          // under a millisecond, and keeps a CPU core busy
                                                          // meanwhile. No effect on software devices.
    bool widen_small_fields = false;                      // store small fields in 32 bits, as on a device
                                                          // without DeviceInfo::small_fields: for tests, the
                                                          // results are the same
};

struct CompileStats {
    uint32_t compiled = 0;  // stages compiled with shaderc
    uint32_t cached = 0;    // stages loaded from the SPIR-V cache
    double ms = 0;          // time spent getting SPIR-V
};

// GPU time of one stage, from GameSim::stage_times(). Stages are named as in the game;
// the built-in ones that Terni adds after them start with `terni_`.
struct StageTime {
    std::string name;
    bool ran = false;    // in the last tick (a stage with a schedule skips some)
    double last_ms = 0;  // GPU time in the last tick; 0 if it did not run
    uint64_t runs = 0;   // ticks it ran in, since the start or the last move_to_device()
    double total_ms = 0; // GPU time over those ticks: total_ms / runs is its average
};

// Whether this build can compile rules: true for Terni::core, false for Terni::runtime
// (built without shaderc; it loads every rule from a SPIR-V cache).
bool can_compile_rules();

// Compiles every rule of `game` (with the prelude for its schema; capacities do not
// matter, they are set when a GameSim starts) into the SPIR-V cache in `cache_dir`,
// without a Vulkan device; rules already there are kept. This prepares the cache that a
// cache-only GameSim or a Terni::runtime build loads from, e.g. at build time, to ship
// with a game. The cache key does not depend on the compiler, so the cache works with
// any build of the same Terni version. A game with small fields gets every rule twice: for
// devices that store them packed and for those that widen them. Throws on a compile error, if the cache cannot be
// written, or in a build without the compiler.
CompileStats compile_rules(const Game& game, const std::string& cache_dir, const std::string& shader_library_dir = "");

// Runs a loaded game on one Vulkan device. `game` must outlive the GameSim.
class GameSim {
public:
    explicit GameSim(const Game& game, const GameSimOptions& options = {});
    GameSim(const Game& game, uint32_t device, const std::string& shader_library_dir = "", bool validation = false);
    ~GameSim();

    void tick();             // begin_tick() then end_tick()
    uint64_t ticks() const;  // completed ticks (a tick in flight counts once end_tick() collects it)

    // ---- a tick that does not block ----
    // begin_tick() submits the next tick and returns at once; the host keeps rendering and
    // collects it with end_tick(), e.g. a frame later. In between, writes, modifier changes
    // and commands are for the tick after it, exactly as if made after end_tick(), so the
    // result never depends on when a host collects. tick_result() still gives the last
    // collected tick's results. read_field(), read_each_tick(), save(), load(), snapshot(),
    // tick_checked(), move_to_device() and a first modifier_total()/modifier_breakdown() of an inherited key
    // collect the tick first (they block). One tick is in flight at a time: begin_tick()
    // collects the previous one first.
    void begin_tick();
    bool tick_ready();            // the tick in flight is done (or none is); never blocks
    void end_tick();              // blocks until the tick is done; nothing to do if none is in flight
    bool tick_in_flight() const;  // begun and not yet collected

    // ---- determinism check (tests) ----
    // A tick like tick(), with the same result, in which every stage first runs twice more
    // from the same state, its entities handled one at a time (FOR_EACH / FOR_EACH_GROUP),
    // forwards and then backwards. Fields whose values depend on that order, or on running
    // in parallel, are returned: the stage reads or writes rows that other threads of the
    // same stage write. Rules need no changes. Much slower than tick() (every stage runs
    // three times and the whole state is read back after each run): for tests, on small worlds.
    std::vector<OrderIssue> tick_checked();

    // ---- host access to state (Terni owns it; writes take effect at the next tick) ----
    std::vector<int32_t> read_field(uint32_t entity, const std::string& field, uint32_t first_row = 0,
                                    uint32_t rows = 0xFFFFFFFFu);  // raw words
    void write_field(uint32_t entity, const std::string& field, uint32_t row, const std::vector<int32_t>& words);
    // Same, for any number of consecutive rows starting at `row`; the words are copied
    // into the GPU staging buffer immediately and uploaded with the next tick.
    void write_field(uint32_t entity, const std::string& field, uint32_t row, std::span<const int32_t> words);
    // Part of one row of a sized field: values [index, index + n) of `row`, where n is the
    // number of values in `words` (two words each for money64). The rest of the row keeps
    // whatever the GPU has, so hosts need not mirror whole rows to change one value.
    // Writes queued for the same tick apply in the order they were made.
    void write_field(uint32_t entity, const std::string& field, uint32_t row, uint32_t index,
                     const std::vector<int32_t>& words);
    void write_field(uint32_t entity, const std::string& field, uint32_t row, uint32_t index,
                     std::span<const int32_t> words);

    // ---- per-tick results without extra GPU round trips ----
    // Register a field to be copied back at the end of every tick, in the same GPU
    // submission as the tick itself. Returns an id. By default the rows in use,
    // [first_row, count): the copy follows the entity's count (the starting one, or the
    // one restored by load()), so a world that fills a small part of its capacity copies
    // only that part. With `rows` given, exactly [first_row, first_row + rows), clipped
    // to the capacity.
    uint32_t read_each_tick(uint32_t entity, const std::string& field, uint32_t first_row = 0,
                            uint32_t rows = 0xFFFFFFFFu);
    // Words of a registered field as of the last collected tick, for the rows it covers
    // (valid until the tick after the next one begins or move_to_device(); after load(),
    // stale until the next tick is collected).
    std::span<const int32_t> tick_result(uint32_t id) const;
    // The same as bytes: a small field's values packed, 1 or 2 bytes each, little-endian (no
    // copy, unless the device widens them); other fields as tick_result(). Valid as long.
    // A small ref's largest value means NO_ROW.
    std::span<const uint8_t> tick_result_bytes(uint32_t id) const;

    // ---- entities at run time (entity types marked dynamic) ----
    // A new row: the lowest free one, with every field at its default, alive from the next
    // tick. NO_ENTITY if no row is free. A row is free if it is not alive, was not destroyed
    // by a rule in the last tick and not by the host since: a destroyed row rests for a tick
    // before it is used again. Rows that rules asked for (WorldBuilder::spawn) are created
    // on the device at the end of their tick, before any call made here, so the same calls
    // give the same rows on every peer. create_entity() and destroy_entity() collect a
    // tick in flight first (they block); they throw std::invalid_argument for an entity
    // type that is not dynamic.
    uint32_t create_entity(uint32_t entity);
    // The row is no longer alive: loops skip it from the next tick, its modifier instances
    // are removed, and it can be used again after that tick. False if it was not alive.
    bool destroy_entity(uint32_t entity, uint32_t row);
    // As of the last collected tick, plus creates and destroys since. Never block. For an
    // entity type that is not dynamic, the rows in use are alive.
    bool entity_alive(uint32_t entity, uint32_t row) const;
    // How many times the row has been created (0: never). With the row it names one
    // entity for good: keep both to tell a row that was destroyed and reused.
    uint32_t entity_generation(uint32_t entity, uint32_t row) const;
    uint32_t entity_count(uint32_t entity) const;    // rows in use: one past the highest row ever alive
    uint32_t entities_alive(uint32_t entity) const;  // rows alive among them
    // Of a dynamic entity type: one bit per row (row r is bit r % 32 of word r / 32), as
    // entity_alive() sees them. Valid until the next call that creates, destroys, ticks or
    // loads. Never blocks. Throws for a type that is not dynamic.
    std::span<const uint32_t> alive_bits(uint32_t entity) const;
    // Of a dynamic entity type: how many rows create_entity() would find now, so that a
    // command can be refused before it is applied. Collects a tick in flight first, as
    // create_entity() does. Throws for a type that is not dynamic.
    uint32_t entities_free(uint32_t entity);

    // ---- modifiers ----
    ModifierHandle add_modifier(uint32_t bundle, uint32_t entity, uint32_t row, int32_t scale = 1 << 16,
                                uint32_t duration = 0);
    bool set_modifier_scale(ModifierHandle h, int32_t scale);  // false if the handle is stale
    bool remove_modifier(ModifierHandle h);                    // false if the handle is stale
    bool modifier_alive(ModifierHandle h) const;
    // Own total of `key` on (entity, row) -- without inheritance.
    int32_t modifier_own_total(uint32_t key, uint32_t entity, uint32_t row) const;
    // Own + inherited total, and the list of contributing instances (for tooltips), nearest
    // first. `entity` is the key's own entity type or one it inherits through; from there
    // the total goes up the rest of the chain. The inherited part follows the ref fields
    // (e.g. owner) as of the last tick plus host writes since. No GPU round trip: the first
    // query that needs an inherit field reads it once, after that every tick brings back
    // its rows in use in the same submission.
    int32_t modifier_total(uint32_t key, uint32_t entity, uint32_t row);
    std::vector<ModifierContribution> modifier_breakdown(uint32_t key, uint32_t entity, uint32_t row);
    size_t modifier_count() const;

    // ---- save / load (between ticks) ----
    // The complete simulation state: tick, entity counts, every field, and the
    // modifier store (instances, free slots, pending expiries, sequence counter).
    // Binary, little-endian, versioned, with the game checksum and a content hash.
    std::vector<uint8_t> save();
    // Restores a save into this simulation (any device). Throws std::runtime_error --
    // leaving the simulation untouched -- if the save is corrupt, truncated or from an
    // unknown format version, or, with SaveMatch::SameGame, from a game with a different
    // checksum. With SaveMatch::ByName such a save is fitted to this game: entity types,
    // fields and bundles are matched by name; new fields and entity types start as the
    // game describes them (a dynamic type's new field at its default); removed ones and
    // the modifier instances of removed bundles are dropped; a field may change its
    // values per row and an entity type its capacity (down to the rows in use). A field
    // that changed its type, or an entity type that became dynamic or stopped being so,
    // is refused. Returns one line per thing fitted; empty for a save of this very game.
    std::vector<std::string> load(const std::vector<uint8_t>& data, SaveMatch match = SaveMatch::SameGame);
    uint64_t checksum() const;

    StateSnapshot snapshot();  // all fields, modifier totals and counts, read back from the GPU
    std::string device_name() const;
    std::string report() const;
    CompileStats compile_stats() const;
    double last_tick_ms() const;      // wall time of the last tick, uploads and per-tick readbacks included
                                      // (begun to seen done: by tick_ready() or end_tick())
    double last_tick_gpu_ms() const;  // GPU time of the last tick's stages alone (timestamps), with their clears
                                      // (counted to the first stage) but without uploads and readbacks
    // One hash per field, as hash_fields() of snapshot() would name them, but computed on the
    // device: only two words per field come back, so it costs little enough to compare
    // every few ticks in a lockstep game. The same values as field_hashes(snapshot()) on
    // the host (state_hash.hpp). entity_hashes() groups them per entity type.
    std::vector<FieldHash> field_hashes();
    // Per stage, in the order they run: GPU time in the last tick and since the start.
    // Times are 0 where the device has no timestamps. tick_checked() is not measured.
    // Never blocks: a tick in flight counts once it is collected.
    std::vector<StageTime> stage_times() const;
    uint64_t sync_reads() const;      // reads so far that waited for the GPU (read_field, snapshot, save, ...)

    // ---- device health ----
    DeviceHealth device_health() const;  // tick times lately against the usual; see DeviceHealth
    // Continues on another device (an index from VulkanContext::list_devices() or
    // pick_device()), e.g. when this one has become slow. The state moves bit for bit, so
    // the simulation goes on exactly as it would have: handles, read_each_tick() ids and
    // their last results stay valid, and lockstep peers need not know. Collects a tick in
    // flight first, and takes about as long as starting the simulation (pipelines are
    // made again, the state is copied over; a device that stores small fields otherwise needs
    // other SPIR-V, compiled or from the cache). Throws, changing nothing, if the device
    // cannot be used.
    void move_to_device(uint32_t device);
    // The same, with another GameSimOptions::widen_small_fields from here on (any device, this
    // one too): for tests.
    void move_to_device(uint32_t device, bool widen_small_fields);
    // Whether small fields are stored in 32 bits here: the device has no DeviceInfo::small_fields
    // (or GameSimOptions::widen_small_fields asked for it).
    bool widened() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terni
