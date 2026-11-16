# Determinism

Terni's promise is that the same world, the same commands and the same number of
ticks give the same bits on every machine: NVIDIA, AMD, a CPU Vulkan driver, any
operating system. Lockstep multiplayer, replays and cross-device saves all rest on it.
Terni keeps its side of the promise; rules keep theirs by following the contract
below, and Terni checks the part that is easiest to get wrong.

## The contract

1. **No floating point in the simulation state.** Fields are integers and fixed point;
   use `fx_mul`, `fx_div` and the other library functions, which give identical bits on
   every device.
2. **Scale quantities by rates.** `fx_mul(x, rate)` keeps the format of `x`; never
   multiply two quantities together.
3. **Sum with integers.** Many threads adding to one row use `atomicAdd` on whole-unit
   (`agg`) values, which gives the same total in any order. Convert with `to_agg` before
   adding.
4. **Randomness only from `rng_roll(entity, tick, salt)`.** It is a hash of its inputs,
   the same everywhere.
5. **Order inside a stage does not matter.** A thread reads and writes its own row, and
   reads other rows only of fields that no thread of the same stage writes. Values one
   stage computes are read by the next.
6. **The host is deterministic too.** Logic that runs between ticks uses integer math on
   values every peer reads back identically, and applies commands in the same order
   everywhere.

Rule 5 is the subtle one. GPUs run the threads of a stage in no fixed order, and in
practice different GPUs often happen to run them in a similar order, so a rule that
breaks it can pass a cross-device test and still desync in a real match.

## Checking rule 5

### When the world loads

`load_game()` and `WorldBuilder::finish()` scan every rule and add a warning for each
field a stage writes while also:

- reading it at a row that is not the thread's own,
- writing a row that is not the thread's own without an atomic, or
- accessing it both through atomics and plainly.

```
rules/planet_growth.glsl:12: stage 'planet_growth' reads planet.pop[neighbour], a row
other threads may write in this stage (planet.pop is written on line 14): the result can
depend on the order the GPU runs the threads in, and differ between devices
(confirm with GameSim::tick_checked)
```

The thread's own row is the `FOR_EACH` variable, `v * size + k`, or a local value set
from those (`uint base = p * N;` then `field[base + k]`). The scan reads the text, so
it misses accesses hidden in macros and treats a helper function's parameters as other
rows. A line that is safe by design can be marked with the comment `terni: order ok`.

### At run time

```cpp
for (const terni::OrderIssue& issue : sim.tick_checked())
    log(issue.stage + ": " + issue.field + " differs in " + std::to_string(issue.rows) + " rows");
```

`tick_checked()` is a tick with the same result as `tick()`. Before the normal run,
every stage also runs twice from the same state, with its rows handled one at a time
(`FOR_EACH` and `FOR_EACH_GROUP` walk them in sequence), forwards and then backwards. A
field that ends up different between the two runs depends on order. The normal parallel
run is compared too, which catches lost updates from a non-atomic `+=`.

It needs no changes to the rules and never reports a false alarm, but it only finds
races that actually happen in the ticks it checks, and it is slow: every stage runs three
times and the whole state is read back after each run. Run it in tests, on small worlds,
for many ticks. The two checks complement each other: the warning finds a race before it
ever happens; the checked tick proves one.

Rows of a dynamic entity type are created by built-in stages at the end of the tick.
Threads cannot each grab a free row, since which thread wins would differ from run to
run. Instead the requests are numbered in a fixed order (entity types, their `spawn`
lines, the asking rows) with running totals, the free rows are numbered the same way,
and request number k takes free row number k: the result a single thread would get,
whatever the device. The host's `create_entity` follows the same rule between ticks.

## Testing parity

The most useful test for a game on Terni runs the same match on every Vulkan device in
the machine and compares the state:

```cpp
for (const terni::DeviceInfo& d : terni::VulkanContext::list_devices()) {
    if (!d.usable) continue;
    terni::GameSimOptions o;
    o.device = int(d.index);
    terni::GameSim sim(*game, o);
    for (int t = 0; t < 1000; ++t) { apply_scripted_commands(sim, t); sim.tick(); }
    print(d.name, terni::combine(terni::hash_fields(sim.snapshot())));
}
```

Add lavapipe (Mesa's CPU Vulkan driver) to any machine to get a second device for
free; it runs the same shaders and gives the same bits. Also save halfway, load the save
on another device, and check that the continued run ends with the same hash.
`terni_run <folder>` does the device comparison for a data folder.

## What Terni guarantees

- Fixed-point arithmetic in C++ (`terni/fixed.hpp`) and GLSL (`fixed.glsl`) gives the same
  bits; the tests compare them on every device.
- 64- and 128-bit helpers (`wide.glsl`) are exact on every device; the tests compare them
  with C++ `__int128` (with GCC and Clang, which have it).
- Decimal values in data files become fixed point through integer math only, not through
  the C library, so the locale and the platform do not matter.
- The generated GLSL is the same text on every machine and with every compiler (GCC,
  Clang, MSVC), and rule sources are read the same whatever their line endings, so
  checksums and SPIR-V cache keys match across platforms.
- Modifier totals are exact integer sums, and expiries happen in a fixed order.
- Saves round-trip byte for byte and continue bit-identically on any device.
