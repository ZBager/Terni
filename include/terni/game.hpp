// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
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
//                        constants                        defines = { growth_rate = rate }
//                        modifier keys                    modifier_keys = { pop_growth = { applies_to = planet type = rate inherit = owner } }
//                        modifier settings (optional)     modifiers = { capacity = 65536 }
//   common/*.txt         values of the constants          growth_rate = 0.02
//   common/modifiers/*.txt  bundles                       war_economy = { pop_growth = -0.02 tax_efficiency = 0.15 }
//   history/*.txt        starting entities                planet = { id = earth owner = ROM pop = 8200
//                                                                     add_modifier = { name = good_harvest duration = 12 } }
//   rules/stages.txt     tick order                       planet_growth = { for_each = planet }
//                        one workgroup per entity         province_sums = { for_each = province dispatch = workgroup }
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

enum class FieldType : uint8_t { Quantity, Rate, Agg, Int, Count, Ref, Money64 };

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
    uint32_t words() const { return type == FieldType::Money64 ? 2 : 1; }  // 32-bit words per value
};

struct EntityDef {
    std::string name;
    uint32_t capacity = 0;
    std::vector<FieldDef> fields;
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
    uint32_t every = 1, offset = 0;  // runs in the ticks where terni.tick % every == offset
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
    // Throws std::out_of_range for an unknown entity, zero rows or fewer rows than in use.
    void set_capacity(uint32_t entity, uint32_t rows);
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

    // starting state
    uint32_t create(uint32_t entity, const std::string& id = "", Location where = {});  // new row; returns row
    void set(uint32_t entity, uint32_t row, const std::string& field, int64_t raw, uint32_t index = 0,
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
    std::unique_ptr<Game> game_;
    std::vector<bool> define_given_;  // per define: set_define() was called for it
};

// Load a Paradox-syntax game folder (uses WorldBuilder). Never throws on bad data.
std::unique_ptr<Game> load_game(const std::string& root);

// GLSL generated from the schema (buffers, defines, counts, loops, modifier accessors).
// Locale-independent: the same text whatever global locale the host process has set.
std::string generate_prelude(const Game& game);

// 64-bit hash of everything that defines how a world behaves: entity types and
// fields, defines and their values, modifier keys and bundles, stage order and
// rule sources. Saves only load into a game with the same checksum, and lockstep
// multiplayer peers compare it before playing together. Starting state (history)
// is not included -- that is what saves are for.
uint64_t game_checksum(const Game& game);

// Exact decimal text -> fixed-point raw value of a type (integer math only).
bool parse_value(const std::string& text, FieldType type, int64_t& out);

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
};

// How the simulation's device is doing, from the wall time of its ticks (last_tick_ms()).
// Ticks are judged 32 at a time, by their median, so a stage that runs once a month or a
// single late tick does not count. Host measurements only: nothing here reaches the
// simulation, and peers may well see different values.
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
};

struct CompileStats {
    uint32_t compiled = 0;  // stages compiled with shaderc
    uint32_t cached = 0;    // stages loaded from the SPIR-V cache
    double ms = 0;          // time spent getting SPIR-V
};

// Whether this build can compile rules: true for Terni::core, false for Terni::runtime
// (built without shaderc; it loads every rule from a SPIR-V cache).
bool can_compile_rules();

// Compiles every rule of `game` (with the prelude for its schema; capacities do not
// matter, they are set when a GameSim starts) into the SPIR-V cache in `cache_dir`,
// without a Vulkan device; rules already there are kept. This prepares the cache that a
// cache-only GameSim or a Terni::runtime build loads from, e.g. at build time, to ship
// with a game. The cache key does not depend on the compiler, so the cache works with
// any build of the same Terni version. Throws on a compile error, if the cache cannot be
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
    // (valid until the tick after the next one begins; after load(), stale until the next
    // tick is collected).
    std::span<const int32_t> tick_result(uint32_t id) const;

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
    // leaving the simulation untouched -- if the save is corrupt, truncated, from
    // another format version, or from a game with a different checksum.
    void load(const std::vector<uint8_t>& data);
    uint64_t checksum() const;

    StateSnapshot snapshot();  // all fields, modifier totals and counts, read back from the GPU
    std::string device_name() const;
    std::string report() const;
    CompileStats compile_stats() const;
    double last_tick_ms() const;      // wall time of the last tick, uploads and per-tick readbacks included
                                      // (begun to seen done: by tick_ready() or end_tick())
    double last_tick_gpu_ms() const;  // GPU time of the last tick's stages alone (timestamps)
    uint64_t sync_reads() const;      // reads so far that waited for the GPU (read_field, snapshot, save, ...)

    // ---- device health ----
    DeviceHealth device_health() const;  // tick times lately against the usual; see DeviceHealth
    // Continues on another device (an index from VulkanContext::list_devices() or
    // pick_device()), e.g. when this one has become slow. The state moves bit for bit, so
    // the simulation goes on exactly as it would have: handles, read_each_tick() ids and
    // their last results stay valid, and lockstep peers need not know. Collects a tick in
    // flight first, and takes about as long as starting the simulation (pipelines are
    // made again, the state is copied over). Throws, changing nothing, if the device
    // cannot be used.
    void move_to_device(uint32_t device);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace terni
