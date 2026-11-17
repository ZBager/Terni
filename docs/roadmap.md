# Roadmap

Terni is pre-1.0. What larger games needed first is done; what remains is ongoing work,
integrations for game engines, and the preparation of stable public releases. The order
is a priority, not a promise.

## Ongoing

- **Parity on more hardware**, Intel Arc and more NVIDIA and AMD generations, on Linux
  and Windows. The suite is run on each new machine as it becomes available.
- **A larger example game** that exercises markets, inheritance, workgroup stages and
  entities that come and go. It grows as the library does.

## Engine integrations

Plugins that let a game use Terni through an engine's own scripting and editor, without
touching Vulkan or the C++ API. They live in a separate repository and link Terni as a
library; Terni itself stays engine-agnostic, and changes here only when the change
helps every host. They are done one at a time, each finished and tested before the
next begins. Windows and Linux first.

1. **Flax Engine**: a C++ plugin with generated C# bindings and Visual Scripting nodes.
2. **Godot**: a GDExtension addon for GDScript and C#.
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

## Public releases

1. **Versioned releases** with a changelog and a stable API from 1.0.
2. **Mod tooling**: `terni_script_check` and the order warnings as one command a
   modder runs before publishing.

Not planned: DirectX or Metal back ends, and consoles.

Suggestions and use cases are welcome in the issue tracker.
