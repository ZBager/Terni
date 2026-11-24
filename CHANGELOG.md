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
  any device; `game_checksum()` over everything that defines behaviour. Save format 2
  records entity types, fields and bundles by name, so a patched game loads its older
  saves with `load(bytes, SaveMatch::ByName)`, which fits them by name and returns what
  it changed; `save_checksum()` tells which game a save is from. Format 1 saves still
  load into the game that made them.
- **Shipping**: a SPIR-V cache keyed by source, not by compiler; `compile_rules()`;
  `Terni::runtime`, a build without the shader compiler; `cache_only`. Several worlds or
  programs may compile into one cache folder at once.
- **Determinism checks**: load-time warnings for stages whose result can depend on thread
  order; `GameSim::tick_checked()` to confirm at run time. Functions a stage never calls
  cause no warnings, and a row index may come from a declaration with several names
  (`const uint n = 4u, base = i * n;`) and be a sum in either order (`k + i * n`).
- **Platforms**: Linux, and Windows with Visual Studio 2022 and 2026 (MSVC) or MinGW-w64,
  with the same checksums, SPIR-V cache keys and results on all of them; rule files
  read the same whatever their line endings. `CMakePresets.json` for each platform, and
  continuous integration that runs the whole suite on lavapipe on all three. OpenMP is
  optional, and a MinGW build takes MSYS2's own shaderc before the Vulkan SDK's.
- **Entities at run time**: an entity type marked `dynamic = yes`
  (`WorldBuilder::entity_dynamic`) has rows that are created and destroyed during play.
  Rules get `<entity>_alive(i)`, `<entity>_destroy(i)` and `<entity>_generation[i]`,
  loops skip rows that are not alive, and rules ask for new rows through a count field
  (`spawn = { from = province count = raise parent = home }`, `WorldBuilder::spawn`).
  The host gets `GameSim::create_entity`, `destroy_entity`, `entity_alive`,
  `entity_generation`, `entity_count`, `entities_alive`, `alive_bits` (one bit per
  row, in one call) and `entities_free` (what `create_entity` would find, so a command
  can be refused before it is applied). New rows are given out on the device, in
  built-in stages at the end of each tick, the same way on every device: the lowest
  free row first, and a destroyed row rests for one tick, whoever destroyed it (a row
  the host destroyed used to rest two). Saves hold who is alive. A spawn line may number
  the rows each asker gets (`ordinal = field`) and run on a schedule (`every`, `offset`,
  `SpawnOptions`); in ticks without a line only a pass over the alive bits runs. Games
  without dynamic entity types keep their checksums and compiled rules.
- **Finding a desync**: `GameSim::field_hashes()` hashes every field on the device and
  reads back two words per field (0.3 ms instead of about 30 ms for a snapshot of a
  million pops on a laptop RTX 4050); `field_hashes(snapshot)` gives the same values on
  the host, and `entity_hashes()` one per entity type. `differences(a, b)` names the
  fields and rows where two snapshots differ, `save_snapshot(bytes)` reads a save on the
  host, and the tool `terni_diff a.sav b.sav` prints where two saves differ. A long test
  runs the benchmark world for 3,000 ticks across every device, through saves, against
  the CPU reference.
- **Mods**: `load_game({game, mod, ...})` loads mod folders on top of the game's, a file
  with the same path replacing the game's; messages name the folder a file came from
  (`StageDef::source_file`). `terni_check <game folder> [mod folder ...]` is what a
  modder runs before publishing: the files a mod replaces and adds and those never
  read, every error and warning, compile errors, order problems at run time, the same
  state on every device, and a `ready` or `NOT READY` verdict.
- **Rules from mods**: Terni turns on Vulkan's robustBufferAccess2 where the device has
  it (every current desktop driver, at no measurable cost), so a rule that reads or
  writes past the end of a field reads 0 and writes nothing, the same on every device;
  `DeviceInfo::robustness` says what each device does. A lost device, for example after
  a rule ran too long, throws `terni::DeviceLost`.
- **UTF-8 paths**: paths given to Terni and file names in its messages are UTF-8 on
  every platform. On Windows they used to be in the system code page, so a folder named
  in another script (a player's user folder, say) could not be used. A Windows host that
  passed code-page strings with non-ASCII characters passes UTF-8 now. The command-line
  tools take their arguments as Windows gives them and print UTF-8.
- **C API**: `include/terni/terni.h`, plain C functions over a world handle, in the shared
  libraries `terni` (with the rule compiler) and `terni_runtime`, which export nothing
  else (`TERNI_C_API`, on when Terni is the top-level project). It covers what the C++
  API offers hosts of data-folder games, and adds a starting state written by the host
  before the start, one bundle added to many rows in one call, a modifier replaced in
  one call, the device a Vulkan or DirectX renderer uses (`terni_find_device()`), and a
  software Vulkan driver shipped with the game (`terni_add_shipped_driver()`).
  `terni_start_options_init()` passes the caller's struct size
  (`terni_start_options_init_sized()`), so a newer library does not write past an older
  program's struct; nothing to change in C code.
- **`terni_compile_rules`**: the rule compiler a game that links `Terni::runtime` ships
  beside itself, so mods can change rules (`TERNI_COMPILE_TOOL`, on with the compiler).
- **C#**: `bindings/csharp/Terni`, a wrapper over the C API for .NET Standard 2.1 (.NET,
  Mono, Unity) that depends on no engine; failures are exceptions.
