# Roadmap

Terni is pre-1.0. What larger games needed first is done, and so is a reproducible
benchmark (only results from Windows are missing). What remains is ongoing work, what
games need before they ship, integrations for game engines, and the preparation of
stable public releases. The order is a priority, not a promise.

## Ongoing

- **Parity on more hardware**, Intel Arc and more NVIDIA and AMD generations, on Linux
  and Windows. The suite and `terni_bench` are run on each new machine as it becomes
  available.
- **A larger example game** that exercises markets, inheritance, workgroup stages and
  entities that come and go. It grows as the library does.

## Before games ship

What games that embed Terni have run into, and what a studio checks before it depends
on a library.

1. **Dynamic entity types, fixed and documented** (done). A row the host destroys rests
   one tick, as documented, not two. The host gets the alive bits of a type in one call
   (`alive_bits(entity)`) and can ask how many rows `create_entity` would find free
   (`entities_free(entity)`), so a lockstep game can refuse a command up front. The
   guides say that a starting count makes every row below it alive, and that per-tick
   readbacks grow with the rows in use.
2. **Measurements that work next to a renderer** (done). GPU time per stage as data
   (`stage_times()`); `device_health()` judged by what the device did (its own clock,
   from submission to results), so that a host that polls a background tick once a
   frame measures the device, not its frame rate; and `terni_bench --frame-load`, which
   runs every device beside a stand-in for a renderer and reports how much the tick time
   spreads and what the ticks cost the renderer's frames.
3. **Saves that survive a patch** (done). `load(bytes, SaveMatch::ByName)` fits a save
   of another version of the game by name: added fields and entity types start as the
   game describes them, removed ones are dropped, sized fields and capacities may change,
   and what cannot be fitted is refused. Peers in a lockstep game keep comparing the full
   checksum.
4. **Rules from mods treated as code** (done). robustBufferAccess2 is on wherever the
   device has it (no measurable cost at `medium`), so a rule that reads or writes past a
   field's end reads 0 and writes nothing on every device; a lost device throws
   `terni::DeviceLost`, so a game can start again from its last save; the guides say what
   a broken or hostile rule can do.
5. **Cheaper and richer spawning.** Spawn lines that run on a schedule like stages
   (`every`, `offset`), or cost nothing in ticks when no row asks, so a type that
   changes once a month does not pay every tick in proportion to its capacity. An
   `ordinal` field that numbers the rows created for one asker in a tick (0, 1, ...),
   so a new row can tell which of its parent's requests it answers.
6. **Finding a desync.** State hashes per entity type in the public API, a tool that
   names the first field and rows where two runs or two saves differ, and a long test
   (thousands of ticks, saves, entities that come and go) compared across devices.
7. **A page on when Terni fits**: worlds small enough for the CPU, aggregating rows
   into groups first, what readbacks cost, and which work (pathfinding, planning) stays
   on the CPU and reaches the simulation as commands.

## Engine integrations

Plugins that let a game use Terni through an engine's own scripting and editor, without
touching Vulkan or the C++ API. They live in [TerniIntegrations](https://github.com/ZBager/TerniIntegrations) and link Terni as
a library; Terni itself stays engine-agnostic, and changes here only when the change
helps every host. They are done one at a time, each finished and tested before the
next begins. Windows and Linux first.

1. **Flax Engine** (done): a plugin for C++, C# and Visual
   Scripting, with background starts and ticks, cooked games that ship the rule
   compiler (so mods can change rules), and the simulation on the GPU the engine renders
   with. Tested on Linux and Windows. Still to come: shipping a software Vulkan driver.
2. **Godot** (done): a GDExtension addon for GDScript and C#, with
   background starts and ticks, a Terni tab in the debugger, exported games that ship
   the rule compiler, and the simulation on the GPU the engine renders with (Vulkan or
   Direct3D 12). Tested on Linux and Windows. Still to come: shipping a software Vulkan
   driver.
3. **A C API and a shared library build**, in Terni itself, for engines that cannot
   link the C++ library or that load Terni from other languages, with a C# wrapper that
   does not depend on any engine (for dedicated servers and test tools). It covers what
   lockstep games use: `tick_checked()`, counts and the modifier store's capacity before
   the start, a generated starting state, state hashes per entity type, modifiers added
   to many rows in one call, load warnings and compile statistics. The two integrations
   below are built on it.
4. **Unreal Engine**: a runtime plugin with a Blueprint API.
5. **Unity**: a package of C# bindings.

Every integration offers the same things:

- Load a world, tick it (blocking or in the background), save and load, with the same
  results as Terni on its own.
- Read fields in bulk through subscriptions, never row by row; write fields and
  modifiers, and create and destroy entities.
- Choose the device, fall back to a software Vulkan driver, and ship precompiled SPIR-V
  with `Terni::runtime`.
- Report load, compile and run-time errors through the engine's own log and events.
- Release everything cleanly on shutdown, scene changes and script or editor reloads.
- An editor panel to configure a world, pick a device and inspect state.
- Installation steps, API documentation and an example project.

## A reproducible benchmark

One command that anyone can run to measure Terni on their own hardware, against a CPU
implementation of the same simulation, with results that can be compared between
machines. Every result is checked: a run whose state hashes differ is reported as
invalid, not as a time. See [Benchmark](benchmark.md).

1. **The benchmark world** (done): empires, provinces, pops, markets with eight goods,
   and armies that come and go, built in code from a seed and a size, from 1,024 to 100
   million pops. Its stages use what real games use: per-row rules, sums through
   atomics, modifiers with expiry and inheritance, a workgroup stage, `rng_roll`, a
   monthly stage and dynamic entities. The world has a version; results are comparable
   only within one version and one size.
2. **The CPU reference** (done): the same stages in plain C++, written the way a careful
   CPU engine would be. It gives the same state hash as Terni after every tick, on 1 to
   every logical core.
3. **`terni_bench`** (done): runs the world on every usable Vulkan device and on the
   CPU reference at a list of thread counts. It reports the total time per tick
   (compute, reading every pop back, submission) and the compute time alone, their
   spread, the GPU time per stage, the compile and start times, and lavapipe's thread
   count, and writes JSON with the machine, the drivers and the Terni version. Devices
   too small for a world are skipped.
4. **A smoke test** (done): the tiny world on every device and on the CPU reference,
   compared after every tick; timings are never checked in CI.
5. **Documentation** (done, results from three laptops, a desktop and a Steam Deck so
   far): how to run it, what it measures, how to read the results, and an issue template
   for sending them in. Still to come: reference results from Windows.

## Public releases

1. **Versioned releases** with a changelog and a stable API from 1.0: a written policy
   on what counts as a breaking change (the API, the save format, `game_checksum`, the
   cache format), a table of supported platforms, and built libraries and tools for
   Linux, MSVC and MinGW with each release. The licence notices a game needs ship with
   Terni itself, with an install rule. TerniIntegrations gets CI of its own.
2. **Mod tooling**: `terni_script_check` and the order warnings as one command a
   modder runs before publishing.

Not planned: DirectX or Metal back ends, and consoles.

Suggestions and use cases are welcome in the issue tracker.
