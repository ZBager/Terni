# Benchmark

`terni_bench` measures how fast Terni runs a grand-strategy tick on your hardware, and
compares it with a CPU implementation of the same simulation. Anyone can run it, and
results from different machines can be compared.

Every result is checked. Each run must end with the same state hash as the others:
Terni on every Vulkan device, and the CPU reference at every thread count. A run whose
state differs is reported as invalid, not given a time.

## Run it

`terni_bench` is built by default when Terni is the top-level project
(`-DTERNI_BUILD_BENCHMARK=ON`). Build in Release, which is also the default:

```sh
cmake -S . -B build -G Ninja && cmake --build build
./build/terni_bench                    # the medium world (the baseline), every device, 1 to all CPU threads
./build/terni_bench --size large --json results.json
./build/terni_bench --list             # the sizes and the Vulkan devices
```

Close games, browsers with video, and anything else that uses the GPU first: a busy GPU
can make ticks many times slower. On a laptop, plug it in and use its performance
power mode.

| Option | Meaning |
| --- | --- |
| `--size NAME` | `tiny`, `small`, `medium` (default), `large`, `huge`, `giant` or `colossal` |
| `--provinces N`, `--pops-per-province N`, `--empires N`, `--markets N` | Change the size (the world is then called `custom`) |
| `--seed N` | Another starting state (default 1) |
| `--ticks N`, `--warmup N` | Ticks measured, after ticks not measured: 100 and 20, fewer for `huge` and larger (see the sizes below) |
| `--device I` | Only this Vulkan device (repeatable); `--no-gpu` for none |
| `--driver LIBRARY` | Add a Vulkan driver library, such as lavapipe |
| `--lavapipe-threads N` | Threads of Mesa's lavapipe (sets `LP_NUM_THREADS`) |
| `--threads LIST` | CPU reference thread counts: `1,2,8`; `powers` (default: 1, 2, 4, ... and all); `sweep` (every count from 1 to all); `all` |
| `--no-cpu` | No CPU reference |
| `--readback WHAT` | What comes back to system memory every tick: `pops` (default; every field of every pop, as a host that keeps a copy of the whole world would, and what `ui` reads) or `ui` (only province unrest and empire treasuries, as a game's map and interface would read) |
| `--frame-load PERCENT` | Also run every device beside a stand-in for a game's renderer, whose frames keep the GPU busy for PERCENT (1 to 95) of each 16.7 ms; see [Beside a renderer](#beside-a-renderer) |
| `--stages` | Also print the GPU time of every stage |
| `--json FILE` | Also write the results, the machine and the versions as JSON |

The exit code is 0 when every run finished with the same state, and 1 otherwise. A
device that cannot start the world, because it is too large for it, is shown as
skipped and does not change the exit code.

## The world

A small grand-strategy economy, built in code from a size and a seed
(`bench/bench_world.cpp`):

| Entity | Count (medium) | What it does |
| --- | --- | --- |
| Pop | 1,000,000 (100 per province) | Works a job making one of 8 goods, earns, pays tax, buys what it needs, grows or shrinks, changes job when unhappy |
| Province | 10,000 | Sums its pops, pays its empire, gains unrest, sometimes raises an army |
| Market | 500 | Sets a price for each good from supply and demand |
| Empire | 100 | Keeps a treasury, pays for armies and administration, changes its tax once a month |
| Army | up to 2,500, created and destroyed as provinces revolt | Wears down, is paid for, calms its province |

The sizes (`medium` is the baseline: the size results are quoted and compared at):

| Size | Provinces | Pops | Empires | Markets | Fields | Ticks (warm-up + measured) |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `tiny` | 64 | 1,024 | 4 | 8 | 0.04 MiB | 20 + 100 |
| `small` | 1,000 | 50,000 | 20 | 50 | 1.6 MiB | 20 + 100 |
| `medium` | 10,000 | 1,000,000 | 100 | 500 | 31 MiB | 20 + 100 |
| `large` | 30,000 | 6,000,000 | 300 | 1,500 | 185 MiB | 20 + 100 |
| `huge` | 50,000 | 10,000,000 | 500 | 2,500 | 308 MiB | 10 + 50 |
| `giant` | 100,000 | 50,000,000 | 1,000 | 5,000 | 1.5 GiB | 5 + 30 |
| `colossal` | 100,000 | 100,000,000 | 1,000 | 5,000 | 3.0 GiB | 5 + 30 |

`huge`, `giant` and `colossal` are there to find the limits of a machine:

- Their fields must fit in the GPU's memory.
- One field of their pops must fit in a single storage buffer. lavapipe allows 128 MiB,
  so it runs `huge` but skips `giant` and `colossal`.
- The CPU reference takes seconds per tick on one thread, which is why these sizes
  measure fewer ticks.
- `colossal` needs about 7 GiB of RAM, and a run of every device and thread count takes
  a few minutes.

Each tick runs eight stages, using what real games use:

- stages with one thread per row, over pops, armies, markets and empires;
- a workgroup stage, in which 256 threads sum each province's pops;
- sums into other rows through integer atomics;
- modifiers with timed instances, inherited from empire to province;
- `rng_roll`;
- 64-bit treasuries;
- a monthly stage;
- dynamic entities, with Terni's built-in stages handing out new rows.

Before every tick, the host adds a few modifiers for events (harvests, famines,
reforms). With every tick, every pop comes back to system memory, as a host that keeps
a copy of the whole world would want; `--readback ui` brings back only two fields, as a
game's map and interface would read.

The world has a version, printed as `terni_bench 1`. When the world, its rules or the
host's events change, the version goes up. Compare results only between runs with the
same version and size.

## The CPU reference

`bench/cpu_reference.cpp` runs the same eight stages in plain C++. It is written the way
a careful CPU engine would be:

- flat arrays in the same layout as Terni's, with no allocation per entity;
- the same fixed-point and random functions (`terni::fx`, `terni::rng`);
- a pool of threads created once, with each stage's rows split statically between them;
- the large sums (pops' goods to markets, provinces' taxes to empires) kept per thread
  and combined after the stage, where Terni uses atomics; the small ones (armies,
  administration costs) through atomics, as in Terni.

After every tick its state is bit-identical to Terni's.

It is a fair baseline, not the fastest CPU code possible:

- It runs the same stages in the same order and does not fuse loops.
- It has no hand-written SIMD.
- It is compiled for the baseline instruction set (no `-march=native`), as a game
  shipped to players would be.

Loops with too little work to be worth waking threads for run on fewer threads, or on
the calling thread alone; so do the stages over empires, which are few.

## Reading the results

```
                                                 total ms           p10 - p90       p99  compute ms  vs 1 CPU  start ms
NVIDIA GeForce RTX 4050 Laptop GPU                  3.493       3.212 - 3.945     4.659       0.498       ...       158
```

| Column | Meaning |
| --- | --- |
| total ms | Median time of a tick as the host sees it: compute, readback to system memory, and submitting and waiting (with the host's events) |
| p10 - p90 | The spread of total ms: 10 % of ticks were faster than the first number, 10 % slower than the second |
| p99 | 1 % of ticks were slower than this |
| compute ms | Median time of the stages alone, from GPU timestamps. The CPU reference has nothing to read back, so for it compute and total are the same |
| vs 1 CPU | How many times faster than the CPU reference on one thread, by total ms |
| start ms | Creating the simulation, with the rules loaded from a SPIR-V cache (the compile time, done once, is printed above the table); for the CPU reference, copying the starting state |

Things to keep in mind:

- **Small worlds favour the CPU.** A tick on a GPU costs a submission and a wait, a few
  hundredths of a millisecond to a few tenths, however little work it has. In the
  `tiny` world the CPU reference wins easily. Terni pays off as worlds grow.
- **lavapipe is not the CPU reference.** lavapipe runs the same compute shaders on the
  CPU. That is what makes Terni work on machines without a usable GPU, but shaders
  written for a GPU run more slowly there than C++ written for the CPU. Workgroup
  stages with barriers are the slowest part. `--lavapipe-threads` and `--threads` with
  the same count compare the two on the same cores.
- **Total and compute differ** by the readback, the cost of submitting and waiting, and
  the host's events. Most of it is the readback: by default every pop field comes back
  every tick, 31 MiB at `medium` and 3 GiB at `colossal`. On a discrete GPU that
  crosses PCIe and costs more than the compute itself; on an integrated GPU, which
  shares the system's memory, it costs less. What came back is checked against the
  state after the last tick. The CPU reference has its state in system memory already,
  so it is the same in both modes.
- **Read back only what the host needs.** With `--readback ui`, only what a game's map
  and interface would show comes back, a few hundred KiB at most, and total is close to
  compute. Games that also start a tick and collect it a frame later (`begin_tick()` /
  `end_tick()`) hide most of what is left.
- **Thread counts past the physical cores** use SMT (Hyper-Threading), which adds less
  than a core. The header shows physical cores and threads.

## Beside a renderer

A game's renderer uses the same GPU as Terni. `--frame-load PERCENT` measures what the
two do to each other: every device runs twice, alone and then beside a stand-in for a
renderer. The stand-in has its own Vulkan device on the same GPU and a queue that can
draw, as a game's renderer has. From a thread of its own it submits 60 frames a second,
each with GPU work sized at the start to PERCENT of a 16.7 ms frame: compute passes
over a 32 MiB buffer, which keep the GPU's cores and memory busy as drawing would.
Terni uses its own queue, a compute-only one where the device has one.

The table gets a second row for each device (`+ frames 60 %`), and below it, for each
device, the GPU time of a frame alone and beside Terni, and how many frames missed their
16.7 ms. Both runs must end in the same state. Ticks run back to back, as in every run,
so this is the heaviest case: a game that ticks a few times a second leaves its renderer
more room. On lavapipe the stand-in runs on the CPU as well, and competes with the ticks
for cores. A device added with `--driver` is not visible to the stand-in and is skipped.

On two of the machines below (see [Reference results](#reference-results)), `medium`,
`--frame-load 60`, alone and then beside the frames.

| Machine, device, read back | Total ms | p90 | p99 | Compute ms | Frame's GPU time | Frames late |
| --- | --- | --- | --- | --- | --- | --- |
| Laptop, Radeon 760M, every pop | 2.66 → 3.15 | 2.94 → 12.98 | 3.16 → 13.22 | 1.66 → 1.66 | 10.3 → 10.5 ms | 0 of 69 |
| Laptop, RTX 4050, every pop | 3.85 → 7.98 | 4.20 → 8.62 | 4.46 → 9.24 | 0.58 → 0.57 | 10.0 → 15.8 ms | 51 of 67 |
| Laptop, Radeon 760M, `ui` | 1.85 → 2.22 | 2.26 → 12.11 | 2.44 → 12.59 | 1.71 → 1.64 | 10.3 → 10.5 ms | 0 of 56 |
| Laptop, RTX 4050, `ui` | 0.66 → 1.78 | 0.67 → 2.21 | 0.69 → 2.38 | 0.58 → 0.57 | 10.0 → 12.6 ms | 6 of 29 |
| Desktop, 7800X3D integrated Radeon, every pop | 4.72 → 14.58 | 4.74 → 14.83 | 4.81 → 15.08 | 3.77 → 13.53 | 10.6 → 10.5 ms | 0 of 100 |
| Desktop, RTX 4070 Ti, every pop | 1.55 → 3.84 | 1.56 → 4.12 | 1.57 → 4.26 | 0.16 → 0.19 | 9.8 → 15.0 ms | 24 of 40 |
| Desktop, 7800X3D integrated Radeon, `ui` | 3.85 → 13.56 | 3.88 → 13.78 | 3.94 → 13.80 | 3.76 → 13.41 | 10.5 → 10.4 ms | 0 of 84 |
| Desktop, RTX 4070 Ti, `ui` | 0.18 → 0.17 | 0.18 → 1.36 | 0.19 → 1.37 | 0.14 → 0.13 | 10.0 → 10.9 ms | 0 of 20 |

- **On an integrated GPU, a tick that meets a frame waits for it, and the frames hardly
  change.** The Radeon 760M's ticks are short, so most of them miss the frames and only
  p90 and p99 rise, to about a frame and a tick. The 7800X3D's 2-compute-unit GPU takes
  3.8 ms a tick, so nearly every tick meets a frame and takes about 14 ms; even its
  compute time rises, because the frame runs between its stages.
- **On a discrete GPU, the stages take about as long as alone, but every tick takes
  longer, and so do the frames.** With every pop read back (31 MiB a tick), ticks back to
  back push half or more of the frames past 16.7 ms on both machines. With only what the
  interface shows read back, the RTX 4070 Ti still ticks in 0.17 ms (ticks that meet a
  frame take about 1.4 ms) and no frame is late.
- So read back only what your interface needs, and tick as often as the game needs, not
  as fast as the GPU allows. On a slow integrated GPU, expect a tick to take as long as
  a frame and a tick together. `GameSim::stage_times()` and `device_health()` measure the
  same things inside a game.

## Charts

The charts show the reference results below; the tables there hold every number in
them. Where a machine has no result for a device or a size, the chart says N/A.

### Every population tier

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/benchmark/tiers-time-dark.svg">
  <img alt="Time per tick on the RTX 4050 laptop at every tier from 1,000 to 100 million pops, on a logarithmic scale, for the RTX 4050 (compute alone and with every pop read back), the Radeon 760M, lavapipe and the CPU reference on 12 threads and on one. The GPU lines stay one to two orders of magnitude below the CPU lines from the medium tier up; lavapipe is N/A at giant and colossal." src="images/benchmark/tiers-time-light.svg">
</picture>

Only in the smallest world is the CPU reference faster than every GPU: a GPU tick costs a
submission and a wait, however little work it has. From a million pops up, the RTX 4050
computes a tick 55 to 60 times faster than one CPU thread, and every line rises about as
steeply as the world grows. Reading every pop back makes a tick five to eight times as
long as its compute, yet it stays ahead of all twelve threads at every size from `medium` up.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/benchmark/tiers-speedup-dark.svg">
  <img alt="Columns showing how many times faster than all 12 CPU threads each approach is at every tier on the RTX 4050 laptop. RTX 4050 compute rises from 1.1x at tiny to 8 to 9x from medium up; with every pop read back it stays between 1.2x and 1.5x; the Radeon 760M reaches about 2x; lavapipe stays below 1x and is N/A at giant and colossal." src="images/benchmark/tiers-speedup-light.svg">
</picture>

Against all twelve CPU threads, the GPU's lead stops growing at about a million pops:
the RTX 4050 computes 8 to 9 times faster from there on, and 1.2 to 1.5 times faster with
every pop read back. The integrated Radeon 760M, which reads back from the memory it
shares with the CPU, is faster than the RTX 4050 from `medium` up once everything comes
back.

### Every machine and tier

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/benchmark/speedup-map-dark.svg">
  <img alt="Two grids, compute only and every pop read back, with a row for each GPU and a column for each tier, giving how many times faster than all CPU threads of the same machine that GPU is. Blue cells mean the GPU is faster, red that the CPU is. The RTX 4050 laptop and the desktop have results at every tier; the other machines only at medium, and the other cells are N/A." src="images/benchmark/speedup-map-light.svg">
</picture>

Each GPU against the CPU reference on all threads of its own machine. The RTX 4050
laptop and the desktop have results at every tier; the other three machines were
measured at `medium` only. The discrete GPUs compute faster from 50,000 pops up;
reading every pop back narrows their lead on Linux to 1.2 to 2.1 times from `medium` up. The
7800X3D's two-compute-unit integrated GPU is at best about as fast as its own CPU.

### The baseline on every machine

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/benchmark/medium-machines-dark.svg">
  <img alt="The medium world on each of five machines, and the desktop again with Windows, one row per approach: discrete GPU, integrated GPU, lavapipe, CPU reference on all threads and on one. GPU rows show compute alone and the total with every pop read back. Machines without a discrete or integrated GPU show N/A on that row." src="images/benchmark/medium-machines-light.svg">
</picture>

The medium world on every machine, one row per approach. A filled dot is the compute
alone, a ring the total with every pop read back; the line between them is the cost of
the readback.

The charts are drawn by `python3 bench/charts/make_charts.py` (it needs matplotlib) from
`bench/charts/results.csv`; `bench/charts/add_results.py` adds a machine's JSON files to
that table.

## Reference results

`terni_bench 1`, with each size's default ticks, on five machines with Linux and GCC 16.
The first two were measured on 25 November 2026, with NVIDIA driver 620.73 and Mesa 26.3;
the others on 18 and 19 November with an earlier Terni, NVIDIA driver 620.70 and Mesa 26.2
(RADV, the Intel driver and lavapipe).

- **RTX 4050 laptop**: Ryzen 5 7640HS (6 cores, 12 threads), Radeon 760M integrated GPU,
  RTX 4050 Laptop GPU. Every size, with Terni e7b100b. Its GPUs' times with every pop read
  back are medians of five runs; the rest are single runs.
- **Desktop**: Ryzen 7 7800X3D (8 cores, 16 threads), its integrated Radeon GPU (2
  compute units), RTX 4070 Ti. Every size, with Terni b51a1c9 (the two commits differ only in
  small fields, which this world does not use), each a single run; the RTX 4070 Ti's
  times with every pop read back vary by up to about 3% between runs.
- **RTX 3070 laptop**: Ryzen 5 5600H (6 cores, 12 threads), RTX 3070 Laptop GPU.
- **Intel laptop**: Core i5-7200U (2 cores, 4 threads, from 2016), Intel HD Graphics 620
  integrated GPU.
- **Steam Deck**: its AMD APU (4 Zen 2 cores, 8 threads, and an integrated RDNA 2 GPU with
  8 compute units), with SteamOS; built and run in an Arch Linux container (Distrobox).
- **Desktop, Windows**: the same desktop with Windows 11 (25H2), built with Visual Studio
  2022 (MSVC 19.44), NVIDIA driver 610.88 and AMD's driver 25.2.1. Every size; no
  lavapipe. Logitech, AMD and fan-control programs were running in the background.

Every run ended in the same state as every other at the same size, on every machine:
`medium` (hash `83415c984147cf51`) on all of them, every size on the RTX 4050 laptop and
the desktop, with Linux and with Windows.

### `medium`, the baseline

Every pop read back each tick (the default):

| RTX 4050 laptop | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4050 Laptop | 3.83 | 0.50 | 7.8x |
| Radeon 760M | 2.68 | 1.66 | 11x |
| lavapipe, 12 threads | 38.0 | 36.0 | 0.8x |
| CPU reference, 1 thread | 29.7 | | 1x |
| CPU reference, 2 threads | 15.2 | | 2.0x |
| CPU reference, 4 threads | 8.06 | | 3.7x |
| CPU reference, 8 threads | 6.26 | | 4.7x |
| CPU reference, 12 threads | 4.52 | | 6.6x |

| Desktop | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4070 Ti | 1.56 | 0.16 | 18x |
| Ryzen 7800X3D integrated Radeon | 4.80 | 3.77 | 5.8x |
| lavapipe, 16 threads | 24.9 | 24.1 | 1.1x |
| CPU reference, 1 thread | 27.8 | | 1x |
| CPU reference, 2 threads | 13.9 | | 2.0x |
| CPU reference, 4 threads | 7.09 | | 3.9x |
| CPU reference, 8 threads | 3.69 | | 7.5x |
| CPU reference, 16 threads | 3.06 | | 9.1x |

| Desktop, Windows | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4070 Ti | 1.56 | 0.16 | 22x |
| Ryzen 7800X3D integrated Radeon | 6.13 | 4.53 | 5.5x |
| CPU reference, 1 thread | 33.9 | | 1x |
| CPU reference, 2 threads | 17.4 | | 2.0x |
| CPU reference, 4 threads | 9.00 | | 3.8x |
| CPU reference, 8 threads | 6.96 | | 4.9x |
| CPU reference, 16 threads | 5.54 | | 6.1x |

| RTX 3070 laptop | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 3070 Laptop | 6.21 | 0.39 | 6.1x |
| lavapipe, 12 threads | 54.2 | 49.6 | 0.7x |
| CPU reference, 1 thread | 37.6 | | 1x |
| CPU reference, 2 threads | 19.2 | | 2.0x |
| CPU reference, 4 threads | 11.0 | | 3.4x |
| CPU reference, 8 threads | 8.22 | | 4.6x |
| CPU reference, 12 threads | 8.09 | | 4.6x |

| Intel laptop | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| Intel HD Graphics 620 | 22.1 | 17.2 | 2.6x |
| lavapipe, 4 threads | 223 | 220 | 0.3x |
| CPU reference, 1 thread | 57.5 | | 1x |
| CPU reference, 2 threads | 28.9 | | 2.0x |
| CPU reference, 4 threads | 22.2 | | 2.6x |

| Steam Deck | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| Steam Deck GPU | 3.80 | 2.60 | 12x |
| lavapipe, 8 threads | 113 | 110 | 0.4x |
| CPU reference, 1 thread | 44.9 | | 1x |
| CPU reference, 2 threads | 22.6 | | 2.0x |
| CPU reference, 4 threads | 11.8 | | 3.8x |
| CPU reference, 8 threads | 10.2 | | 4.4x |

What they show:

- **The time to read every pop back depends on the PCIe link.** The RTX 4070 Ti
  computes a tick 19 times faster than all 16 threads of the 7800X3D, and reads back at
  about 23 GB/s: with every pop read back it is still twice as fast. The RTX 4050
  reads back at about 10 GB/s, 1.2 times faster than its 12 CPU threads in all. Its compute
  alternates between 0.50 and 0.58 ms from run to run. The RTX
  3070 Laptop computes faster than the RTX 4050 (0.39 ms) but reads
  back at about 5.5 GB/s, so in all it takes 6.2 ms, still 1.3 times faster than its 12
  CPU threads.
- **Integrated GPUs differ widely.** The Radeon 760M (8 compute units) shares the
  system's memory, reads back cheaply, and is 1.7 times as fast as its 12 CPU threads.
  The 7800X3D's 2-compute-unit Radeon stays behind its CPU's 16 threads (4.80 against 3.06 ms
  with every pop read back). The 2016 HD Graphics 620
  computes a tick 1.3 times faster than all four CPU threads, and with every pop read
  back the two are even. The Steam Deck's GPU is 2.7 times faster than its 8 CPU threads
  with every pop read back, the largest lead of an integrated GPU here: its CPU is small,
  and reading back from shared memory costs it about 1 ms.
- **lavapipe** computes 6 to 11 times slower than the C++ on the same threads.
- **SMT helps unevenly.** From 8 to 12 threads the 5600H gains almost nothing, the
  7640HS about 40 %.

With `--readback ui` (only province unrest and empire treasuries come back), total ms
against compute ms:

| `medium`, `--readback ui` | Total ms | Compute ms |
| --- | ---: | ---: |
| RTX 4070 Ti | 0.18 | 0.14 |
| RTX 3070 Laptop | 0.57 | 0.42 |
| RTX 4050 Laptop | 0.65 | 0.58 |
| Radeon 760M | 1.81 | 1.70 |
| Ryzen 7800X3D integrated Radeon | 3.86 | 3.76 |
| Intel HD Graphics 620 | 17.6 | 17.2 |
| Steam Deck GPU | 2.86 | 2.59 |

The same with `--readback ui` at every size, on the two machines measured at every size:

| Total ms, every size | RTX 4070 Ti | 7800X3D integrated Radeon | RTX 4050 Laptop | Radeon 760M |
| --- | ---: | ---: | ---: | ---: |
| `tiny` | 0.052 | 0.112 | 0.063 | 0.196 |
| `small` | 0.062 | 0.391 | 0.099 | 0.441 |
| `medium` | 0.179 | 3.86 | 0.650 | 1.81 |
| `large` | 1.38 | 17.7 | 4.19 | 9.11 |
| `huge` | 2.33 | 29.3 | 6.72 | 14.8 |
| `giant` | 10.8 | 146 | 27.5 | 64.7 |
| `colossal` | 21.0 | 288 | 53.2 | 127 |

### Every size

The two machines measured at every size; total ms with every pop read back, and the
compute ms in brackets:

| RTX 4050 laptop | RTX 4050 Laptop | Radeon 760M | lavapipe, 12 threads | CPU, 1 thread | CPU, 12 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| `tiny` | 0.065 (0.028) | 0.185 (0.101) | 0.865 | 0.029 | 0.030 |
| `small` | 0.235 (0.058) | 0.463 (0.344) | 3.85 | 1.50 | 0.238 |
| `medium` | 3.83 (0.499) | 2.68 (1.66) | 38.0 | 29.7 | 4.52 |
| `large` | 19.9 (3.34) | 14.2 (8.49) | 143 | 182 | 27.4 |
| `huge` | 30.8 (5.65) | 23.4 (14.2) | 236 | 311 | 47.4 |
| `giant` | 150 (26.3) | 107 (64.2) | N/A | 1,498 | 221 |
| `colossal` | 297 (52.4) | 212 (127) | N/A | 3,000 | 446 |

| Desktop | RTX 4070 Ti | 7800X3D integrated Radeon | lavapipe, 16 threads | CPU, 1 thread | CPU, 16 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| `tiny` | 0.053 (0.024) | 0.115 (0.047) | 0.826 | 0.027 | 0.028 |
| `small` | 0.134 (0.032) | 0.430 (0.320) | 2.82 | 1.40 | 0.229 |
| `medium` | 1.56 (0.160) | 4.80 (3.77) | 24.9 | 27.8 | 3.06 |
| `large` | 9.36 (1.27) | 23.7 (17.3) | 91.9 | 167 | 17.7 |
| `huge` | 14.5 (2.14) | 38.4 (28.9) | 150 | 288 | 31.1 |
| `giant` | 71.5 (10.4) | 190 (145) | N/A | 1,402 | 151 |
| `colossal` | 143 (20.7) | 375 (287) | N/A | 2,811 | 302 |

| Desktop, Windows | RTX 4070 Ti | 7800X3D integrated Radeon | CPU, 1 thread | CPU, 16 threads |
| --- | ---: | ---: | ---: | ---: |
| `tiny` | 0.088 (0.030) | 0.153 (0.049) | 0.034 | 0.034 |
| `small` | 0.173 (0.039) | 0.564 (0.381) | 1.71 | 0.325 |
| `medium` | 1.56 (0.164) | 6.13 (4.53) | 33.9 | 5.54 |
| `large` | 12.2 (1.26) | 28.8 (20.4) | 210 | 29.8 |
| `huge` | 14.6 (2.13) | 48.1 (34.1) | 352 | 35.6 |
| `giant` | 71.7 (10.4) | 213 (142) | 1,674 | 175 |
| `colossal` | 142 (20.7) | 414 (275) | 3,357 | 455 |

From `large` up, a tick costs about the same per pop. Compute alone, the RTX 4050 is
55 to 57 times faster than one CPU thread and 8 times faster than twelve; the RTX 4070
Ti 132 to 136 times faster than one thread and 14 to 15 times faster than sixteen.
With every pop read back a tick takes five to seven times as long as its compute, and even so the
RTX 4050 stays about 1.4 to 1.5 times faster than its twelve threads and the RTX 4070 Ti about
twice as fast as its sixteen. lavapipe is N/A at `giant` and `colossal`: one pop
field is larger than the 128 MiB a buffer may have there.

The same desktop with Windows (19 November, an earlier Terni) and with Linux, every pop read back:

- The RTX 4070 Ti computes as fast on both from `medium` up (within 3%). Its total is the
  same at `medium`, `huge`, `giant` and `colossal` too (about 142 ms on both at `colossal`), but
  higher with Windows at `tiny` and `small` (0.09 against 0.05 ms at `tiny`: each tick's
  submission costs more), and at `large` it alternates between about 9.3 and 12.6 ms.
- The integrated Radeon takes 15 to 17% less time with Mesa's RADV on Linux than with
  AMD's own driver on Windows from `small` to `huge` (compute alone), and 2 to 4% more at
  `giant` and `colossal`.
- The CPU reference built with MSVC is 19 to 26% slower on one thread than with GCC on
  Linux, and gains less from more threads (5.54 against 3.06 ms on sixteen at `medium`).
  That is the compiler and its standard library, not Windows: on the same machine with
  Windows, a MinGW-w64 build (GCC 16.2) takes 30.7 ms on one thread and 3.22 ms on
  sixteen at `medium`, with the GPU times unchanged.

`colossal` with `--readback ui`, every pop staying on the GPU: the RTX 4070 Ti takes
21.0 ms per tick, the RTX 3070 Laptop 25.4 ms (24.8 ms compute), the RTX 4050 53.2 ms,
the Radeon 760M 127 ms, the Steam Deck 161 ms and the HD Graphics 620 688 ms, after a
42-second start, all in the same state (hash `efd84db0206e40e3`). With every pop read
back the HD Graphics 620 is skipped: the 3 GiB of state and two 3 GiB readback buffers do
not fit in the memory it shares with the CPU (`vkAllocateMemory` fails).

## Sending in results

Results from other hardware are welcome, above all from GPUs not listed here. Run
`terni_bench --json results.json` (the `medium` world; other sizes too if you have the
time), then open an issue with the
[benchmark result template](https://github.com/ZBager/Terni/issues/new?template=benchmark-result.md)
and attach the JSON file. Besides the results, it holds the CPU and GPU names, the GPU
drivers, core counts, the operating system and its kernel version, the compiler and the
Terni commit, and nothing else about the machine.
