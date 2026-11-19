# API reference

Everything is in namespace `terni`. Include `terni/game.hpp` for the world description
and the simulation; the other headers are smaller utilities. Raw values are the
integer form of a field's type (a quantity of 1.5 is `384`, a rate of 1.5 is
`98304`); `terni/fixed.hpp` converts. Strings are UTF-8, paths included, on every
platform.

- [World description](#world-description): `WorldBuilder`, `load_game`, `Game`
- [Simulation](#simulation): `GameSim`, `GameSimOptions`
- [Rules and the SPIR-V cache](#rules-and-the-spir-v-cache)
- [Devices](#devices)
- [Fixed point and randomness](#fixed-point-and-randomness)
- [State hashes](#state-hashes)
- [Script parser](#script-parser)
- [Lower level: ComputeRunner](#lower-level-computerunner)
- [Version](#version)

For C, C# and other languages, the same simulation is behind plain C functions in a
shared library: see [The C API](c-api.md).

## World description

### `WorldBuilder`

Describes a world in code. Every call validates; problems are collected in
`game().errors` with the `Location` passed in.

| Call | Does |
| --- | --- |
| `uint32_t entity(name, capacity)` | Adds an entity type; returns its index |
| `void field(entity, name, type, ref_entity = "", size = 1, default_raw = 0)` | Adds a field |
| `void entity_dynamic(entity)` | Rows of the entity type are created and destroyed during play |
| `void spawn(entity, from_entity, count_field, parent_field = "")` | Rules ask for new rows of the dynamic `entity` through a `Count` field of `from_entity`; `parent_field` of the new row is set to the row that asked |
| `void spawn(entity, from_entity, count_field, const SpawnOptions&)` | The same with `SpawnOptions{parent_field, ordinal_field, every, offset}`: the rows one asking row gets in a tick are numbered in `ordinal_field` (0, 1, ...), and the line gives rows out only in ticks where `tick % every == offset` |
| `void define(name, type)` / `set_define(name, raw)` | Declares a constant / sets its value |
| `uint32_t modifier_key(name, entity, type, inherit_field = "")` | Adds a modifier key (`Rate`, `Quantity` or `Int`). `inherit_field` is a ref field of `entity`, or several joined by `.`, nearest first: `"district.region.realm"` |
| `uint32_t bundle(name, {{key, raw}, ...})` | Adds a modifier bundle |
| `void modifier_capacity(n)` | Most modifier instances alive at once (default 65,536) |
| `void stage(name, for_each, glsl, clear = {})` | Adds a stage at the end of the tick |
| `void stage_dispatch(stage, Dispatch::Workgroup)` | One 256-thread workgroup per row |
| `void stage_schedule(stage, every, offset = 0)` | Run the stage only in the ticks where `terni.tick % every == offset` |
| `uint32_t create(entity, id = "")` | Adds a starting row; returns the row |
| `void set(entity, row, field, raw, index = 0)` | Sets a starting value (`index` within a sized field) |
| `void add_modifier(bundle, entity, row, scale = fx::ONE, duration = 0)` | A modifier at the start |
| `std::unique_ptr<Game> finish()` | Final validation, plus warnings (defines never given a value, order-dependent stages); check `ok()` |

### `load_game`

```cpp
std::unique_ptr<Game> load_game(const std::string& root);
std::unique_ptr<Game> load_game(const std::vector<std::string>& roots);   // or load_game({"game", "mod", ...})
```

Loads a game folder (see [Data format](data-format.md)) through `WorldBuilder`. Never
throws on bad data. With several folders, the first is the game and the others are mods
on top of it: a file with the same path in a later folder replaces the earlier one
([Mods](data-format.md#mods)).

### `Game`

The finished description. Usually only read, but public so that hosts which generate
their world can fill the starting state before creating a `GameSim`.

| Member | Holds |
| --- | --- |
| `entities`, `defines`, `define_values`, `modifier_keys`, `bundles`, `stages` | The description |
| `tables` | Per entity: `count` (rows in use), `columns` (raw words per field), `ids` |
| `initial_modifiers`, `modifier_capacity` | Modifiers at the start |
| `errors`, `warnings` | `GameDiagnostic`s; `text()` gives `file:line: message` |
| `ok()` | No errors |
| `entity_index(name)`, `modifier_key_index(name)`, `bundle_index(name)`, `define_index(name)` | Index by name, or -1 |
| `void set_capacity(entity, rows)` | Room for `rows` rows, before a `GameSim` starts; new rows get defaults. The rules need no recompiling |
| `int32_t define(name)` | A define's raw value, as rules read it (`D.name`); throws `std::invalid_argument` for an unknown name |

### Functions

| Call | Does |
| --- | --- |
| `uint64_t game_checksum(const Game&)` | Hash of everything that defines behaviour (not the starting state) |
| `uint64_t save_checksum(bytes)` | The checksum of the game a save was made with; throws if the data is not an intact save |
| `std::string generate_prelude(const Game&)` | The GLSL Terni puts in front of every rule |
| `bool parse_value(text, type, int64_t& raw)` | Exact decimal text to a raw value |

## Simulation

### `GameSimOptions`

| Member | Default | Meaning |
| --- | --- | --- |
| `device` | -1 | A device index; -1 = choose by `preference` |
| `preference` | `Discrete` | `Discrete`, `Integrated` or `Cpu` |
| `device_uuid` | none | Run on the GPU with this UUID if usable (e.g. the renderer's) |
| `device_luid` | none | Run on the GPU with this adapter LUID if usable (a DirectX renderer's; Windows) |
| `spirv_cache_dir` | "" | Where compiled rules are kept and reused; "" = no cache |
| `cache_only` | false | Never compile; every rule must be in the cache |
| `shader_library_dir` | "" | A folder with `terni/*.glsl` instead of the embedded library |
| `validation` | false | Vulkan validation layers |

### `GameSim`

Runs a `Game` on one device. The `Game` must outlive it. Not thread-safe.

| Call | Does |
| --- | --- |
| `GameSim(const Game&, const GameSimOptions& = {})` | Compiles or loads the rules, uploads the state. Throws `std::runtime_error` on failure |
| `GameSim(const Game&, device, shader_library_dir = "", validation = false)` | The same with a device index and no cache |
| `void tick()` | One tick: uploads, every stage, per-tick readbacks; blocks until done |
| `void begin_tick()` / `bool tick_ready()` / `void end_tick()` | The same tick without blocking: submit, poll, collect ([Embedding](embedding.md#a-tick-that-does-not-block)) |
| `bool tick_in_flight()` | Begun and not yet collected |
| `uint64_t ticks()` | Completed ticks |
| `std::vector<OrderIssue> tick_checked()` | A tick that also checks every stage for order dependence ([Determinism](determinism.md)) |
| `std::vector<int32_t> read_field(entity, field, first_row = 0, rows = all)` | Raw words, read from the GPU now |
| `void write_field(entity, field, row, words)` | Raw words from `row` on (vector or span); applied before the next tick |
| `void write_field(entity, field, row, index, words)` | Values `index` onwards of one row of a sized field; the rest of the row is untouched |
| `uint32_t read_each_tick(entity, field, first_row = 0, rows = in use)` | Registers a per-tick readback; returns an id |
| `std::span<const int32_t> tick_result(id)` | Its words as of the last collected tick; valid until the tick after the next one begins |
| `uint32_t create_entity(entity)` | A new row of a dynamic entity type: the lowest one not alive, fields at their defaults, alive from the next tick. `NO_ENTITY` if full. Collects a tick in flight |
| `bool destroy_entity(entity, row)` | The row is no longer alive and its modifier instances are removed; it rests through the next tick. False if it was not alive. Collects a tick in flight |
| `bool entity_alive(entity, row)`, `uint32_t entity_generation(entity, row)` | As of the last collected tick plus creates and destroys since; never block |
| `uint32_t entity_count(entity)`, `uint32_t entities_alive(entity)` | Rows in use (one past the highest row ever alive); rows alive |
| `std::span<const uint32_t> alive_bits(entity)` | A dynamic entity type's alive bits, one per row (row `r` is bit `r % 32` of word `r / 32`), as `entity_alive` sees them; valid until the next create, destroy, tick or load. Never blocks |
| `uint32_t entities_free(entity)` | Rows `create_entity` would find now: not alive and not resting. Collects a tick in flight |
| `ModifierHandle add_modifier(bundle, entity, row, scale = fx::ONE, duration = 0)` | Adds an instance; an invalid handle if the store is full |
| `bool set_modifier_scale(handle, scale)`, `bool remove_modifier(handle)` | False if the handle is stale |
| `bool modifier_alive(handle)`, `size_t modifier_count()` | |
| `int32_t modifier_own_total(key, entity, row)` | Without inheritance |
| `int32_t modifier_total(key, entity, row)` | Own plus inherited, from host memory. `entity` is the key's entity type or one it inherits through; the total counts from there up |
| `std::vector<ModifierContribution> modifier_breakdown(key, entity, row)` | Every contributing instance, for tooltips, nearest level first |
| `std::vector<uint8_t> save()` | The complete state |
| `std::vector<std::string> load(bytes, match = SaveMatch::SameGame)` | Restores a save; throws on a bad save and changes nothing. `SaveMatch::ByName` fits a save of another version of the game by name and returns one line per thing fitted ([Embedding](embedding.md#saves-from-an-earlier-version-of-your-game)) |
| `uint64_t checksum()` | `game_checksum` of its game |
| `StateSnapshot snapshot()` | Every field and modifier total, read back now |
| `std::string device_name()`, `std::string report()` | The device; upload time, memory, GPU time per stage |
| `CompileStats compile_stats()` | Stages compiled vs loaded from the cache, time spent |
| `double last_tick_ms()`, `double last_tick_gpu_ms()` | Wall time incl. transfers; GPU time of the stages |
| `std::vector<FieldHash> field_hashes()` | One hash per field, computed on the device: the same as `field_hashes(snapshot())`, at a small part of its cost. Collects a tick in flight |
| `std::vector<StageTime> stage_times()` | Per stage, in the order they run (the rules', then Terni's own, named `terni_...`): GPU time in the last tick and since the start. Never blocks |
| `uint64_t sync_reads()` | Reads so far that waited for the GPU |
| `DeviceHealth device_health()` | Tick times lately against the usual, and whether the device has become slow |
| `void move_to_device(device)` | Continues on another device with the same state, bit for bit; handles and `read_each_tick` ids stay valid. Throws, changing nothing, if the device cannot be used |

### Small types

| Type | Members |
| --- | --- |
| `ModifierHandle` | `slot`, `version`, `valid()`; stale handles never match a reused slot |
| `ModifierContribution` | `handle`, `bundle`, `entity`, `row` (where the instance sits), `value` (raw, scaled) |
| `OrderIssue` | `stage`, `field`, `parallel` (false: forwards and backwards differ; true: the parallel run differs), `first_row`, `rows` |
| `CompileStats` | `compiled`, `cached`, `ms` |
| `SaveMatch` | `SameGame` (default: refuse a save of another game) or `ByName` (fit it by name) |
| `DeviceLost` | Thrown (a `std::runtime_error`) by every call that ticks or reads the device once the device is lost, for example after a rule ran too long. Start a new `GameSim` and load the last save |
| `BufferRobustness` | `DeviceInfo::robustness`: `None`, `Bounded` (an access past a field's end stays in its buffer, as the driver chooses) or `Defined` (reads give 0, writes are dropped, the same on every device) |
| `StageTime` | `name`, `ran` (in the last tick), `last_ms` (0 if it did not run), `runs` (ticks it ran in since the start or `move_to_device()`), `total_ms` (over those runs; `total_ms / runs` is its average). Times are 0 where the device has no timestamps; `tick_checked()` is not measured |
| `DeviceHealth` | `usual_ms` (lowest median of 32 ticks so far), `recent_ms` (latest median), `ticks` (measured since the start, `load()` or `move_to_device()`), `slow` (over 4 times the usual and at least 1 ms longer) |
| `StageDef` (in `Game::stages`) | `name`, `for_each`, `dispatch`, `clear`, `source`, `source_file` (where the source was read from, for messages), `every`, `offset`; `runs_in(tick)` says whether it runs in the tick after `tick` completed ticks |
| `EntityDef` (in `Game::entities`) | `name`, `capacity`, `fields`, `dynamic`, `spawns`; `field(name)`, `field_index(name)` |
| `SpawnDef` (in `EntityDef::spawns`) | `from`, `count_field`, `parent_field`, `ordinal_field`, `every`, `offset`, and the fields' indices once the world is finished; `runs_in(tick)` |

## Rules and the SPIR-V cache

| Call | Does |
| --- | --- |
| `bool can_compile_rules()` | True for `Terni::core`, false for `Terni::runtime` |
| `CompileStats compile_rules(game, cache_dir, shader_library_dir = "")` | Compiles every rule into the cache without a GPU; throws on a compile error, an unwritable cache, or in `Terni::runtime` |

## Devices

`terni/vk_context.hpp`:

| Call | Does |
| --- | --- |
| `static std::vector<DeviceInfo> VulkanContext::list_devices()` | Every Vulkan device: `index`, `name`, `type`, `usable`, `why_not`, `uuid`, `vendor_id`, `luid` and `luid_valid` (Windows), `driver` (name and version), `robustness` (what a rule reading or writing past a field's end does there); `cpu()` is true for a software driver. Empty on a machine with no Vulkan driver |
| `void add_vulkan_driver(library_path)` | Adds a driver shipped with the game (a software driver such as lavapipe) to the system's, for the rest of the process. Call before listing or picking devices. Throws if the file is not a Vulkan driver or the Vulkan loader is older than 1.3.234 |
| `int pick_device(preference, const std::array<uint8_t,16>* uuid = nullptr, const std::array<uint8_t,8>* luid = nullptr)` | The device `GameSim` would choose: a UUID match, then a LUID match, then by preference; -1 if none is usable |

## Fixed point and randomness

`terni/fixed.hpp` (namespace `terni::fx`) mirrors the GLSL library on the CPU, bit for bit:

| Name | Meaning |
| --- | --- |
| `QF`, `RF`, `ONE` | 8 and 16 fraction bits; rate 1.0 |
| `rate(num, den = 1)`, `qty(num, den = 1)`, `qty_from_count(n)` | Build raw values |
| `mul(x, r)`, `div(a, b)`, `to_agg(q)`, `clamp`, `min`, `max` | As in GLSL |
| `qty_to_double(raw)`, `rate_to_double(raw)` | For display only, never back into the state |

`terni/rng.hpp` (namespace `terni::rng`): `hash32(x)`, `roll(entity, tick, salt)`,
`below(roll, p)`, the same functions as in GLSL.

## State hashes

`terni/state_hash.hpp`:

| Name | Meaning |
| --- | --- |
| `StateSnapshot` | Named fields of raw values, and each field's values per row (`values_per_row(i)`) |
| `std::vector<FieldHash> field_hashes(snapshot)` | One hash per field (`name`, `hash`): the values `GameSim::field_hashes()` computes on the device |
| `std::vector<FieldHash> entity_hashes(field_hashes)` | One hash per entity type ("terni" for the tick and counts; `mod.<entity>.<key>` counts for its entity) |
| `uint64_t combine(hashes)` | One hash of many |
| `std::vector<FieldDifference> differences(a, b)` | Every field whose values differ, matched by name: `field`, `first_row`, `rows` (how many rows differ), `per_row`, `only_in` (1 or 2 for a field in one snapshot only) |
| `hash_fields(snapshot)`, `first_difference(a, b)` | An older, order-dependent hash per field, and the name of the first field whose values differ ("" if none) |

`GameSim::field_hashes()` (table above) hashes the state on the device. `save_snapshot(bytes)`
(in `game.hpp`) reads a save on the host, without a game or a device: the completed ticks,
the state, every column by name and the modifier store, for `differences()` between two
saves. The tool `terni_diff a.sav b.sav [--rows N]` prints those differences.

## Script parser

`terni/script.hpp` (namespace `terni::script`) parses Paradox script, the syntax of the
data folders, tolerating the quirks real game files have. `parse(text, path)` and
`parse_file(path)` return a `Document` with a tree of `Block`s and `Item`s
(`key`, `op`, `scalar`, `block`, `line`, `column`) plus `errors` and `warnings`. It is
used by `load_game`, and by `terni_script_check`, which checks whole mod folders.

## Lower level: `ComputeRunner`

`terni/compute_runner.hpp` runs any list of storage buffers and compute stages as one
pre-recorded command buffer per tick, with batched uploads and readbacks. `GameSim` is
built on it; use it directly only for models that do not fit the world description.
`tick(run)` takes one flag per stage to skip some stages in that tick; each set of
stages that run together is recorded once and reused. The constructor's `constants`
are specialization constants (ids 2 and up) given to every shader. `tick_submit()`,
`tick_done()` and `tick_wait()` split `tick()` for hosts that keep working meanwhile.
`add_extra(spirv)` and `run_extra(id, elements)` run a pipeline outside the tick, on the
same buffers. `stage_timings()` gives each stage's GPU time, as `GameSim::stage_times()` does, and
`last_tick_device_ms()` the tick from submission to its results on the device's clock,
which is what `GameSim::device_health()` judges.
The header documents every call, including the transfers (`queue_write`,
`add_tick_readback`) and `VulkanContext`'s buffer and submission helpers.

## Version

`terni/version.hpp` gives the version of the Terni a program was built with:
`TERNI_VERSION_MAJOR`, `TERNI_VERSION_MINOR`, `TERNI_VERSION_PATCH` and
`TERNI_VERSION_STRING` (macros, usable from C too), and `terni::version_major`,
`version_minor`, `version_patch` and `version_string`. Lockstep peers compare the major
and minor version along with `game_checksum()`
([Releases and compatibility](releases.md#lockstep-peers-need-the-same-terni)).
