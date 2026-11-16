# API reference

Everything is in namespace `terni`. Include `terni/game.hpp` for the world description
and the simulation; the other headers are smaller utilities. Raw values are the
integer form of a field's type (a quantity of 1.5 is `384`, a rate of 1.5 is
`98304`); `terni/fixed.hpp` converts.

- [World description](#world-description): `WorldBuilder`, `load_game`, `Game`
- [Simulation](#simulation): `GameSim`, `GameSimOptions`
- [Rules and the SPIR-V cache](#rules-and-the-spir-v-cache)
- [Devices](#devices)
- [Fixed point and randomness](#fixed-point-and-randomness)
- [State hashes](#state-hashes)
- [Script parser](#script-parser)
- [Lower level: ComputeRunner](#lower-level-computerunner)

## World description

### `WorldBuilder`

Describes a world in code. Every call validates; problems are collected in
`game().errors` with the `Location` passed in.

| Call | Does |
| --- | --- |
| `uint32_t entity(name, capacity)` | Adds an entity type; returns its index |
| `void field(entity, name, type, ref_entity = "", size = 1, default_raw = 0)` | Adds a field |
| `void define(name, type)` / `set_define(name, raw)` | Declares a constant / sets its value |
| `uint32_t modifier_key(name, entity, type, inherit_field = "")` | Adds a modifier key (`Rate`, `Quantity` or `Int`) |
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
```

Loads a game folder (see [Data format](data-format.md)) through `WorldBuilder`. Never
throws on bad data.

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
| `std::string generate_prelude(const Game&)` | The GLSL Terni puts in front of every rule |
| `bool parse_value(text, type, int64_t& raw)` | Exact decimal text to a raw value |

## Simulation

### `GameSimOptions`

| Member | Default | Meaning |
| --- | --- | --- |
| `device` | -1 | A device index; -1 = choose by `preference` |
| `preference` | `Discrete` | `Discrete`, `Integrated` or `Cpu` |
| `device_uuid` | none | Run on the GPU with this UUID if usable (e.g. the renderer's) |
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
| `ModifierHandle add_modifier(bundle, entity, row, scale = fx::ONE, duration = 0)` | Adds an instance; an invalid handle if the store is full |
| `bool set_modifier_scale(handle, scale)`, `bool remove_modifier(handle)` | False if the handle is stale |
| `bool modifier_alive(handle)`, `size_t modifier_count()` | |
| `int32_t modifier_own_total(key, entity, row)` | Without inheritance |
| `int32_t modifier_total(key, entity, row)` | Own plus inherited, from host memory |
| `std::vector<ModifierContribution> modifier_breakdown(key, entity, row)` | Every contributing instance, for tooltips |
| `std::vector<uint8_t> save()` / `void load(bytes)` | The complete state; `load` throws on a bad save and changes nothing |
| `uint64_t checksum()` | `game_checksum` of its game |
| `StateSnapshot snapshot()` | Every field and modifier total, read back now |
| `std::string device_name()`, `std::string report()` | The device; upload time, memory, GPU time per stage |
| `CompileStats compile_stats()` | Stages compiled vs loaded from the cache, time spent |
| `double last_tick_ms()`, `double last_tick_gpu_ms()` | Wall time incl. transfers; GPU time of the stages |
| `uint64_t sync_reads()` | Reads so far that waited for the GPU |

### Small types

| Type | Members |
| --- | --- |
| `ModifierHandle` | `slot`, `version`, `valid()`; stale handles never match a reused slot |
| `ModifierContribution` | `handle`, `bundle`, `entity`, `row` (where the instance sits), `value` (raw, scaled) |
| `OrderIssue` | `stage`, `field`, `parallel` (false: forwards and backwards differ; true: the parallel run differs), `first_row`, `rows` |
| `CompileStats` | `compiled`, `cached`, `ms` |
| `StageDef` (in `Game::stages`) | `name`, `for_each`, `dispatch`, `clear`, `source`, `every`, `offset`; `runs_in(tick)` says whether it runs in the tick after `tick` completed ticks |
| `EntityDef` (in `Game::entities`) | `name`, `capacity`, `fields`; `field(name)`, `field_index(name)` |

## Rules and the SPIR-V cache

| Call | Does |
| --- | --- |
| `bool can_compile_rules()` | True for `Terni::core`, false for `Terni::runtime` |
| `CompileStats compile_rules(game, cache_dir, shader_library_dir = "")` | Compiles every rule into the cache without a GPU; throws on a compile error, an unwritable cache, or in `Terni::runtime` |

## Devices

`terni/vk_context.hpp`:

| Call | Does |
| --- | --- |
| `static std::vector<DeviceInfo> VulkanContext::list_devices()` | Every Vulkan device: `index`, `name`, `type`, `usable`, `why_not`, `uuid`, `vendor_id` |
| `int pick_device(preference, const std::array<uint8_t,16>* uuid = nullptr)` | The device `GameSim` would choose; -1 if none is usable |

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

`terni/state_hash.hpp`: `StateSnapshot` (named fields), `hash_fields(snapshot)` (one
hash per field), `combine(hashes)` (one hash), and `first_difference(a, b)`, the name of
the first field whose values differ between two snapshots ("" if none). Comparing
per-field hashes on peers tells you which field diverged first.

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
The header documents every call, including the transfers (`queue_write`,
`add_tick_readback`) and `VulkanContext`'s buffer and submission helpers.