- **Versions and releases**: `terni/version.hpp` (`TERNI_VERSION_STRING`,
  `terni::version_major`, ...), which lockstep peers compare along with the game
  checksum; `docs/releases.md`, on what counts as a breaking change and the supported
  platforms; `cmake --install` also installs the tools; release packages for Linux
  (built on Debian 12), Visual Studio and MinGW built and tested by a workflow (`TERNI_INSTALL_SHADERC` puts
  `shaderc_shared.dll` in the Visual Studio one).
- **Licence notices**: `notices/Terni-NOTICES.txt` (Terni and the rule compiler's
  libraries) and `notices/Terni-runtime-NOTICES.txt` (Terni alone), ready to ship beside a
  game; `cmake --install` installs them, and `TERNI_NOTICES_DIR` names their folder.
- **Machines without a usable GPU**: `add_vulkan_driver()` loads a software Vulkan driver
  shipped with the game (such as lavapipe), and the rules run on the CPU with the same
  results; `DeviceInfo::cpu()`; `list_devices()` is empty, instead of throwing, where
  there is no Vulkan driver; `terni_run --driver`.
- **Device health**: `GameSim::device_health()` compares recent tick times with the usual
  and says when the device has become slow. Ticks are timed on the device's own clock
  (calibrated timestamps), from submission until the results are back, so a host that
  polls a background tick once a frame is judged by the device, not by its frame rate; `GameSim::move_to_device()` continues on
  another device with the same state, bit for bit.
- **GPU time per stage**: `GameSim::stage_times()` (and `ComputeRunner::stage_timings()`)
  gives each stage's GPU time in the last tick, whether it ran, and its runs and total
  time since the start, Terni's own stages included.
- **Stage schedules**: `every = 30 offset = 29` in `stages.txt` (or
  `WorldBuilder::stage_schedule`) runs a stage only in some ticks; other ticks do not
  dispatch it, and its `clear` fields keep their values until it runs again.
  `ComputeRunner::tick()` and `tick_checked()` take the stages to run.
- **Benchmark**: `terni_bench` runs a grand-strategy world (up to 100 million pops,
  markets, modifiers, armies that come and go) on every Vulkan device and on a
  multithreaded CPU implementation of the same rules, from 1 to all cores. Every run must
  end with the same state hash. It reports the total time per tick (compute,
  readback, submission) and the compute time alone, per stage too, its spread up to
  p99, and with `--frame-load` the same beside a stand-in for a renderer on the same
  GPU, with what the ticks cost the renderer's frames; it writes JSON
  results (`docs/benchmark.md`). By default every pop is copied back to system memory
  every tick; `--readback ui` reads back only two fields, as a game's interface would.
  `DeviceInfo::driver` gives a device's driver name and version.
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
- **Waiting for small ticks**: `GameSimOptions::spin_us` (`terni_set_tick_spin()`, C#
  `StartOptions.SpinMicroseconds`) makes waits for a tick spin before they sleep, which
  saves about 10 microseconds a tick. Off by default.
- **Dividing by a define**: `DIV_DEFINE(a, name)` in rules gives exactly `a / D.name`
  without dividing. It is legacy from the start: it keeps working, but new rules should
  divide plainly, since only software devices gain much. Hosts recompile their rules once.
- **Cheaper per-tick readbacks**: the copies are recorded once and reused until what is
  read back changes, which saves about 1.2 microseconds of CPU time a tick.
- **Less host memory**: a running `GameSim` no longer keeps a copy of every field, so it
  needs about as much less host memory as its state has (a world with 305 MiB of fields:
  330 MiB in all instead of 635, where the game's own starting state is the rest), and
  starts 10-17% faster.
- **A starting state in bulk**: `WorldBuilder::create_rows()` and `set_rows()` add rows and
  set whole columns in one call.
- **Faster compiling**: one shader compiler serves all the rules of a world, instead of one
  per rule: 12 rules compile in 0.16 s instead of 1.5 s. The compiled rules are the same.
- **Licence**: LGPL 2.1 with the Terni Linking Exception (static linking, no relinking or
  reverse-engineering terms for games, compiled shaders belong to the game). Version
  1.1 of the exception adds that Terni may be combined and shipped with libraries
  under the Apache License 2.0, such as shaderc. The SPDX line of the source files no
  longer carries the exception's version.
