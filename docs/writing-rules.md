# Writing rules

A rule is the body of one stage: GLSL 4.60 with a `void main()`. Terni puts a
generated prelude in front of it, compiles it with shaderc and runs it once per tick
over the rows of the stage's `for_each` entity.

```glsl
// rules/planet_growth.glsl
void main() {
    FOR_EACH_PLANET(p) {
        int pop = planet_pop[p];
        int growth = D.growth_rate + planet_mod_pop_growth(p);   // base + modifiers
        planet_pop[p] = pop + fx_mul(pop, growth);
    }
}
```

## What a rule can use

| Name | What it is |
| --- | --- |
| `<entity>_<field>[i]` | A field; row `i`, or `i * SIZE + k` for sized fields |
| `<entity>_<field>(i)`, `<entity>_<field>_set(i, v)` | A [small field](#small-fields) (`int8` to `ref16`): read and write |
| `FOR_EACH_<ENTITY>(i) { }` | Loop over the rows in use of an entity type |
| `FOR_EACH_<ENTITY>_GROUP(i) { }` | The same in `dispatch = workgroup` stages; 256 threads per row |
| `<ENTITY>_CAPACITY`, `<ENTITY>_<FIELD>_SIZE` | Capacity (see below); values per row |
| `terni.tick`, `terni.<entity>_count` | Completed ticks; rows in use |
| `D.<define>` | Constants from `common/` |
| `<entity>_mod_<key>(i)` | Modifier total of row `i`, own plus inherited. For a key that inherits in several steps, every entity type on the way has one, and it counts from that level up |
| `<entity>_alive(i)`, `<entity>_destroy(i)`, `<entity>_generation[i]` | Dynamic entity types only; see [Entities that come and go](#entities-that-come-and-go) |
| `workgroup_sync()` | Barrier for shared memory, in workgroup stages |
| The GLSL library below | Fixed point, wide integers, randomness |

A stage may loop over another entity type than its `for_each` (the loop handles any
count), and a rule may define helper functions and `const` values above `main`.

`<ENTITY>_CAPACITY` is a specialization constant: its value is set when the world
starts, so a game can change capacities without compiling its rules again. Use it in
code, in `const` expressions and as an array size, but not in `#if`, which runs before
the value is known.

## Fixed point

Fields are integers that stand for fractions (see [Concepts](concepts.md#fields)).
Add and subtract them normally; multiply and divide with the library:

| Function | Result |
| --- | --- |
| `fx_mul(x, r)` | `x × r` where `r` is a rate; keeps the format of `x` |
| `fx_div(a, b)` | `a / b` as a rate, exactly; `a ≥ 0`, `b > 0`, both in the same format |
| `DIV_DEFINE(a, name)` | Exactly `a / D.name` for an `int` (or `a / uint(D.name)` for a `uint`), without dividing. Legacy, see below |
| `fx_min`, `fx_max`, `fx_clamp` | As named |
| `to_agg(q)` | A quantity in whole units (rounded down), for sums |
| `qty_from_count(n)` | A count as a quantity |
| `FX_ONE`, `QF`, `RF` | 1.0 as a rate; fraction bits of quantities (8) and rates (16) |

Rules of thumb:

- Never multiply two quantities together; scale a quantity by a rate.
- `fx_mul(pop, FX_ONE + bonus)` applies a percentage bonus.
- Keep intermediate values in range: a quantity holds about ±8.4 million.
- `DIV_DEFINE(a, name)` is legacy: it keeps working, but write `a / D.name` in new rules.
  It multiplies by a reciprocal that Terni works out when the world starts. With one
  division per row GPUs gain little or nothing; a stage of four divisions per row took
  13% less time with `int` values and 30% less with `uint` on a laptop RTX 4050, 5% and 13%
  less on a Radeon 760M, and about a third of the time on a software device. The define must
  not be 0, and for `uint` values it must be above 0.

## Small fields

A field of a small type (`int8`, `int16`, `count8`, `count16`, `ref8`, `ref16`) is read and
written with functions instead of `[ ]`:

```glsl
void main() {
    FOR_EACH_POP(i) {
        const uint p = pop_home(i);                       // ref16:province
        if (pop_mood(i) < -50) pop_job_set(i, 0u);        // int8; count8
        pop_mood_set(i, pop_mood(i) + province_calm[p]);  // computed in 32 bits, stored in 8
    }
}
```

- Reading gives an `int` (`int8`, `int16`) or a `uint` (the others), so nothing overflows
  while you compute. For a sized field the index is `i * SIZE + k`, as usual.
- Writing stores the nearest value that fits: 300 in a `count8` becomes 255, -200 in an
  `int8` becomes -128. A `count` below 0 is a very large `uint`, so it becomes the largest
  value, not 0. `tick_checked()` reports such writes.
- A small ref that points at no row reads `0xFFFFFFFFu`. Writing that, or any row that does
  not fit, stores "no row", so a ref never ends up at a wrong row. A fresh ref is 0 (the
  first row), as in any ref field.
- A write past the last row does nothing, and a read there gives 0, on every device.
- Atomics do not work on small fields: the rule does not compile.
- Threads may write neighbouring rows of a small field at the same time, as of any field.
- Changing a field between 32 bits and a small type means changing every rule that uses it,
  those in mods too.

What they gain depends on the GPU and on how much of the memory a stage touches becomes
smaller. The GPU time of a tick of the benchmark's world, with two of its eight pop fields
small (a `ref16` and a `count8`) and with four, against 32-bit ones:

| | `medium`, two | `medium`, four | `large`, two | `large`, four |
| --- | ---: | ---: | ---: | ---: |
| RTX 4050 Laptop | -16% | -27% | -13% | -19% |
| RTX 4070 Ti | 0% | 0% | -11% | -19% |
| Radeon 760M | -6% | -10% | -8% | -13% |
| 7800X3D integrated Radeon | -2% | -2% | -2% | -2% |

A tick with every pop read back gains more, since there is less to copy: with four small
fields it took 23 to 30% less time on the RTX GPUs, 9 to 18% less on the Radeon 760M and 8%
less on the 7800X3D's. On a software device (lavapipe) it did not change, and a world made
mostly of small values ran 10% slower there. Devices that widen small fields take as long as
with 32-bit ones.

## 64- and 128-bit integers

`money64` fields are `uvec2` (low word, high word). `terni/wide.glsl` (included
automatically) gives exact arithmetic on them from 32-bit operations, so it runs on
every Vulkan device:

`add_i64` (also `add_i64(a, int)`), `sub_i64`, `neg_i64`, `abs_i64`, `min_i64`,
`max_i64`, `lt_i64`, `le_i64`, `eq_i64`, `lt_u64`, `is_neg_i64`, `i64_from_int`,
`mul_i64_q16` (i64 × rate), `mul_i32_i64` and `mul_i64_i64` (to i128), `mad_i128`,
`add_i128`, `neg_i128`, `shr_i128_to_i64`, `div_u128_u64`, `div_i128_i64`, `muldiv_i64`
(a × b / c), `div_q16_i64`.

```glsl
empire_treasury[e] = add_i64(empire_treasury[e], empire_income[e]);
```

## Randomness

Randomness is a hash, not a generator with state, so every machine rolls the same:

```glsl
if (rng_below(rng_roll(p, terni.tick, 1u), D.unrest_chance)) { ... }   // true with probability unrest_chance
```

`rng_roll(entity, tick, salt)` gives 32 random bits; use a different `salt` for each
independent roll in the same tick. `rng_below(roll, p)` is true with probability `p`
(a rate from 0 to 1).

## Sums over many entities

Many threads adding to one row (a realm's income from its provinces) must use integer
atomics, which give the same total in any order:

```glsl
// rules/empire_income.glsl, with clear = { empire.income } in stages.txt
void main() {
    FOR_EACH_PLANET(p) {
        atomicAdd(empire_income[planet_owner[p]], to_agg(planet_tax[p]));
    }
}
```

Convert to whole units (`agg` fields, `to_agg`) before adding, list the field in
`clear`, and read the total in a **later** stage, never in the stage that adds to it.

Work that happens once a month belongs in a stage with a schedule
(`every = 30 offset = 29` in `stages.txt`) rather than in an `if` on `terni.tick`: on the
other days the stage is not dispatched at all. Fields it lists in `clear` are zeroed
only when it runs, so last month's total stays readable all month. To zero a field on a
schedule without a stage (a month's total that daily rules add to), give it a `clear` line
of its own in `stages.txt` ([Data format](data-format.md#stagestxt)).

When thousands of threads add to a few rows, `terni/slot_table.glsl` pre-sums them
per workgroup in shared memory first. Define `TABLE_SLOTS` and `VALUES` and include it
in the rule:

```glsl
#define TABLE_SLOTS 64
#define VALUES 2
#include "terni/slot_table.glsl"
```

## Entities that come and go

For an entity type marked `dynamic = yes`, `FOR_EACH_ARMY(a)` visits only the armies
that are alive, and three more names exist:

| Name | Meaning |
| --- | --- |
| `army_alive(i)` | True if row `i` is alive. False for any index that is not a row, so it is safe on a stale ref |
| `army_destroy(i)` | Row `i` is no longer alive, from this moment |
| `army_generation[i]` | How many times row `i` has been created (0: never) |

**Destroying.** A row normally destroys itself:

```glsl
FOR_EACH_ARMY(a) {
    army_strength[a] -= army_losses[a];
    if (army_strength[a] <= 0) army_destroy(a);
}
```

The fields of a destroyed row keep their values until the row is created again.
Destroying *another* army from a stage that loops over armies breaks the
[rule about order](#the-one-rule-about-order): that army's own thread may or may not
have started. Do it from a stage over another entity type (a battle destroys its
armies), or set a field and let the army destroy itself.

**Creating.** Rules cannot create a row on the spot, because two threads would race for
the same one. They ask instead, through a `count` field named in the schema:

```
army = {
    capacity = 4096
    dynamic  = yes
    spawn    = { from = province  count = raise  parent = home }
    home     = ref:province
    fresh    = { type = int  default = 1 }
}
```

```glsl
FOR_EACH_PROVINCE(p) {
    if (province_unrest[p] > D.revolt_at) province_raise[p] += 1u;   // ask for one army
}
```

At the end of the tick, each province gets as many new armies as its `raise` says, in
row order, and `raise` goes back to 0. This happens on the device, in built-in stages
after yours, so it costs no trip to the host however many rows ask. A new army has every field at its default and, if
`parent` is given, that field set to the province that asked. It is alive from the next
tick, so give it a field such as `fresh` with a default of 1 and set it up there:

```glsl
FOR_EACH_ARMY(a) {
    if (army_fresh[a] != 0) {
        army_fresh[a] = 0;
        army_strength[a] = province_manpower[army_home[a]];
    }
    ...
}
```

If the entity type is full, the armies that do not fit are not created and `raise`
keeps the number still wanted; they are created when rows come free, unless a rule
sets `raise` back to 0.

**Telling new rows apart.** All the rows a province gets in one tick have the same
parent. When each should be something different (a pop of a given culture, an army of
a given kind), name an `ordinal` field in the `spawn` line: the rows a province gets in
a tick are numbered 0, 1, 2, ... The province keeps a list of what it asked for, and each
new row reads its entry:

```
pop = {
    dynamic = yes
    spawn   = { from = province  count = new_pops  parent = home  ordinal = nth }
    home    = ref:province
    nth     = int
    culture = { type = int  default = -1 }
}
province = {
    new_pops = count
    wanted   = { type = int  size = 4 }   # the culture of each pop asked for
}
```

```glsl
FOR_EACH_POP(i) {
    if (pop_culture[i] < 0) pop_culture[i] = province_wanted[pop_home[i] * 4u + uint(pop_nth[i])];
}
```

If the type is full and a province gets only some of the rows it asked for, the next
rows it gets start again at 0, and `new_pops` holds how many are still missing; drop
the entries already served before the next tick.

**Rows asked for once a month.** `every = 30  offset = 29` in the `spawn` line gives rows
out only at the end of every 30th tick, like a stage with that schedule; the count
fields keep what was asked until then. On the other ticks only a short pass over the
alive bits runs, so a type that changes monthly costs almost nothing in between.

**Refs to entities that may be gone.** A destroyed row is not used again in the tick it
was destroyed or the one after, so a rule that checks `army_alive(r)` every tick always
sees an army go before another takes its row. A ref that is checked less often can
miss that: compare `army_generation[r]` with a generation you stored next to the ref to
know it is still the same army.

**Cost.** In ticks a `spawn` line runs in, the built-in stages pass over every row of
the dynamic entity type and of the types that ask, whether anything is asked or not:
about as much work as three to five simple stages of your own over those rows. In
other ticks one pass over the alive bits (one word per 32 rows) is all, so give lines
that need not run every tick a schedule.

## Workgroup stages

With `dispatch = workgroup`, each row gets a whole workgroup of 256 threads.
`gl_LocalInvocationID.x` is the thread within it; `shared` arrays and
`workgroup_sync()` let the threads combine their work, in a fixed order so the result
is exact:

```glsl
shared uvec2 part[256];
void main() {
    FOR_EACH_PROVINCE_GROUP(p) {
        const uint t = gl_LocalInvocationID.x;
        uvec2 s = uvec2(0u);
        for (uint k = t; k < PROVINCE_POP_SIZE; k += 256u) s = add_i64(s, i64_from_int(province_pop[p * PROVINCE_POP_SIZE + k]));
        part[t] = s;
        workgroup_sync();
        for (uint step = 128u; step > 0u; step >>= 1u) {   // a fixed tree: the same order every time
            if (t < step) part[t] = add_i64(part[t], part[t + step]);
            workgroup_sync();
        }
        if (t == 0u) province_total[p] = part[0];
        workgroup_sync();
    }
}
```

Every thread of the workgroup must reach every `workgroup_sync()`.

## The one rule about order

Threads of a stage run in no fixed order. **A thread may read and write its own row
freely, and read any row of a field that no thread of this stage writes.** It must not
read a row that another thread of the same stage may be writing, or write such a row
without an atomic: the result would depend on which thread ran first, and could
differ between GPUs. Compute such values in one stage and read them in the next.

Terni warns about these reads when the world loads, and `GameSim::tick_checked()`
confirms them at run time. A line that is safe by design (each thread writes a
different row of another entity, say) can be marked `// terni: order ok`. See
[Determinism](determinism.md).

## Making rules fast

Measure before and after with `stage_times()`; what helps depends on the GPU. The numbers
here are from a laptop RTX 4050 and an integrated Radeon 760M.

- **Write what the host reads in the order the host wants it.** In a grid simulation,
  rules that put the results into the host's order, so that the host no longer had to
  reorder them, took a tick from 0.124 to 0.104 ms (RTX) and from 0.513 to 0.497 ms (Radeon).
- **On integrated GPUs, keep rows that are used together close together.** Giving buildings
  on a map of 2048 x 2048 cells row numbers in Morton order (neighbours on the map are
  neighbours in memory), not random ones, took a tick from 2.7 to 1.5 ms on the Radeon. It
  made no difference on the RTX.
- **Do not merge two different loops into one stage to save a dispatch.** A loop over 24,576
  rows with many live values and one over 1.5 million rows with few took 0.134 ms in one stage
  and 0.120 ms in two on the RTX, and the same on the Radeon.

## When a rule goes wrong

Rules from mods run in every player's game, so Terni keeps the damage a broken rule can
do small and the same on every machine:

- **Reading or writing past the end of a field** (`province_stock[p * GOODS + g]` with
  a good too many, a ref that points nowhere) reads 0 and writes nothing, atomics
  included. Every current desktop driver promises this (Vulkan's robustBufferAccess2,
  which Terni turns on), so the rule gives the same wrong result on every GPU and in a
  lockstep game nobody desyncs. Nothing outside the field is touched.
- **A loop that never ends**, or a stage that runs for seconds, makes the GPU driver
  reset the GPU. The game gets a `terni::DeviceLost` error and starts again from its
  last save. Keep loops bounded by a count you know, such as a capacity or a define.
- **A rule can change any field of the world.** It is part of the game: test a mod as
  you would test the game.

Before publishing a mod, run `terni_check <game folder> <mod folder>`. It loads the game
with the mod as players will, compiles the rules, runs them on every device and checks
the first ticks for order problems, and says `ready` or `NOT READY` with every error and
warning at its `file:line`. Treat a warning about order as an error: it means the mod
can desync a multiplayer game. See [Data format](data-format.md#mods) for how mod
folders replace and add files.

## Debugging

- `terni_run <folder> --prelude` prints the generated prelude.
- A compile error names the rule file and line: `rules/planet_growth.glsl:7: error: ...`.
- `terni_run <folder> --ticks N --dump` prints every field after N ticks, on every device.
- `terni_check <game folder> [mod folder]` loads, compiles and runs the rules on every
  device, and checks the first ticks for order problems.
- `terni_diff a.sav b.sav` names the fields and rows where two saves differ.
- `GameSimOptions::validation = true` turns on the Vulkan validation layers.
