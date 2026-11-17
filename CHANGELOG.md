# Changelog

Terni follows semantic versioning from 1.0. Before 1.0, any minor version may change
the API; changes are listed here.

## 0.1.0 (unreleased)

The first public version.

- **Core**: 32-bit fixed point (Q24.8 quantities, Q16.16 rates) with identical results on
  CPU and GPU; exact 64- and 128-bit integers in GLSL; stateless randomness; per-field
  state hashes.
- **World description**: `WorldBuilder` in code, or `load_game()` from a folder in Paradox
  script syntax (schema, common, history, rules), with `file:line` errors; a parser
  that reads the script files of Paradox titles without errors.
- **Simulation**: `GameSim` runs every stage as a compute shader in one pre-recorded
  command buffer per tick, on any Vulkan 1.3 device; `dispatch = workgroup` stages;
  batched host writes and per-tick readbacks of the rows in use.
- **Modifiers**: additive keys, bundles and timed instances with inheritance through a ref
  field, or through several in a row (`inherit = { district region realm }`,
  `"district.region.realm"` in code; `ModifierKeyDef::inherit_steps` replaces
  `inherit_entity`); exact totals; breakdowns for tooltips answered without waiting for the GPU.
- **Saves and multiplayer**: complete, versioned saves that continue bit-identically on
  any device; `game_checksum()` over everything that defines behaviour.
- **Shipping**: a SPIR-V cache keyed by source, not by compiler; `compile_rules()`;
  `Terni::runtime`, a build without the shader compiler; `cache_only`. Several worlds or
  programs may compile into one cache folder at once.
- **Determinism checks**: load-time warnings for stages whose result can depend on thread
  order; `GameSim::tick_checked()` to confirm at run time.
- **Platforms**: Linux, and Windows with Visual Studio 2022 and 2026 (MSVC) or MinGW-w64,
  with the same checksums, SPIR-V cache keys and results on all of them; rule files
  read the same whatever their line endings. `CMakePresets.json` for each platform, and
  continuous integration that runs the whole suite on lavapipe on all three.
- **Entities at run time**: an entity type marked `dynamic = yes`
  (`WorldBuilder::entity_dynamic`) has rows that are created and destroyed during play.
  Rules get `<entity>_alive(i)`, `<entity>_destroy(i)` and `<entity>_generation[i]`,
  loops skip rows that are not alive, and rules ask for new rows through a count field
  (`spawn = { from = province count = raise parent = home }`, `WorldBuilder::spawn`).
  The host gets `GameSim::create_entity`, `destroy_entity`, `entity_alive`,
  `entity_generation`, `entity_count` and `entities_alive`. New rows are given out on
  the device, in built-in stages at the end of each tick, the same way on every device:
  the lowest free row first, and a destroyed row rests for one tick. Saves hold who is
  alive. Games without dynamic entity types
  keep their checksums and compiled rules.
- **Machines without a usable GPU**: `add_vulkan_driver()` loads a software Vulkan driver
  shipped with the game (such as lavapipe), and the rules run on the CPU with the same
  results; `DeviceInfo::cpu()`; `list_devices()` is empty, instead of throwing, where
  there is no Vulkan driver; `terni_run --driver`.
- **Device health**: `GameSim::device_health()` compares recent tick times with the usual
  and says when the device has become slow; `GameSim::move_to_device()` continues on
  another device with the same state, bit for bit.
- **Stage schedules**: `every = 30 offset = 29` in `stages.txt` (or
  `WorldBuilder::stage_schedule`) runs a stage only in some ticks; other ticks do not
  dispatch it, and its `clear` fields keep their values until it runs again.
  `ComputeRunner::tick()` and `tick_checked()` take the stages to run.
- **Benchmark**: `terni_bench` runs a grand-strategy world (up to 100 million pops,
  markets, modifiers, armies that come and go) on every Vulkan device and on a
  multithreaded CPU implementation of the same rules, from 1 to all cores. Every run must
  end with the same state hash. It reports the time per tick and per stage, and writes
  JSON results; see `docs/benchmark.md`. `DeviceInfo::driver` gives a device's driver
  name and version.
- **Partial writes**: `GameSim::write_field(entity, field, row, index, words)` changes
  some values of one row of a sized field, so hosts need not mirror whole rows.
  Host writes queued for one tick now always apply in the order they were made, even
  where they overlap (before, two writes to the same words in one tick could land in
  either order on the GPU).
- **Defines by name**: `Game::define(name)` gives a constant's raw value, so host code
  (tooltips, AI, UI) uses the same numbers as the rules; `Game::define_index(name)`.
  `WorldBuilder::set_define` rejects a value that does not fit the define's type, and a
  define never given a value (it is 0) gets a load warning.
- **Capacities at run time**: `Game::set_capacity(entity, rows)` sizes an entity type
  before the world starts (e.g. for the map size a match picks). `<ENTITY>_CAPACITY` is
  now a specialization constant, so one compiled set of rules and one SPIR-V cache entry
  serve every capacity; rules can no longer test it in `#if`.
  `ComputeRunner` takes extra specialization constants.
- **A tick that does not block**: `GameSim::begin_tick()`, `tick_ready()` and
  `end_tick()` split `tick()`, so a host keeps rendering while the GPU runs the tick.
  Commands made meanwhile apply to the next tick exactly as if made after `end_tick()`
  (timed modifiers included), and `tick_result()` keeps the last collected results.
  `ComputeRunner::tick_submit()`, `tick_done()` and `tick_wait()`;
  `VulkanContext::submit()`, `submitted_done()` and `wait_submitted()`.
- **Sharing a DirectX renderer's GPU**: `GameSimOptions::device_luid` (and a `luid`
  argument to `pick_device()`) runs the simulation on the GPU with a given adapter LUID,
  the ID a DirectX renderer has for its GPU on Windows. `DeviceInfo` gives each device's
  `luid` and `luid_valid`.
- **Licence**: LGPL 2.1 with the Terni Linking Exception (static linking, no relinking or
  reverse-engineering terms for games, compiled shaders belong to the game). Version
  1.1 of the exception adds that Terni may be combined and shipped with libraries
  under the Apache License 2.0, such as shaderc. The SPDX line of the source files no
  longer carries the exception's version.
