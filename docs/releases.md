# Releases and compatibility

What a new version of Terni may change, what it keeps, which platforms it supports, and
what a release package holds.

## Versions

Terni's version is `MAJOR.MINOR.PATCH`, in `terni/version.hpp` (`TERNI_VERSION_STRING`,
`terni::version_major`, ...) and from the C API's `terni_version()`.

- **From 1.0**, Terni follows semantic versioning: a breaking change (below) needs a new
  major version, a new feature a new minor version, and a patch version only fixes bugs.
- **Before 1.0** (now), a minor version may also break things; a patch version still
  never does.

Every change is listed in [CHANGELOG.md](../CHANGELOG.md), breaking ones with what to do
about them.

## What counts as a breaking change

| What | Breaking when | Not breaking |
| --- | --- | --- |
| **C++ API** (`include/terni/*.hpp`) | A public call, type or member is removed, renamed, or changes what it means, so code that built before no longer builds or behaves differently | New calls, types, members and default arguments |
| **C API** (`terni.h`, the shared libraries `terni` and `terni_runtime`) | A function is removed or changes its meaning, or a struct changes its layout; then `TERNI_ABI_VERSION` goes up | New functions, and new fields at the end of a struct that carries its size (`terni_start_options`) |
| **Simulation results** | The same game, starting state and commands give different bits: a change to the GLSL library's arithmetic, the built-in stages, how modifiers sum or expire, or how rows are given out | A change that gives the same bits |
| **The game checksum** | What `game_checksum()` hashes changes, so every game's checksum changes: peers on two versions refuse each other, and saves must be loaded by name | |
| **Saves** | A save from an older version no longer loads | A new save format that also loads the older ones (the format has a version; an older Terni refuses a newer save, and says so) |
| **Data folders and rules** | A key, field type or generated GLSL name (`FOR_EACH_...`, `fx_...`, `D.<define>`) is removed or changes its meaning, so a game or mod that loaded before no longer does, or behaves differently | New keys and names |

The C++ libraries are static and have no stable binary interface: build your game
against the Terni version you ship. The C API's shared libraries do: a host runs with the
library it was built against or any newer one of the same ABI version (an older one may
lack functions added since).

Not covered: the output and options of the command-line tools (except
`terni_compile_rules`, which games ship and run), the benchmark's numbers, and anything
under `src/`.

### Lockstep peers need the same Terni

`game_checksum()` covers your game, not Terni's own code. Results are the same for every
build of the same `MAJOR.MINOR`, on every device and platform, but a new minor (or, from
1.0, major) version may change them. Peers in a lockstep game should therefore compare
Terni's major and minor version as well as the checksum before they play together, and a
replay records them. Patch versions never change results, so peers on 0.3.1 and 0.3.2
stay in step.

### The SPIR-V cache

The cache key covers Terni's GLSL library, so a new version of Terni (any change under
`shaders/terni/`) misses every cached rule once:

- A game that compiles rules (`Terni::core`) compiles them again at its next start and
  carries on.
- A game that ships a prepared cache with `Terni::runtime` must prepare it again with the
  new version (`compile_rules()` or `terni_compile_rules`); with an old cache, the start
  fails with an error that names the missing rules.

## Supported platforms

| Platform | Compilers | How it is tested |
| --- | --- | --- |
| Linux, x86-64 | GCC 12 or newer (the release packages are built with GCC 12 on Debian 12; Terni is developed with GCC 16) | The whole suite on every push (CI, lavapipe), and on NVIDIA, AMD and Intel GPUs on the reference machines of the [benchmark](benchmark.md) |
| Windows 10 and 11, x64 | Visual Studio 2022 and 2026 (MSVC); MinGW-w64 (GCC, MSYS2 UCRT64) | The whole suite on every push with all three (CI, lavapipe), and on an RTX 4070 Ti and an AMD integrated GPU |
| Not supported | macOS, consoles, the web, 32-bit systems | |

Clang is expected to work (the code is standard C++20) but is not tested. ARM64 Linux has
not been tried.

A device must support Vulkan 1.3 with `synchronization2`; Terni needs no 64-bit shader
features. Every current NVIDIA, AMD and Intel driver qualifies, and so does Mesa's
lavapipe on the CPU. The drivers Terni has been tested with are in the
[benchmark's reference results](benchmark.md#reference-results).

## Release packages

Each release on GitHub has three packages, built in Release by the
[release workflow](../.github/workflows/release.yml) after the whole test suite passed
on each, and a `SHA256SUMS` file:

| Package | For |
| --- | --- |
| `terni-<version>-linux-x86_64.tar.gz` | Linux with glibc 2.36 or newer; GCC 12 or newer to link the static libraries |
| `terni-<version>-windows-msvc-x64.zip` | Visual Studio 2022 or 2026 |
| `terni-<version>-windows-mingw-x64.zip` | MinGW-w64 in MSYS2 UCRT64 |

Each holds what `cmake --install` installs:

- `include/terni/`: the headers, `version.hpp` and the C API's `terni.h` among them;
- `lib/`: the static libraries `Terni::core` and `Terni::runtime`, the C API's shared
  libraries (on Windows the DLLs are in `bin/`), and the CMake files for
  `find_package(Terni)`;
- `bin/`: `terni_compile_rules`, `terni_run`, `terni_check`, `terni_diff` and
  `terni_script_check`;
- `share/licenses/terni/`: the licence and the notices files a game ships
  ([Licensing](licensing.md#the-notices-file));
- `share/terni/shaders/`: the GLSL library, for `terni_add_shaders`;
- `README.md` and `CHANGELOG.md`.

At run time they need the Vulkan loader (a GPU driver installs it) and, for everything
that compiles rules, shaderc: on Linux from the system's packages (`libshaderc_shared.so.1`),
in MSYS2 from its `shaderc` package, and in the Visual Studio package `shaderc_shared.dll`
is included beside the programs. The Visual Studio package also needs the Microsoft
Visual C++ Redistributable, which most Windows machines have; the MinGW package needs
MSYS2's UCRT64 runtime (its `bin` folder on `PATH`), as anything built with it does.

Building from source, as in [Getting started](getting-started.md), gives the same
result; the packages save the build.
