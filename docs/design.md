# Design

Every major choice in Terni serves one requirement: the same inputs must give the
same bits on every machine, cheaply enough to run every tick of a lockstep game. This
page explains the choices and what they cost, then what the benchmark and the first
experiments measured.

## Decisions

| Decision | Why | What it costs |
| --- | --- | --- |
| 32-bit fixed point, no floats in the state | Floating-point results differ between GPUs, drivers and compilers; integer results do not. 32-bit was 1.4–3x faster than 64-bit in the experiments and needs no 64-bit GPU features, which many shader paths lack | Rules use `fx_mul`/`fx_div`; values past 32 bits need `money64` or the wide-integer library |
| Terni's own Vulkan device and GLSL | Engines' shader pipelines differ (HLSL compiled through glslang, for example, has no 64-bit integers); Terni's own device runs the same SPIR-V everywhere and stays engine-independent | A second Vulkan device in the process; the simulation does not share the renderer's queue |
| Worlds described as data, compiled to shaders at load time | Mods can change the rules, not only the numbers; the game checksum covers the rules, so peers must agree | A shader compiler at run time, or a cache prepared at build time (`Terni::runtime`) |
| Terni owns the state; the host sends commands and reads results | One authoritative state on the GPU. Moving state between CPU and GPU every tick is costly: in the benchmark, reading every pop back each tick takes a laptop RTX 4050 about five times as long as computing the tick | Hosts keep small read-only views, refreshed once per tick |
| Modifiers stored on the host, decided by the host | Research, focuses and events are game logic that changes rarely; Terni only keeps exact totals and expiries | Every modifier change goes through the API |
| One pre-recorded command buffer per tick, batched transfers | A tick costs one GPU round trip, uploads and readbacks included | Writes and reads take effect at tick boundaries |
| Sums through integer atomics | Integer addition gives the same total in any order | Rules must not read rows other threads write in the same stage (checked) |
| Fixed capacity per entity type, set before the world starts | Buffers stay static; no memory is allocated during play. Capacities are specialization constants, so one compiled set of rules serves every map size | A world cannot outgrow its capacities; entities that come and go live in the rows of a dynamic entity type, up to its capacity |
| Entities are created at the end of a tick, never in the middle of a stage | Threads that each grabbed a free row would get different rows from run to run. Rules ask through a count field instead; built-in stages number the requests and the free rows in a fixed order, and request k takes free row k, as a single thread would | A new entity is alive from the next tick; the built-in stages pass over the rows of dynamic entity types every tick |
| A destroyed row rests for one tick before it is used again | Every death and every new row shows in the one bit per row the host reads back each tick, and rules that hold a ref get a tick to see the entity gone | A row cannot be reused at once; refs checked less often than every tick compare generations |
| Robust buffer access (robustBufferAccess2) always on | Rules from mods may index past the end of a field; with it such a read gives 0 and such a write is dropped, the same on every device, so a broken mod neither crashes nor desyncs a game | Nothing measurable at `medium`; devices without it get plain robustBufferAccess, whose results differ between drivers |
| A standalone library with no engine code | It is meant to be embedded by any engine | Engine plugins live in [TerniIntegrations](https://github.com/ZBager/TerniIntegrations), and games can embed Terni directly |

## What the benchmark measures

`terni_bench` ([Benchmark](benchmark.md)) runs a grand-strategy world on every Vulkan
device and on a multithreaded C++ implementation of the same rules, which must give the
same bits. Its baseline is the `medium` world, with 1 million pops. On a laptop with a
Ryzen 5 7640HS (6 cores, 12 threads), an RTX 4050 and a Radeon 760M:

- **The GPU computes far faster.** With 1 million pops, the stages take 0.58 ms on the
  RTX 4050 against 31.5 ms on one CPU thread and 4.9 ms on twelve: 54 and 8 times
  faster. A desktop RTX 4070 Ti takes 0.16 ms. From a million pops up the lead stays about
  the same: with 100 million pops the RTX 4050 computes a tick in 52 ms, against 3.0 s
  and 462 ms. In smaller worlds it shrinks, and in the smallest (1,024 pops) the CPU
  wins, 0.03 ms against 0.06 ms: a GPU tick costs a submission and a wait, however
  little work it has.
- **Keep the state on the GPU.** The benchmark copies every pop back to system memory
  each tick (31 MiB for 1 million pops). That takes the RTX 4050's tick from 0.58 ms to
  3.4 ms in all, still 1.4 times faster than twelve CPU threads; the RTX 4070 Ti, with
  twice the PCIe bandwidth, takes 1.5 ms, 2.6 times faster than sixteen. Reading back
  only what an interface shows costs almost nothing (0.65 ms on the RTX 4050).
- **Integrated GPUs pay too.** The Radeon 760M computes the 1-million-pop world in
  1.8 ms, 17 times faster than one CPU thread. It shares the system's memory, so
  reading every pop back costs it little: 2.8 ms in all, less than the RTX 4050.
- **A software driver is a fallback, not a replacement.** lavapipe computes the
  1-million-pop world in 38 ms on twelve threads: 8 times slower than the C++ on the
  same threads, and 66 times slower than the RTX 4050.
- **Exact everywhere.** Every run, on every device and thread count, ends with the same
  state hash, on five machines with NVIDIA, AMD and Intel GPUs, a Steam Deck among them.

## What the first experiments measured

Terni began as a proof of concept comparing CPU and GPU versions of the same models.
These results are outside what the benchmark measures:

- **Data layout beats hardware.** Flat arrays with 12 CPU threads were about 30x faster
  than object-per-pop code, before any GPU was involved.
- **32-bit beats 64-bit.** 32-bit fixed point was 1.4–3x faster than 64-bit and needs no
  64-bit GPU features.
- **Pop groups are cheap.** With pop groups (up to 1,000 people each), a
  50-million-pop galaxy cost about 1 ms per month on one CPU thread; individual pops
  cost 150–370x more. Games with pop groups may not need a GPU for their pops at all.

## What real games taught it

Terni has since run games in development, from a small territory game to a grand
strategy game with 32,000 provinces and 6.5 million pop rows, with lockstep
multiplayer, saves and bots. Every match ended with the same state hash on an RTX
4050, a Radeon 760M and lavapipe.

The problems they found were about embedding, not about wrong results, and shaped
several features:

| Found | Now |
| --- | --- |
| A host engine's global locale wrote `16,384u` into the generated GLSL | The generated text ignores the locale |
| A host engine that exports its own glslang clashed with shaderc in the same process | `Terni::runtime`, `cache_only`, `compile_rules()` |
| Players' systems lacked a compatible shaderc | `Terni::runtime` ships with a prepared cache |
| Per-tick readbacks copied the whole capacity of a mostly empty world | Only the rows in use are copied (0.13 to 0.05 ms per tick on a small world) |
| Tooltips of inherited modifiers waited for the GPU | Inherited totals are answered from host memory |
| A rule read a row that another thread of the same stage wrote; cross-device tests passed anyway | Load-time warnings and `tick_checked()` |
| Monthly stages were dispatched every day, only to test the date | Stage schedules: `every = 30 offset = 29` |
| Changing one value of a sized field meant writing the whole row | Partial writes: `write_field(entity, field, row, index, words)` |
| Two host writes to the same words before one tick could land in either order | Overlapping writes apply in the order they were made (found with synchronization validation) |
| Host code looked up constants by scanning the description | `Game::define(name)` |
| Each map size needed its own compiled rules | Capacities are specialization constants; `Game::set_capacity()` |
| A tick blocked the frame that started it | `begin_tick()` / `end_tick()`; commands made meanwhile go to the next tick |
| Games kept their own pools of rows with an "alive" field, and their own rules for reusing them | Dynamic entity types: `dynamic = yes`, `spawn`, `create_entity()` |
| A machine without a Vulkan 1.3 GPU could not run a game at all | `add_vulkan_driver()` with a software driver shipped by the game |
| Another program on the GPU made ticks twenty times slower, unnoticed | `device_health()` and `move_to_device()` |
| A host that renders with DirectX could not ask for the GPU it renders with | `GameSimOptions::device_luid` |
| Two worlds compiling into one cache folder at once could fail on Windows | Each writer has its own temporary file; a shader already in the cache is left alone |

The ones still open are on the [roadmap](roadmap.md).
