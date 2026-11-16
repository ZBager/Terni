# Roadmap

Terni is pre-1.0. The plan has two phases: add what larger games need, then prepare
stable public releases. Order within a phase is a priority, not a promise.

## Ongoing

- **Parity on more hardware**, Intel Arc and more NVIDIA and AMD generations, on Linux
  and Windows. The suite is run on each new machine as it becomes available.

## Phase 1: what larger games need

1. **A larger example game** that exercises markets, inheritance, workgroup stages and
   entities that come and go.
2. **New rows given out on the GPU.** Rows that rules ask for are created on the host
   between ticks, which costs a readback of the asking field. For millions of asking
   rows a tick, do it in a built-in stage instead.

## Phase 2: public releases

3. **Versioned releases** with a changelog and a stable API from 1.0, and a shared
   library build for engines that load Terni from other languages.
4. **Mod tooling**: `terni_script_check` and the order warnings as one command a
   modder runs before publishing.

Not planned: DirectX or Metal back ends, and consoles.

Suggestions and use cases are welcome in the issue tracker.
