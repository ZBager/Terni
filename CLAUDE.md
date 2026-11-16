# CLAUDE.md

Guidance for Claude Code (and other assistants) working in this repository.

Terni is a C++20 library that runs the tick of a grand-strategy game as Vulkan compute
shaders, with bit-identical results on every GPU and CPU so it can drive lockstep
multiplayer. Worlds are described in code (`WorldBuilder`) or as Paradox-syntax data
folders (`load_game`), compiled to SPIR-V at load time, and run by `GameSim`. Read
`README.md` and `docs/concepts.md` first; `docs/design.md` explains why things are the
way they are, and `docs/roadmap.md` is the plan: work in its order unless asked
otherwise, and update it when an item is done or the plan changes.

## Hard rules

- **Terni is standalone and engine-agnostic.** Never add code, names, comments, examples
  or documentation that refer to a particular engine, game or private project that uses
  Terni. Integrations live in those projects. Use generic examples (planets, empires,
  provinces, towns). Before committing, grep the diff for anything project-specific.
- **Determinism is the product.** No floating point in simulation state; fixed point
  through `fx_*` / `terni::fx`; sums through integer atomics on `agg` values; randomness
  only from `rng_roll(entity, tick, salt)`; a stage never reads a row another thread of the
  same stage writes. Any change to arithmetic needs a parity test across devices.
- **Generated text must not depend on the process locale.** Streams that produce GLSL,
  cache keys or reports imbue `std::locale::classic()`; identifier checks and case
  changes are ASCII-only.
- **`Terni::runtime` must never link shaderc.** Only `src/rule_compiler_shaderc.cpp` may
  include shaderc; the `runtime_without_shaderc` test checks the binary.
- **Licence header on every new source file** (C++, GLSL, CMake), as the first two lines:
  ```
  // SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception-1.0
  // Copyright (C) 2026 Rafał Zając
  ```
  (`#` comments in CMake files). Do not change `LICENSE` or `LICENSE-EXCEPTION`.
- **Build system: plain CMake.** Do not add a Makefile wrapper, pkg-config files or
  another build system; this was tried and deliberately removed.
- **Windows: MSVC (Visual Studio 2022 and 2026) and MinGW-w64 are both supported**, and
  CI builds both. Keep code standard C++20: no GCC/Clang-only extensions (such as
  `__int128`; tests that use it as a reference are built only where it exists), and no
  non-ASCII characters in public headers (except the licence header), string literals
  or generated text. Line endings must not matter: stage sources are read CRLF -> LF.

## Build and test

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

- `CMakePresets.json` has `linux`, `windows-msvc` and `windows-mingw` presets
  (`cmake --preset linux`, output in `build-<preset>/`). CI (`.github/workflows/ci.yml`)
  runs the suite on lavapipe on Linux, MSVC (VS 2022 and 2026) and MinGW, and checks the
  mini_galaxy state hash; if that example changes, update the hash there and the pinned
  values in `tests/test_portable_text.cpp` (which also pins the prelude's hash, so a
  change to `generate_prelude` updates it too).
