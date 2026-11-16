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
| `FOR_EACH_<ENTITY>(i) { }` | Loop over the rows in use of an entity type |
| `FOR_EACH_<ENTITY>_GROUP(i) { }` | The same in `dispatch = workgroup` stages; 256 threads per row |
| `<ENTITY>_CAPACITY`, `<ENTITY>_<FIELD>_SIZE` | Capacity (see below); values per row |
| `terni.tick`, `terni.<entity>_count` | Completed ticks; rows in use |
| `D.<define>` | Constants from `common/` |
| `<entity>_mod_<key>(i)` | Modifier total of row `i`, own plus inherited. For a key that inherits in several steps, every entity type on the way has one, and it counts from that level up |
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
| `fx_min`, `fx_max`, `fx_clamp` | As named |
| `to_agg(q)` | A quantity in whole units (rounded down), for sums |
| `qty_from_count(n)` | A count as a quantity |
| `FX_ONE`, `QF`, `RF` | 1.0 as a rate; fraction bits of quantities (8) and rates (16) |

Rules of thumb:

- Never multiply two quantities together; scale a quantity by a rate.
- `fx_mul(pop, FX_ONE + bonus)` applies a percentage bonus.
- Keep intermediate values in range: a quantity holds about ±8.4 million.

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
only when it runs, so last month's total stays readable all month.

When thousands of threads add to a few rows, `terni/slot_table.glsl` pre-sums them
per workgroup in shared memory first. Define `TABLE_SLOTS` and `VALUES` and include it
in the rule:

```glsl
#define TABLE_SLOTS 64
#define VALUES 2
#include "terni/slot_table.glsl"
```

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

## Debugging

- `terni_run <folder> --prelude` prints the generated prelude.
- A compile error names the rule file and line: `rules/planet_growth.glsl:7: error: ...`.
- `terni_run <folder> --ticks N --dump` prints every field after N ticks, on every device.
- `GameSimOptions::validation = true` turns on the Vulkan validation layers.
