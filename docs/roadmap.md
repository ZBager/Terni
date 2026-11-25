# Roadmap

Terni is pre-1.0. What larger games needed first is done, and so are a reproducible
benchmark and what games need before they ship.
What remains is ongoing work, integrations for game engines, the rest of smaller fields, and
the preparation of stable public releases. The order is a priority, not a promise.

## Ongoing

- **Parity on more hardware**, Intel Arc and more NVIDIA and AMD generations, on Linux
  and Windows. The suite and `terni_bench` are run on each new machine as it becomes
  available.
- **A larger example game** that exercises markets, inheritance, workgroup stages and
  entities that come and go. It grows as the library does.

## Before games ship

What games that embed Terni have run into, and what a studio checks before it depends
on a library. All done.

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
5. **Cheaper and richer spawning** (done). Spawn lines run on a schedule like stages
   (`every`, `offset`); in other ticks only a pass over the alive bits runs, about a
   tenth of the cost or less. An `ordinal` field numbers the rows one asker gets in a
   tick (0, 1, ...), so a new row can tell which of its parent's requests it answers.
6. **Finding a desync** (done). `field_hashes()` hashes the state on the device, cheaply
   enough for every tick, and `entity_hashes()` groups it per entity type; `terni_diff`
   names the fields and rows where two saves differ (`differences()` in code); a long
   test runs 3,000 ticks with saves and entities that come and go across every device
   and against the CPU reference.
7. **A page on when Terni fits** (done): [Is Terni right for your game?](when-terni-fits.md),
   on worlds small enough for the CPU, grouping rows first, what readbacks cost, and
   which work (pathfinding, planning) stays on the CPU and reaches the simulation as
   commands.

## Engine integrations

Plugins that let a game use Terni through an engine's own scripting and editor, without
touching Vulkan or the C++ API. They live in [TerniIntegrations](https://github.com/ZBager/TerniIntegrations) and link Terni as
a library; Terni itself stays engine-agnostic, and changes here only when the change
helps every host. They are done one at a time, each finished and tested before the
next begins. Windows and Linux first.

1. **Flax Engine** (done): a plugin for C++, C# and Visual
   Scripting, with background starts and ticks, cooked games that ship the rule
   compiler (so mods can change rules), mod folders on top of the game's, saves loaded
   by name, a lost device as its own event, state hashes on the device, GPU time per
   stage in the editor, the simulation on the GPU the engine renders with, and a
   software Vulkan driver (Mesa's lavapipe) shipped with the game for machines without
   a usable GPU. Tested on Linux and Windows (the mods, saves by name, lost devices,
   hashes, stage times and the software driver so far on Linux).
2. **Godot** (done): a GDExtension addon for GDScript and C#, with
   background starts and ticks, a Terni tab in the debugger, exported games that ship
   the rule compiler, mod folders on top of the game's, saves loaded by name, a lost
   device as its own signal, state hashes on the device, GPU time per stage in the
   debugger and the Monitors tab, and the simulation on the GPU the engine renders with
   (Vulkan or Direct3D 12), and the same software Vulkan driver. Tested on Linux and
   Windows (the mods, saves by name, lost devices, hashes and stage times so far on
   Linux; the software driver on both).
3. **A C API and a shared library build** (done, [The C API](c-api.md)), in Terni itself, for engines that cannot
   link the C++ library or that load Terni from other languages, with a C# wrapper that
   does not depend on any engine (for dedicated servers and test tools). It covers what
   lockstep games use: `tick_checked()`, counts and the modifier store's capacity before
   the start, a generated starting state, state hashes per entity type, modifiers added
   to many rows in one call, load warnings and compile statistics. With it come a C# wrapper
   and `terni_compile_rules`, the rule compiler a game ships. The two integrations below
   are built on it, and the Flax and Godot plugins move to it.
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
5. **Documentation** (done, results from three laptops, a desktop with Linux and with
   Windows, and a Steam Deck): how to run it, what it measures, how to read the results,
   and an issue template for sending them in.

## Smaller fields

Field types of 8 and 16 bits, chosen field by field; 32 bits stay the default. Every field
used to take at least 32 bits, so a job or a reference to one of 10,000 provinces took up to
four times the memory it needs, and large worlds are limited by memory, not arithmetic.

1. **Small integers and refs** (done): `int8` to `ref16`
   ([Concepts](concepts.md#small-fields)). In the benchmark's world the GPU time of a tick
   falls by 17% (medium) and 13% (large) on a laptop RTX 4050 and by 7% and 8% on a Radeon
   760M with two of the eight pop fields small; with four, by 34% and 19%, and by 10% and 13%. It
   does not change on lavapipe at medium. Devices without 8 and 16-bit storage store them in
   32 bits, with the same results.
2. **Small rates**: a `rate16` (Q2.14, from -2 up to 2), for percentages and
   multipliers. Writes round, so changes too small to show are lost; whether that is worth
   it waits for a game that asks.
3. **The benchmark world with small fields**: a new version of it, with every result measured
   again.

To measure: Windows drivers and discrete GPUs, and the parity test on them; a real device
without 8 and 16-bit storage.

## Public releases

1. **Versioned releases** with a changelog and a stable API from 1.0. Done: the policy
   on what counts as a breaking change, the supported platforms
   ([Releases and compatibility](releases.md)), a version header, and a workflow that
   builds, tests and packages the libraries and tools for Linux, MSVC and MinGW for each
   tag. Still to come: the first tagged release, 0.1.0.
2. **Mod tooling** (done): mod folders load on top of the game's
   (`load_game({game, mod, ...})`), and `terni_check` is the one command a modder runs
   before publishing: files, errors and warnings with `file:line`, compile errors, order
   problems at run time and the same state on every device.

## Parked

Possible, but low priority: each waits for a game that shows it needs it.

- **Indirect dispatch.** A stage launches enough threads for every row its entity type has
  room for (its capacity), fixed when the world starts, however few rows have work to do.
  Dispatches sized on the GPU, from a count or a list of rows that an earlier stage writes,
  would skip the rest. The saving is small: looking at 400,000 idle rows takes about 5
  microseconds on a laptop RTX 4050 and 22 on a Radeon 760M. The cost is more machinery in
  every tick, and lists whose order must not depend on the device. It becomes worth it for
  a game with many large entity types whose stages are mostly idle.

Not planned: DirectX or Metal back ends, and consoles.

Suggestions and use cases are welcome in the issue tracker.
