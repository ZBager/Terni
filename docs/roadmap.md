# Roadmap

Terni is pre-1.0. The plan has three phases: close the gaps games work around today,
add what larger games need, and prepare stable public releases. Order within a phase
is a priority, not a promise.

## Phase 1: close the workarounds

1. **Parity on more hardware**, Intel Arc and more NVIDIA and AMD generations, on Linux
   and Windows.
2. **Changing capacities in a supported way**, e.g. `WorldBuilder::set_capacity(entity, rows)`.
   Capacities are part of the rules (they appear in the generated GLSL), so games that
   offer several map sizes need several sets of compiled rules.
3. **Partial writes**: one element of a sized field (`write_field(..., row, index, words)`),
   so hosts do not have to mirror a whole row to change one value.
4. **`Game::define(name)`**, a public lookup of constants.
5. **Modifiers that inherit in more than one step** (a province from its district, the
   district from its region, the region from its realm). Today a key inherits through
   one ref field.

## Phase 2: what larger games need

6. **A CPU path** for machines without a usable Vulkan GPU: either a software Vulkan
   driver shipped with the game (it runs the same shaders unchanged), or rules that also
   compile as C++. Both need parity tests.
7. **A tick that does not block**: submit, keep rendering, collect the results a frame
   later.
8. **Device health**: measure tick times and warn, or switch device, when the chosen GPU
   is busy with other work.
9. **Entities at run time** (create and destroy rows during play, with stable ids and
    counts in saves). Pools of rows work for now; this comes when a game needs it.
10. **A larger example game** that exercises markets, inheritance and workgroup stages.

## Phase 3: public releases

11. **Versioned releases** with a changelog and a stable API from 1.0, and a shared
    library build for engines that load Terni from other languages.
12. **Mod tooling**: `terni_script_check` and the order warnings as one command a
    modder runs before publishing.

Not planned: DirectX or Metal back ends, and consoles.

Suggestions and use cases are welcome in the issue tracker.
