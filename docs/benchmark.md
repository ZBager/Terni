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
./build/terni_bench                    # the medium world, every device, 1 to all CPU threads
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

The sizes:

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
reforms) and reads two fields back, as a game's interface would.

The world has a version, printed as `terni_bench 1`. When the world, its rules or the
host's events change, the version goes up. Compare results only between runs with the
same version and size.

## The CPU reference

`bench/cpu_reference.cpp` runs the same eight stages in plain C++. It is written the way
a careful CPU engine would be:

- flat arrays in the same layout as Terni's, with no allocation per entity;
- the same fixed-point and random functions (`terni::fx`, `terni::rng`);
- a pool of threads created once, with each stage's rows split statically between them;
- sums that many rows add to one value kept per thread and combined after the stage,
  where Terni uses atomics.

After every tick its state is bit-identical to Terni's.

It is a fair baseline, not the fastest CPU code possible:

- It runs the same stages in the same order and does not fuse loops.
- It has no hand-written SIMD.
- It is compiled for the baseline instruction set (no `-march=native`), as a game
  shipped to players would be.

Loops too small to be worth waking threads for (a few hundred rows) run on fewer
threads, or on the calling thread alone.

## Reading the results

```
                                                 total ms           p10 - p90  compute ms  vs 1 CPU  start ms
NVIDIA GeForce RTX 4050 Laptop GPU                  3.753       3.594 - 4.224       3.393     50.6x       ...
```

| Column | Meaning |
| --- | --- |
| total ms | Median time of a tick as the host sees it: compute, readback to system memory, and submitting and waiting (with the host's events) |
| p10 - p90 | The spread of total ms: 10 % of ticks were faster than the first number, 10 % slower than the second |
| compute ms | Median time of the stages alone, from GPU timestamps. The CPU reference has nothing to read back, so for it compute and total are the same |
| vs 1 CPU | How many times faster than the CPU reference on one thread, by total ms |
| start ms | Creating the simulation, with the rules loaded from a SPIR-V cache (the compile time, done once, is printed above the table) |

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
  the host's events. By default only what a game's map and interface would read comes
  back every tick: province unrest and empire treasuries, a few hundred KiB at most.
  Games that start a tick and collect it a frame later (`begin_tick()` / `end_tick()`)
  hide most of the difference.
- **Thread counts past the physical cores** use SMT (Hyper-Threading), which adds less
  than a core. The header shows physical cores and threads.

## Reference results

`terni_bench 1`, 20 warm-up and 100 measured ticks. A laptop with a Ryzen 5 7640HS (6 cores,
12 threads), a Radeon 760M integrated GPU and an RTX 4050 Laptop GPU; Linux, GCC 16,
NVIDIA driver 620.70, Mesa 26.2 (RADV and lavapipe).

`medium` (1,000,000 pops):

| Run | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4050 Laptop | 0.65 | 0.58 | 48x |
| Radeon 760M | 1.87 | 1.75 | 17x |
| lavapipe, 12 threads | 38.4 | 38.2 | 0.8x |
| CPU reference, 1 thread | 31.4 | | 1x |
| CPU reference, 4 threads | 8.17 | | 3.8x |
| CPU reference, 8 threads | 6.39 | | 4.9x |
| CPU reference, 12 threads | 4.76 | | 6.6x |

`large` (6,000,000 pops):

| Run | Total ms | Compute ms | vs 1 CPU thread |
| --- | ---: | ---: | ---: |
| RTX 4050 Laptop | 3.75 | 3.39 | 51x |
| Radeon 760M | 8.99 | 8.55 | 21x |
| lavapipe, 12 threads | 137 | 137 | 1.4x |
| CPU reference, 1 thread | 190 | | 1x |
| CPU reference, 12 threads | 30.8 | | 6.2x |

The largest sizes, total ms per tick:

| Run | `huge` (10M pops) | `giant` (50M) | `colossal` (100M) |
| --- | ---: | ---: | ---: |
| RTX 4050 Laptop | 6.27 | 27.0 | 53.0 |
| Radeon 760M | 14.9 | 67.7 | 133 |
| lavapipe, 12 threads | 225 | skipped | skipped |
| CPU reference, 1 thread | 324 | 1,567 | 3,159 |
| CPU reference, 12 threads | 51.0 | 248 | 483 |

On the GPUs, a tick costs about the same per pop from `large` up, and the RTX 4050 runs
50 to 60 times faster than one CPU thread and about 9 times faster than all twelve.

## Sending in results

Results from other hardware are welcome, above all from GPUs not listed here. Run
`terni_bench --size large --json results.json` (and `--size medium` if `large` does not
fit in the GPU's memory; `huge`, `giant` and `colossal` too if you have the time), then
open an issue with the
[benchmark result template](https://github.com/ZBager/Terni/issues/new?template=benchmark-result.md)
and attach the JSON file. The file holds the CPU and GPU names, core counts, operating
system, compiler and Terni commit, and nothing else about the machine.
