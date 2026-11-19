# The C API

Terni's main API is C++20. The C API puts the same simulation behind plain C functions,
in a shared library, for everything that cannot use C++ directly: C# (through P/Invoke),
other languages through their foreign function interface, and engines whose own headers
do not compile as C++20 or that are built without exceptions.

```c
#include <terni/terni.h>

terni_world* world = NULL;
terni_world_create(&world);

const char* folders[] = {"data/my_game"};
if (terni_load(world, folders, 1) != TERNI_OK) {
    for (uint32_t i = 0; i < terni_diagnostic_count(world, TERNI_ERRORS); ++i)
        printf("%s\n", terni_diagnostic(world, TERNI_ERRORS, i));   // file:line: message
    return 1;
}
if (terni_start(world, NULL) != TERNI_OK) {                         // the default device
    printf("%s\n", terni_last_error());
    return 1;
}

uint32_t planet = (uint32_t)terni_find_entity(world, "planet");
for (int t = 0; t < 100; ++t) terni_tick(world);

int32_t pop[16];
size_t words = 0;
terni_read_field(world, planet, "pop", 0, 16, pop, 16, &words);    // raw Q24.8 values
printf("pop of row 0: %.2f\n", pop[0] / 256.0);

terni_world_destroy(world);
```

## Which library

