# Roadmap

Terni is pre-1.0. The plan has two phases: add what larger games need, then prepare
stable public releases. Order within a phase is a priority, not a promise.

## Ongoing

- **Parity on more hardware**, Intel Arc and more NVIDIA and AMD generations, on Linux
  and Windows. The suite is run on each new machine as it becomes available.

## Phase 1: what larger games need

1. **A CPU path** for machines without a usable Vulkan GPU: either a software Vulkan
   driver shipped with the game (it runs the same shaders unchanged), or rules that also
   compile as C++. Both need parity tests.
2. **Device health**: measure tick times and warn, or switch device, when the chosen GPU
   is busy with other work.
3. **Entities at run time** (create and destroy rows during play, with stable ids and
   counts in saves). Pools of rows work for now; this comes when a game needs it.
4. **A larger example game** that exercises markets, inheritance and workgroup stages.

## Phase 2: public releases

5. **Versioned releases** with a changelog and a stable API from 1.0, and a shared
   library build for engines that load Terni from other languages.
6. **Mod tooling**: `terni_script_check` and the order warnings as one command a
   modder runs before publishing.

Not planned: DirectX or Metal back ends, and consoles.

Suggestions and use cases are welcome in the issue tracker.
