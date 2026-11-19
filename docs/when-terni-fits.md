# Is Terni right for your game?

Terni runs a simulation's tick as compute shaders, with the same result on every GPU and
CPU. That pays off for some games and is needless weight for others. This page helps
you tell which yours is, with numbers from the [benchmark](benchmark.md).

## In short

Terni fits when most of these hold:

- **Many similar things change every tick**: hundreds of thousands to millions of pops,
  cells, units or provinces, each following the same rules.
- **Every machine must agree**: lockstep multiplayer, replays, or saves that continue
  exactly as they would have. Terni gives that on any GPU, with tools to find out why
  two machines disagree.
- **Mods change the rules**, not only the numbers, and you want them checked and
  deterministic like your own.
- **Windows or Linux** with a Vulkan 1.3 GPU, or Mesa's lavapipe as a fallback on the
  CPU. macOS, consoles and the web are not supported.

It does not fit when:

- **The world is small**: a few thousand things, or hundreds of thousands that change
  rarely. Plain code on the CPU is simpler and as fast or faster.
- **The work is irregular**: pathfinding, planning, scripted events, decisions with many
  branches. Those stay on the CPU (see [below](#what-stays-on-the-cpu)).
- **The game is GPU-bound already**, and a millisecond of the GPU's time each tick is
  too much to give.

## Small worlds belong on the CPU

A tick on a GPU costs a submission and a wait, a few hundredths of a millisecond,
however little work it does. The benchmark world on a laptop (RTX 4050, 12 CPU
threads), total time per tick with every pop read back:

| Pops | RTX 4050 | CPU, 1 thread | CPU, 12 threads |
| ---: | ---: | ---: | ---: |
| 1,024 | 0.065 ms | 0.030 ms | 0.030 ms |
| 50,000 | 0.25 ms | 1.5 ms | 0.24 ms |
| 1,000,000 | 3.4 ms | 32 ms | 4.9 ms |
| 10,000,000 | 31 ms | 329 ms | 52 ms |

With a thousand pops the CPU is twice as fast. At 50,000 the GPU and all 12 CPU threads
are even. From a million on, the GPU leads, and by far more when only what the
interface shows is read back (below). On the desktop's RTX 4070 Ti the GPU already
leads at 50,000 pops (0.14 ms against 0.26 ms on 16 threads).

These numbers are for rules like the benchmark's: a few stages of arithmetic per row.
Measure your own with `terni_bench`, or by writing one subsystem in Terni and timing it.

## Group first

The biggest saving is often not the hardware but the data model. If ten million
people can be ten thousand groups (by culture, job and place) without losing what the
game is about, the CPU handles them easily. Measured while Terni was designed
([Design](design.md#what-the-first-experiments-measured)): groups of up to 1,000 people
made a 50-million-person galaxy cost about 1 ms a month on one CPU thread, 150 to 370
times less than one row per person, and flat arrays instead of an object per person
were about 30 times faster, before any GPU was involved.

Terni fits when the detail is the point: individual pops that move, change and die, a
grid of cells, units that each have state.

## Read back only what you show

Terni keeps the state on the GPU. Each tick, the host reads back what it needs, and that
traffic often costs more than the computing:

| `medium` (1,000,000 pops), RTX 4050 laptop | Per tick |
| --- | ---: |
| The rules alone | 0.58 ms |
| Every pop read back (31 MiB) | 3.4 ms |
| Only what the map and interface show | 0.65 ms |

A discrete GPU reads back over PCIe; an integrated one shares the system's memory and
pays less. So subscribe with `read_each_tick` to the fields your interface shows, read
the rest when the player opens a panel, and start the tick at the end of one frame and
collect it in the next (`begin_tick()` / `end_tick()`), so the GPU works while you draw.
Next to a renderer, reading every pop back each tick also delays its frames
([Beside a renderer](benchmark.md#beside-a-renderer)).

## What stays on the CPU

The GPU is fast at the same steps over many rows. It is slow, or unsuited, for:

- **Pathfinding and movement planning**: a graph search per army or caravan.
- **AI decisions**: weighing options, planning a war, choosing a policy.
- **Events and scripts** that fire rarely and change a few things.
- **Diplomacy, interface logic, anything with many branches per entity.**

Do these on the CPU between ticks, as commands to the simulation: write fields
(`write_field`), add modifiers, create and destroy entities. The rules then do the
dense part: production, consumption, prices, growth, taxes, attrition, everything that
touches every row every tick. For example, an AI empire decides once a month to raise
its tax rate (CPU); every province then pays the new rate and every pop reacts to it
(Terni). In a lockstep game the commands are what peers send each other, and the host
logic stays deterministic if it uses integer math on values every peer reads back
([Embedding](embedding.md#host-side-logic)).

A game can also keep its own entity system for what the player touches (armies on the
map, characters, cities with buildings) and use Terni for the world underneath.

## What Terni asks of you

- **Rules in GLSL with fixed point**, not floats: `fx_mul`, `fx_div` and the rest
  ([Writing rules](writing-rules.md)). That is what makes results the same everywhere.
- **One rule about order**: a stage never reads a row another thread of the same stage
  writes, and sums use atomics. Terni warns when a rule breaks it.
- **Capacities set before the world starts**: entity types that come and go live within
  a capacity, which a game can set from its map before starting.
- **A second Vulkan device** next to your renderer, on the same GPU or another one.
- **An engine binding**: the [Flax and Godot plugins](https://github.com/ZBager/TerniIntegrations), the C++ API, or the
  [C API](c-api.md) and its C# wrapper.

## If you are not sure

Run `terni_bench` on the machines you target, then move one subsystem, such as the
economy or the pops, into Terni while the rest of the game stays as it is. If the
subsystem is small, rarely changes, or must run where Vulkan does not, keep it on the
CPU.
