# Getting started

This guide builds Terni, runs the two examples, and walks through a first world
written in code and the same kind of world written as a data folder. It takes about
fifteen minutes.

## 1. Install the requirements

Terni needs CMake 3.21 or newer, a C++20 compiler (GCC 11+, Clang 14+, or Visual Studio
2022 or 2026), the Vulkan loader and headers, and shaderc (the library and the `glslc`
tool). A Vulkan 1.3 driver must be installed for at least one device; the drivers of
current NVIDIA, AMD and Intel GPUs have one, and on Linux Mesa's lavapipe, a Vulkan
driver that runs on the CPU, is enough to start.

| System | Packages |
| --- | --- |
| Arch Linux | `cmake ninja gcc vulkan-headers vulkan-icd-loader shaderc vulkan-swrast` (lavapipe) |
| Debian / Ubuntu | `cmake ninja-build g++ libvulkan-dev glslc libshaderc-dev mesa-vulkan-drivers` |
| Windows (Visual Studio) | Visual Studio 2022 or 2026 with "Desktop development with C++" (includes CMake and Ninja), and the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows) (includes shaderc and `glslc`) |
| Windows (MSYS2, UCRT64 shell) | `mingw-w64-ucrt-x86_64-{gcc,libgomp,cmake,ninja,vulkan-headers,vulkan-loader,shaderc}` |

The command-line tools also use OpenMP (part of GCC and MSVC, but a separate `libgomp`
package in MSYS2; `libomp` for Clang). Package names change between releases; if one is
missing, search for "shaderc" or "vulkan loader" in your package manager.

Check that Vulkan sees a device:

```sh
vulkaninfo --summary
```

## 2. Build and test

```sh
git clone https://github.com/ZBager/Terni.git terni && cd terni
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The tests run every GPU test on every usable Vulkan device in the machine and compare
the results bit for bit, so a machine with an NVIDIA card, an integrated AMD GPU and
lavapipe runs each test three times.

`CMakePresets.json` has the same steps ready for each platform; Visual Studio, VS Code
(with the CMake Tools extension) and CLion read it too:

```sh
cmake --preset linux && cmake --build --preset linux && ctest --preset linux
```

On Windows with Visual Studio, open a "Developer PowerShell for VS" (so the compiler is
on `PATH`), or open the folder in Visual Studio and pick the preset there:

```sh
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

The Vulkan SDK installer puts its `Bin` folder on `PATH`; the tools and tests need the
`shaderc_shared.dll` found there. With MSYS2, use the `windows-mingw` preset from a
UCRT64 shell. The programs land in `build-<preset>/` instead of `build/`.

## 3. Run the examples

```sh
./build/hello_towns
./build/terni_run examples/mini_galaxy --ticks 100 --dump
```

`hello_towns` describes a tiny world in C++ and prints how three towns grew.
`terni_run` loads a game folder, runs it on every device, compares the state hashes,
and with `--dump` prints the final state. `--prelude` shows the GLSL that Terni
generated from the folder's schema; `--device N` picks one device.

## 4. A world in code

[`examples/hello_towns/hello_towns.cpp`](../examples/hello_towns/hello_towns.cpp) is
the smallest complete program. It describes the world with `WorldBuilder`:

```cpp
terni::WorldBuilder b;
const uint32_t town = b.entity("town", 1024);            // an entity type with room for 1024 rows
b.field(town, "pop", terni::FieldType::Quantity);        // a Q24.8 fixed-point column
b.field(town, "food", terni::FieldType::Quantity);
b.define("growth", terni::FieldType::Rate);              // a constant the rules can read
b.set_define("growth", terni::fx::rate(1, 100));         // 1 % per tick
```

Then one stage, the GLSL that runs for every town each tick. Every name in it was
generated from the description above:

```cpp
b.stage("grow", "town", R"(
void main() {
    FOR_EACH_TOWN(t) {
        int pop = town_pop[t];
        if (town_food[t] >= pop) pop += fx_mul(pop, D.growth);  // fed towns grow
        town_pop[t] = pop;
    }
}
)");
```

Then the starting state, and `finish()`, which validates everything:

```cpp
for (int i = 0; i < 3; ++i) {
    const uint32_t row = b.create(town);
    b.set(town, row, "pop", terni::fx::qty(1000 * (i + 1)));
    b.set(town, row, "food", terni::fx::qty(2500));
}
auto game = b.finish();
if (!game->ok()) { /* print game->errors: each is "file:line: message" */ }
```

Finally the simulation:

```cpp
terni::GameSim sim(*game);   // compiles the rule, picks a device, uploads the state
for (int t = 0; t < 10; ++t) sim.tick();
auto pops = sim.read_field(town, "pop", 0, 3);   // raw Q24.8 words
```

The third town does not grow: 3,000 people need more than its 2,500 food.

## 5. The same as a data folder

Most games describe their world as files, so modders can change it.
[`examples/mini_galaxy`](../examples/mini_galaxy) is a small example:

```
examples/mini_galaxy/
  schema/entities.txt        entity types, fields, constants, modifier keys
  common/defines.txt         values of the constants
  common/modifiers/static.txt  modifier bundles
  history/empires.txt        starting entities
  history/planets.txt
  rules/stages.txt           the order of the stages
  rules/planet_growth.glsl   one GLSL body per stage
  rules/empire_income.glsl
  rules/empire_treasury.glsl
```

The schema declares what exists:

```
planet = {
    capacity  = 4096
    owner     = ref:empire
    housing   = quantity
    pop       = quantity
}
defines = { growth_rate = rate }
modifier_keys = {
    pop_growth = { applies_to = planet  type = rate  inherit = owner }
}
```

History creates the starting entities, referring to each other by `id`:

```
planet = { id = latium  owner = ROM  housing = 12000  pop = 8200
           add_modifier = { name = good_harvest duration = 24 } }
```

And `rules/stages.txt` sets the tick order:

```
planet_growth   = { for_each = planet }
empire_income   = { for_each = planet  clear = { empire.income } }
empire_treasury = { for_each = empire }
```

Loading it is one call:

```cpp
auto game = terni::load_game("examples/mini_galaxy");
```

## 6. Add Terni to your project

```cmake
add_subdirectory(external/terni)
target_link_libraries(my_game PRIVATE Terni::core)
```

As a subproject only the libraries are built. See
[Embedding in an engine](embedding.md) for FetchContent, `find_package`, the
build without the shader compiler, and how to run the tick inside a game loop.

## Next

- [Concepts](concepts.md): what a world is made of, and who owns what.
- [Writing rules](writing-rules.md): everything a stage can use.
- [Data format](data-format.md): every key the loader understands.
