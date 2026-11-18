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
                                                 total ms           p10 - p90  compute ms  vs 1 CPU  start ms
NVIDIA GeForce RTX 4050 Laptop GPU                  3.511       3.337 - 3.956       0.525      9.0x       ...
```

| Column | Meaning |
| --- | --- |
| total ms | Median time of a tick as the host sees it: compute, readback to system memory, and submitting and waiting (with the host's events) |
| p10 - p90 | The spread of total ms: 10 % of ticks were faster than the first number, 10 % slower than the second |
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

## Reference results

`terni_bench 1`, with each size's default ticks, on five machines with Linux, GCC 16,
NVIDIA driver 620.70 and Mesa 26.2 (RADV, the Intel driver and lavapipe):

- **RTX 4050 laptop**: Ryzen 5 7640HS (6 cores, 12 threads), Radeon 760M integrated GPU,
  RTX 4050 Laptop GPU.
- **RTX 3070 laptop**: Ryzen 5 5600H (6 cores, 12 threads), RTX 3070 Laptop GPU.
- **Desktop**: Ryzen 7 7800X3D (8 cores, 16 threads), its integrated Radeon GPU (2
  compute units), RTX 4070 Ti.
- **Intel laptop**: Core i5-7200U (2 cores, 4 threads, from 2016), Intel HD Graphics 620
  integrated GPU.
- **Steam Deck**: its AMD APU (4 Zen 2 cores, 8 threads, and an integrated RDNA 2 GPU with
  8 compute units), with SteamOS; built and run in an Arch Linux container (Distrobox).

`medium` (1,000,000 pops), the baseline, with every pop read back each tick (the
default). Every run on all five machines ended in the same state (hash
`83415c984147cf51`).

| RTX 4050 laptop | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4050 Laptop | 3.51 | 0.53 | 9.0x |
| Radeon 760M | 2.64 | 1.70 | 12x |
| lavapipe, 12 threads | 40.7 | 37.9 | 0.8x |
| CPU reference, 1 thread | 31.7 | | 1x |
| CPU reference, 2 threads | 16.4 | | 1.9x |
| CPU reference, 4 threads | 8.98 | | 3.5x |
| CPU reference, 8 threads | 6.56 | | 4.8x |
| CPU reference, 12 threads | 5.09 | | 6.2x |

| RTX 3070 laptop | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 3070 Laptop | 6.21 | 0.39 | 6.1x |
| lavapipe, 12 threads | 54.2 | 49.6 | 0.7x |
| CPU reference, 1 thread | 37.6 | | 1x |
| CPU reference, 2 threads | 19.2 | | 2.0x |
| CPU reference, 4 threads | 11.0 | | 3.4x |
| CPU reference, 8 threads | 8.22 | | 4.6x |
| CPU reference, 12 threads | 8.09 | | 4.6x |

| Desktop | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4070 Ti | 1.50 | 0.16 | 20x |
| Ryzen 7800X3D integrated Radeon | 4.80 | 3.84 | 6.1x |
| lavapipe, 16 threads | 25.5 | 24.4 | 1.2x |
| CPU reference, 1 thread | 29.5 | | 1x |
| CPU reference, 2 threads | 14.8 | | 2.0x |
| CPU reference, 4 threads | 7.96 | | 3.7x |
| CPU reference, 8 threads | 5.48 | | 5.4x |
| CPU reference, 16 threads | 4.20 | | 7.0x |

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
  computes a tick 27 times faster than all 16 threads of the 7800X3D, and reads back at
  about 25 GB/s: with every pop read back it is still 2.8 times faster. The RTX 4050
  reads back at about 11 GB/s, 1.4 times faster than its 12 CPU threads in all. The RTX
  3070 Laptop computes faster than the RTX 4050 (0.39 ms against 0.53 ms) but reads
  back at about 5.5 GB/s, so in all it takes 6.2 ms, still 1.3 times faster than its 12
  CPU threads.
- **Integrated GPUs differ widely.** The Radeon 760M (8 compute units) shares the
  system's memory, reads back cheaply, and is twice as fast as its 12 CPU threads. The
  7800X3D's 2-compute-unit Radeon only matches its CPU. The 2016 HD Graphics 620
  computes a tick 1.3 times faster than all four CPU threads, and with every pop read
  back the two are even. The Steam Deck's GPU is 2.7 times faster than its 8 CPU threads
  with every pop read back, the largest lead of an integrated GPU here: its CPU is small,
  and reading back from shared memory costs it about 1 ms.
- **lavapipe** computes 6 to 11 times slower than the C++ on the same threads.
- **SMT helps unevenly.** From 8 to 12 threads the 5600H gains almost nothing, the
  7640HS about 30 %.

With `--readback ui` (only province unrest and empire treasuries come back), total ms
against compute ms:

| `medium`, `--readback ui` | Total ms | Compute ms |
| --- | ---: | ---: |
| RTX 4070 Ti | 0.18 | 0.14 |
| RTX 3070 Laptop | 0.57 | 0.42 |
| RTX 4050 Laptop | 0.65 | 0.58 |
| Radeon 760M | 1.87 | 1.75 |
| Ryzen 7800X3D integrated Radeon | 3.91 | 3.83 |
| Steam Deck GPU | 2.86 | 2.59 |
| Intel HD Graphics 620 | 17.6 | 17.2 |

The larger sizes on the RTX 4050 laptop, total ms with every pop read back, and compute
ms:

| Size (read back per tick) | RTX 4050 Laptop | Radeon 760M | lavapipe, 12 threads | CPU, 1 thread | CPU, 12 threads |
| --- | ---: | ---: | ---: | ---: | ---: |
| `large` (183 MiB) | 19.8 (3.35) | 14.2 (8.57) | 147 (133) | 190 | 30.8 |
| `huge` (305 MiB) | 30.7 (5.66) | 23.2 (14.1) | 242 (217) | 324 | 51.0 |
| `giant` (1.5 GiB) | 151 (26.3) | 109 (65.5) | skipped | 1,567 | 248 |
| `colossal` (3.0 GiB) | 297 (52.4) | 217 (131) | skipped | 3,159 | 483 |

From `large` up, a tick costs about the same per pop. Compute alone, the RTX 4050 is 50
to 60 times faster than one CPU thread and about 9 times faster than twelve. Reading
every pop back takes five times as long as the compute; even so it stays about 1.6
times faster than twelve threads at every size, and the Radeon 760M, which shares the
system's memory, is faster still.

On the desktop, `colossal` (100M pops), every run in the same state as on the other
machines (hash `efd84db0206e40e3`):

| Desktop, `colossal` | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4070 Ti | 148 | 22.1 | 20x |
| Ryzen 7800X3D integrated Radeon | 381 | 291 | 7.8x |
| lavapipe | skipped | | |
| CPU reference, 1 thread | 2,973 | | 1x |
| CPU reference, 8 threads | 395 | | 7.5x |
| CPU reference, 16 threads | 321 | | 9.3x |

The RTX 4070 Ti stays twice as fast as all 16 CPU threads with 3 GiB coming back every
tick. With `--readback ui` it takes 22.5 ms, 14 times faster than the 16 threads.

`colossal` with `--readback ui` on the other GPUs, in the same state: the RTX 3070 Laptop
takes 25.4 ms per tick (24.8 ms compute), the RTX 4050 53.0 ms, the Radeon 760M 133 ms
and the Steam Deck 161 ms. The HD Graphics 620
takes 688 ms, after a 42-second start; with every pop read back it is skipped, because
the 3 GiB of state and two 3 GiB readback buffers do not fit in the memory it shares
with the CPU (`vkAllocateMemory` fails).

## Sending in results

Results from other hardware are welcome, above all from GPUs not listed here. Run
`terni_bench --json results.json` (the `medium` world; other sizes too if you have the
time), then open an issue with the
[benchmark result template](https://github.com/ZBager/Terni/issues/new?template=benchmark-result.md)
and attach the JSON file. Besides the results, it holds the CPU and GPU names, the GPU
drivers, core counts, the operating system and its kernel version, the compiler and the
Terni commit, and nothing else about the machine.
