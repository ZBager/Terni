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
  field; exact totals; breakdowns for tooltips answered without waiting for the GPU.
- **Saves and multiplayer**: complete, versioned saves that continue bit-identically on
  any device; `game_checksum()` over everything that defines behaviour.
- **Shipping**: a SPIR-V cache keyed by source, not by compiler; `compile_rules()`;
  `Terni::runtime`, a build without the shader compiler; `cache_only`.
- **Determinism checks**: load-time warnings for stages whose result can depend on thread
  order; `GameSim::tick_checked()` to confirm at run time.
- **Platforms**: Linux, and Windows with Visual Studio 2022 and 2026 (MSVC) or MinGW-w64,
  with the same checksums, SPIR-V cache keys and results on all of them; rule files
  read the same whatever their line endings. `CMakePresets.json` for each platform, and
  continuous integration that runs the whole suite on lavapipe on all three.
- **Licence**: LGPL 2.1 with the Terni Linking Exception (static linking, no relinking or
  reverse-engineering terms for games, compiled shaders belong to the game).