| Library | File | What it does |
| --- | --- | --- |
| `terni` (`Terni::c`) | `terni.dll`, `libterni.so` | Compiles rules when a world starts, so mods can change them. Needs shaderc beside it (`shaderc_shared.dll` on Windows) |
| `terni_runtime` (`Terni::c_runtime`) | `terni_runtime.dll`, `libterni_runtime.so` | No rule compiler: every rule comes from a SPIR-V cache filled beforehand ([Shipping](embedding.md#shipping)) |

Both export exactly the functions in `include/terni/terni.h` and nothing else. They are
built when Terni is the top-level CMake project, or with `-DTERNI_C_API=ON`:

```cmake
set(TERNI_C_API ON)
add_subdirectory(external/terni)
target_link_libraries(my_host PRIVATE Terni::c)   # or Terni::c_runtime
```

`cmake --install` puts them in `bin` (Windows) or `lib`, with the header in
`include/terni`. Ship the library with your program, with the licence notices from
`TERNI_NOTICES_DIR` ([Licensing](licensing.md#the-notices-file)).

At startup, compare `terni_abi_version()` with `TERNI_ABI_VERSION` from the header you
built with. A library of another ABI version may lay out its structs differently.

## From C#

`bindings/csharp/Terni` is a C# wrapper over the C API that depends on no engine: a
.NET Standard 2.1 library (C# 8), so .NET 6 and later, Mono and Unity can use it, in a
game, a dedicated server or a test tool. Add the project (or its two source files) to
your solution and put the native library beside your program.

```csharp
using Terni;

using var world = new World();
world.Load("data/my_game", "mods/my_mod");          // throws TerniException; world.Errors lists them
world.Start(new StartOptions { CacheDir = "spirv-cache" });

int planet = world.FindEntity("planet");
int pop = world.Subscribe(planet, "pop");            // copied back with every tick
for (int t = 0; t < 100; ++t) world.Tick();
ReadOnlySpan<int> pops = world.Subscription(pop);    // raw Q24.8 values, no GPU round trip

byte[] save = world.Save();
ulong hash = world.StateHash;                        // compare with lockstep peers
```

Calls that fail throw a `TerniException` whose `Status` says what kind of failure it
was; a lost device throws `DeviceLostException`. The names follow the C API
(`terni_rows_free` is `World.RowsFree`, `terni_device_count` and `terni_device_info`
are `Library.Devices()`). The wrapper loads `terni`; define `TERNI_RUNTIME` when you
build it to load `terni_runtime` instead.

## Conventions

- **A world** (`terni_world*`) holds one game and, once started, its simulation on one
  device. Use a world from one thread at a time. Different worlds may run on different
  threads.
- **Status codes.** Calls that can fail return a `terni_status`: `TERNI_OK` (0), or a
  code that says what kind of failure it was (`TERNI_INVALID` for a bad argument,
  `TERNI_NOT_RUNNING`, `TERNI_DEVICE_LOST`, ...). `terni_last_error()` gives the
  message, for the calling thread. Out parameters are written only on success. No call
  throws, aborts or exits.
- **Lookups** by name (`terni_find_entity()`, `terni_find_field()`, ...) return -1 when
  there is no such thing.
- **Indices.** Entity types, their fields, bundles, modifier keys and defines are
  numbered from 0 in the order the game declares them.
- **Strings** are UTF-8 and end with a NUL byte. A string the API returns belongs to
  Terni; each call says how long it stays valid. On Windows, paths are for now limited
  to the characters of the system code page.
- **Lists** (devices, diagnostics, hashes, stage times, order issues, a modifier
  breakdown) are kept for you: one call fills a list and returns its length, and an
  accessor reads an item by index. A list stays valid until the call that fills it is
  made again, or the world loads, starts, stops or is destroyed.
- **Buffers.** Reads copy into your buffer. If it is too small, the call returns
  `TERNI_TOO_SMALL` and writes the number of words it needs.
- **Raw values.** Fields hold the raw integer form of their type, as in C++: a quantity
  of 1.5 is `384` (Q24.8), a rate of 1.5 is `98304` (Q16.16), a money64 value is two
  words, the low one first.
- **Truth values** are `int32_t`: 0 is false.

## A world from start to end

1. `terni_world_create()`, then `terni_load()` with the game's folder and any mod
   folders on top. On `TERNI_GAME_ERRORS`, `terni_diagnostic()` lists the errors as
   `file:line: message`; warnings are listed either way.
2. Before the start, optionally: `terni_set_capacity()` for the map size a match picks,
   and a starting state made by your code (below).
3. `terni_start()` with options, or `NULL` for the defaults. Fill the options with
   `terni_start_options_init()` first, then change what you need: the device, the
   renderer's GPU (`has_uuid` / `uuid`, `has_luid` / `luid`), the SPIR-V cache folder,
   `cache_only`.
4. Tick: `terni_tick()` blocks; `terni_begin_tick()`, `terni_tick_ready()` and
   `terni_end_tick()` let your program keep rendering while the GPU works
   ([Embedding](embedding.md#a-tick-that-does-not-block)).
5. Read results: `terni_subscribe()` once for each field you show every frame, then
   `terni_subscription()` after each tick (no extra GPU round trip). `terni_read_field()`
   waits for the GPU; keep it for occasional reads.
6. Change the state between ticks: `terni_write_field()`, `terni_write_values()`
   (part of a row), `terni_create_entity()` / `terni_destroy_entity()`, modifiers.
7. `terni_save()` / `terni_load_save()`, `terni_stop()`, `terni_world_destroy()`.

## The renderer's GPU and machines without one

To run the simulation on the GPU your renderer uses, ask the renderer's own objects for
its GPU and find the matching device:

```c
terni_renderer_gpu gpu;
terni_vulkan_renderer_gpu(vk_instance, NULL, vk_physical_device, &gpu);  // a Vulkan renderer
// terni_dxgi_adapter_gpu(dxgi_adapter, &gpu);                          // a DirectX one (Windows)
int32_t device = -1;
terni_find_device(&gpu, &device);           // -1: the renderer's GPU cannot run the simulation

terni_start_options options;
terni_start_options_init(&options);
options.device = device;                    // -1 falls back to the preference
terni_start(world, &options);
```

The match goes by the Vulkan UUID, then the adapter LUID, then by vendor and name when
exactly one usable device has them.

For machines whose GPUs cannot run the simulation (an old GPU, a virtual machine, a
server), ship a software Vulkan driver such as Mesa's lavapipe beside your program, and
call `terni_add_shipped_driver(folder, &result)` before listing devices. It adds the
driver unless the system has a software device of its own, and works once per process.
The default preference then falls back to it when no GPU can run the simulation.

## A starting state from your code

A game that generates its map from a seed writes the starting state before the start,
instead of describing it in history files:

```c
terni_set_capacity(world, province, 20000);       // room for this match's map
terni_set_start_count(world, province, 18432);     // rows in use at the start
terni_write_start(world, province, "terrain", 0, terrain, 18432);
terni_add_start_modifier(world, coastal, province, 17, 65536, 0);
terni_start(world, &options);
```

`terni_read_start()` reads it back. Rows given up by a lower count go back to their
defaults and lose their ids and starting modifiers. Capacities are part of the game's
checksum, so lockstep peers must pick the same ones; the starting state is not, so
peers that generate it must generate the same one (from the same seed) or start from
the same save.

## Lockstep multiplayer

The calls a lockstep game makes ([Embedding](embedding.md#lockstep-multiplayer)):

- `terni_checksum()` before playing together; `terni_save_checksum()` to tell whether
  a save fits before loading it.
- `terni_rows_free()` to refuse a command that would create more entities than there is
  room for, before applying it.
- `terni_state_hash()`, or `terni_entity_hashes()` / `terni_field_hashes()` with
  `terni_hash_at()`, computed on the device and cheap enough to compare every few ticks;
  they name the entity type or field that diverged.
- `terni_tick_checked()` in tests and parity runs: stages whose result depends on the
  order of their threads, read with `terni_order_issue_at()`.
- `terni_add_modifiers()` adds one bundle to many rows in one call, and
  `terni_replace_modifier()` swaps the instance a game keeps one handle for (a policy, a
  focus) in one call.

## A lost device

If the GPU is reset (a rule ran too long) or goes away, the call that finds it returns
`TERNI_DEVICE_LOST` and the world stops: `terni_device_lost()` is then 1. Start the
world again, on the same device or another, and load the last save. To test this path,
`terni_simulate_device_lost()` makes the next call that uses the device find it lost.

## Reference

Every function is declared, with a comment, in `include/terni/terni.h`.

| Group | Functions |
| --- | --- |
| Library | `terni_abi_version`, `terni_version`, `terni_last_error`, `terni_can_compile_rules` |
| Devices | `terni_device_count`, `terni_device_info`, `terni_add_vulkan_driver`, `terni_pick_device` |
| The renderer's GPU, a shipped driver | `terni_vulkan_renderer_gpu`, `terni_dxgi_renderer_gpu`, `terni_dxgi_adapter_gpu`, `terni_find_device`, `terni_shipped_driver_name`, `terni_add_shipped_driver` |
| World | `terni_world_create`, `terni_world_destroy`, `terni_load`, `terni_unload`, `terni_loaded`, `terni_diagnostic_count`, `terni_diagnostic` |
| What the game declares | `terni_entity_count`, `terni_entity_name`, `terni_find_entity`, `terni_capacity`, `terni_dynamic`, `terni_field_count`, `terni_field_name`, `terni_find_field`, `terni_field_info_of`, `terni_find_row`, `terni_row_id`, `terni_define_count`, `terni_define_name`, `terni_find_define`, `terni_define_value`, `terni_bundle_count`, `terni_bundle_name`, `terni_find_bundle`, `terni_modifier_key_count`, `terni_modifier_key_name`, `terni_find_modifier_key`, `terni_checksum` |
| Before the start | `terni_set_capacity`, `terni_set_modifier_capacity`, `terni_set_start_count`, `terni_start_count`, `terni_write_start`, `terni_read_start`, `terni_add_start_modifier`, `terni_compile_rules` |
| Starting and stopping | `terni_start_options_init`, `terni_start`, `terni_stop`, `terni_running`, `terni_device_lost`, `terni_simulate_device_lost` |
| Ticks | `terni_tick`, `terni_begin_tick`, `terni_tick_ready`, `terni_end_tick`, `terni_tick_in_flight`, `terni_ticks`, `terni_tick_checked`, `terni_order_issue_at` |
| State | `terni_read_field`, `terni_write_field`, `terni_write_values`, `terni_subscribe`, `terni_subscription` |
| Entities at run time | `terni_create_entity`, `terni_destroy_entity`, `terni_alive`, `terni_generation`, `terni_rows_in_use`, `terni_rows_alive`, `terni_alive_bits`, `terni_rows_free` |
| Modifiers | `terni_add_modifier`, `terni_add_modifiers`, `terni_replace_modifier`, `terni_set_modifier_scale`, `terni_remove_modifier`, `terni_modifier_alive`, `terni_modifier_instances`, `terni_modifier_total`, `terni_modifier_breakdown`, `terni_contribution_at` |
| Saves | `terni_save`, `terni_load_save`, `terni_fitted`, `terni_save_checksum` |
| Hashes | `terni_field_hashes`, `terni_entity_hashes`, `terni_hash_at`, `terni_state_hash` |
| Measurements | `terni_stage_times`, `terni_stage_time_at`, `terni_health`, `terni_compile_stats_of`, `terni_last_tick_ms`, `terni_last_tick_gpu_ms`, `terni_device_name`, `terni_report`, `terni_move_to_device` |

Worlds described in code (`WorldBuilder`) are C++ only; the C API loads data folders.
In C#, `World` has a method or property for each call on a world, and the static class
`Library` has the rest.