- The GPU tests run on **every usable Vulkan device** and compare results bit for bit;
  lavapipe (Mesa's CPU driver) counts as a device. A change is done when the whole suite
  passes on every device in the machine.
- Run a test binary under the validation layer with
  `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./build/test_embedding` and check its
  output for `VUID` / `Validation Error`; a clean run prints none. Confirm the layer loaded
  (`VK_LOADER_DEBUG=layer`) before trusting silence. For changes to transfers or
  submission, also run `test_partial_write`, `test_async_tick` and `test_device_health` with
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1` (CI does).
- `./build/terni_run <game folder> [--ticks N] [--dump] [--prelude] [--device I] [--driver LIBRARY]` runs a
  data folder on every device and compares state hashes; `--prelude` prints the generated
  GLSL. `./build/hello_towns` is the getting-started example.
- `-DTERNI_RULE_COMPILER=OFF` builds only `Terni::runtime` (tools and tests need the
  compiler). `-DTERNI_SCRIPT_CORPUS="<game folders>"` adds the optional `script_corpus`
  test.
- Do not benchmark while another program is using the GPU; tick times can rise
  twentyfold and mislead.

## Layout

| Path | Contents |
| --- | --- |
| `include/terni/` | Public headers. `game.hpp` is the main API (`WorldBuilder`, `load_game`, `GameSim`) |
| `src/game_sim.cpp` | Prelude generation, SPIR-V cache, modifier store, `GameSim`, saves |
| `src/game_builder.cpp`, `src/game_load.cpp` | Validation; the data-folder loader |
| `src/compute_runner.cpp` | Vulkan: buffers, pipelines, the per-tick command buffers, transfers, order-check runs |
| `src/rule_compiler_*.cpp` | GLSL to SPIR-V with shaderc (`core`) or none (`runtime`) |
| `src/order_lint.cpp` | Load-time warnings for order-dependent stages |
| `src/vk_context.cpp` | The Vulkan instance and device, device choice, drivers added by the game |
| `src/device_health.hpp` | Tick-time statistics behind `GameSim::device_health()` |
| `shaders/terni/` | The GLSL library, embedded into the binary at build time |
| `tests/` | Test programs; `tests/data/` holds small games for loader tests, most of them broken on purpose |
| `examples/` | `mini_galaxy` (a data folder) and `hello_towns` (a world in code) |
| `docs/` | User documentation, written for outside game developers |

## Changes that require other changes

- **Public API** (`include/terni/`): update `docs/api.md` and the guide that covers it, and
  add a line to `CHANGELOG.md`.
- **Generated GLSL names or loop macros** (`generate_prelude`, `shaders/terni/compute.glsl`):
  update `docs/writing-rules.md` and `docs/concepts.md`. Any change to `shaders/terni/`
  changes every SPIR-V cache key, so hosts recompile once; that is expected.
- **Compile settings** in `rule_compiler_shaderc.cpp`: change `detail::kCompileSettings`
  in `src/rule_compiler.hpp` too, so old cache files miss. Cache file layout changes bump
  `CACHE_FORMAT`.
- **Save layout**: bump `SAVE_VERSION`; `test_save` must still round-trip byte for byte.
- **Anything `game_checksum` covers** (schema, defines, modifiers, stages): changing what
  it hashes changes every game's checksum and breaks existing saves; treat it as a
  breaking change.
- **New rules in tests or examples** must load without order warnings, or mark a
  deliberate line with `// terni: order ok`.
- **Data-folder syntax** (`game_load.cpp`): update `docs/data-format.md`.
- **Dynamic entity types**: rows that rules ask for are given out by the built-in stages
  of `spawn_stage_sources()`, rows the host asks for by `Impl::allocate()`, both by the
  same rule (lowest free row; a destroyed row rests one tick). `tests/test_dynamic.cpp`
  has that rule as CPU models, so change all three together. The built-in stages must
  not use barriers: lavapipe runs such shaders several times slower.

## Style

- Follow the surrounding code: 4-space indents, lines up to about 120 columns, `snake_case`
  functions and variables, `PascalCase` types.
- Comments say why, not what; keep them short.
- Errors from bad game data are collected as `GameDiagnostic`s (`file:line: message`),
  never thrown; programming errors and failed loads throw `std::runtime_error` /
  `std::invalid_argument`.
- Documentation is plain English for developers who have never seen Terni: short
  sentences, concrete examples, no internal history.

## Git

- `main` is the only branch and is public on GitHub (`origin`). Commit with the
  repository's configured identity; never put a personal email address in commits or
  files.
- Commit, push and especially force-push only when asked. Others may have cloned the
  repository: add new commits rather than rewriting history unless told otherwise.
- Commit messages: a short summary line, then what changed and why.
