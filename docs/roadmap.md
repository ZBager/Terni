# Roadmap

Terni is pre-1.0. What larger games needed first is done, and so is a reproducible
benchmark (only results from Windows are missing). What remains is ongoing work,
integrations for game engines, and the preparation of stable public releases. The
order is a priority, not a promise.

## Ongoing

- **Parity on more hardware**, Intel Arc and more NVIDIA and AMD generations, on Linux
  and Windows. The suite and `terni_bench` are run on each new machine as it becomes
  available.
- **A larger example game** that exercises markets, inheritance, workgroup stages and
  entities that come and go. It grows as the library does.

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
   link the C++ library or that load Terni from other languages. The two integrations
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

1. **Versioned releases** with a changelog and a stable API from 1.0.
2. **Mod tooling**: `terni_script_check` and the order warnings as one command a
   modder runs before publishing.

Not planned: DirectX or Metal back ends, and consoles.

Suggestions and use cases are welcome in the issue tracker.
