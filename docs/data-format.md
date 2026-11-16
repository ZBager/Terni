# Data format

A game folder holds four subfolders. `load_game(root)` reads every `.txt` file in
each one, in file-name order, in the Paradox script syntax (`key = value`,
`key = { ... }`, `#` comments). Problems never throw: they are collected in
`game->errors` and `game->warnings` as `file:line: message`.

```
my_game/
  schema/      what exists: entity types, fields, constants, modifier keys
  common/      values: constants, and modifier bundles in common/modifiers/
  history/     the starting state: one block per entity
  rules/       stages.txt (the tick order) and one <stage>.glsl per stage
```

Everything a mod may change is in these folders. A mod is a copy of the folder with
some files changed or added.

## Names

Entity, field, define, modifier key, bundle and stage names become GLSL identifiers:
ASCII letters, digits and `_`, not starting with a digit, `gl_` or `terni`.

## schema/

### Entity types

```
planet = {
    capacity      = 4096                          # most rows this type can hold (Game::set_capacity changes it)
    owner         = ref:empire                    # field = type
    pop           = quantity
    stability     = { type = rate default = 50 }  # a starting value for every row
    stock         = { type = agg size = 3 }       # three values per row
    unrest_events = count
}
```

| Field type | Value | Notes |
| --- | --- | --- |
| `quantity` | Q24.8 | about ±8.4 million with 1/256 steps |
| `rate` | Q16.16 | about ±32,768 with 1/65,536 steps |
| `agg` | 32-bit integer | whole units; sums over many entities |
| `int` | 32-bit integer | |
| `count` | 32-bit unsigned | |
| `ref:<entity>` | row index | written as the target's `id` in history |
| `money64` | 64-bit integer | two words on the GPU |

`size = N` gives each row N values. `default` is written in the field's own units
(`default = 50` on a rate means 50.0).

### Dynamic entity types

```
army = {
    capacity = 4096
    dynamic  = yes                                               # rows are created and destroyed during play
    spawn    = { from = province  count = raise  parent = home } # provinces ask for armies (optional, repeatable)
    home     = ref:province
    strength = { type = int  default = 100 }
}
```

- `dynamic = yes` gives rules `army_alive(i)`, `army_destroy(i)` and
  `army_generation[i]`, and makes loops skip rows that are not alive. The fields may
  not be named `alive`, `alive_bits`, `destroy` or `generation`, and the capacity must
  be under 16,777,216.
- `spawn` lets rules ask for new rows. `from` is the entity type that asks, `count` one
  of its `count` fields (how many rows a row wants), and `parent`, if given, a
  `ref:<from>` field of the new rows that is set to the row that asked. Several
  `spawn` blocks are served in the order written.
- History creates the starting rows as for any entity type; a new row gets each
  field's `default`.

See [Writing rules](writing-rules.md#entities-that-come-and-go) for how rules use them.

### Constants

```
defines = {
    growth_rate = rate
    max_armies  = int
}
```

Give every declared define a value in `common/`; one without a value is 0, with a
warning at its line in the schema.

### Modifier keys

```
modifier_keys = {
    pop_growth     = { applies_to = planet  type = rate  inherit = owner }
    tax_efficiency = { applies_to = planet  type = rate }
    research       = { applies_to = empire  type = rate }
    unrest         = { applies_to = province  type = rate  inherit = { district region realm } }
}
modifiers = { capacity = 65536 }   # most modifier instances alive at once (optional)
```

`type` is `rate`, `quantity` or `int`. `inherit` names a single-valued `ref` field of
the key's entity that points to another entity type; the entity's total is then its
own plus that row's.

A list inherits in several steps, nearest first. With `inherit = { district region
realm }`, the first name is a field of the province, the second a field of the district
and the third a field of the region. A province's total is its own, plus its
district's, plus that district's region's, plus that region's realm's. Each entity type
can appear once in the list. A ref that points at no row ends the chain there.

A bundle value for a key that does not apply to an entity is
ignored on that entity, as in Paradox games.

## common/

Values of the constants, one per line:

```
growth_rate = 0.02     # 2 % per tick
max_armies  = 512
```

Decimal text is converted to fixed point with integer math only (truncating toward
zero), so every machine reads the same bits. `yes`/`no` are 1/0 for integer types.

### common/modifiers/

Bundles: named sets of key values.

```
good_harvest = { pop_growth = 0.01 }
war_economy  = { pop_growth = -0.005  tax_efficiency = 0.25 }
```

## history/

The starting state: one block per entity, in file order. `id` names the row so that
other entities can refer to it; refs are resolved across all history files.

```
empire = { id = ROM  treasury = 1000  stock = { 100 50 20 } }
planet = { id = latium  owner = ROM  housing = 12000  pop = 8200.5
           add_modifier = { name = good_harvest  duration = 24  scale = 1.5 } }
```

- A sized field takes `{ v1 v2 ... }` with exactly `size` values.
- `add_modifier` applies a bundle at the start: `name`, optional `duration` (ticks)
  and `scale` (default 1).
- Rows in use (the count) are the rows created here. Games that generate their world
  in code set rows with `WorldBuilder` or by filling `Game::tables` before starting.

## rules/

### stages.txt

The tick order, one line per stage:

```
planet_growth   = { for_each = planet }
empire_income   = { for_each = planet  clear = { empire.income } }
empire_treasury = { for_each = empire }
province_sums   = { for_each = province  dispatch = workgroup }
monthly_taxes   = { for_each = province  every = 30  offset = 29  clear = { realm.tax } }
```

| Key | Meaning |
| --- | --- |
| `for_each` | The entity type whose rows the stage visits; sets the number of GPU threads |
| `clear` | Fields set to zero at the start of every tick in which the stage runs, before the first stage (accumulators) |
| `dispatch` | `thread` (default, one thread per row) or `workgroup` (256 threads per row) |
| `every` | Run the stage only every N ticks (default 1, every tick) |
| `offset` | With `every`: which of those ticks, 0 to N-1 (default 0) |

A stage with `every = 30 offset = 29` runs in the ticks where `terni.tick` (the ticks
completed before it) is 29, 59, 89, and so on: the last day of each 30-day month. In the
other ticks it is not dispatched at all, so it costs nothing. Its `clear` fields are
zeroed only in the ticks it runs, so a monthly total stays readable, by the host and by
other stages, until the next month replaces it.

### &lt;stage&gt;.glsl

The body of the stage: GLSL 4.60 with a `void main()`, using the names Terni
generates. See [Writing rules](writing-rules.md).

```glsl
void main() {
    FOR_EACH_PLANET(p) {
        planet_pop[p] += fx_mul(planet_pop[p], D.growth_rate + planet_mod_pop_growth(p));
    }
}
```

## What the checksum covers

`game_checksum()` covers the schema (with capacities, including those changed with
`Game::set_capacity`), the define values, modifier keys and bundles, the modifier
capacity, and the stages with their sources, `clear` lists, dispatch mode and schedule
(`every`, `offset`). It does not cover `history/`. Two peers with different rule files,
or a save made with different rules, are refused.

Line endings do not count: a rule file with Windows line endings (CRLF) is read as if it
had Unix ones (LF), so a game folder checked out on Windows has the same checksum as on
Linux.
