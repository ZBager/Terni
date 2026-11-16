# Design

Every major choice in Terni serves one requirement: the same inputs must give the
same bits on every machine, cheaply enough to run every tick of a lockstep game. This
page explains the choices and what they cost, then what the experiments behind them
measured.

## Decisions

| Decision | Why | What it costs |
| --- | --- | --- |
| 32-bit fixed point, no floats in the state | Floating-point results differ between GPUs, drivers and compilers; integer results do not. 32-bit was 1.4–3x faster than 64-bit in the experiments and needs no 64-bit GPU features, which many shader paths lack | Rules use `fx_mul`/`fx_div`; values past 32 bits need `money64` or the wide-integer library |
| Terni's own Vulkan device and GLSL | Engines' shader pipelines differ (HLSL compiled through glslang, for example, has no 64-bit integers); Terni's own device runs the same SPIR-V everywhere and stays engine-independent | A second Vulkan device in the process; the simulation does not share the renderer's queue |
| Worlds described as data, compiled to shaders at load time | Mods can change the rules, not only the numbers; the game checksum covers the rules, so peers must agree | A shader compiler at run time, or a cache prepared at build time (`Terni::runtime`) |
| Terni owns the state; the host sends commands and reads results | One authoritative state on the GPU. Copying a CPU engine's state to the GPU every tick was slower than 12 CPU threads in an experiment | Hosts keep small read-only views, refreshed once per tick |
| Modifiers stored on the host, decided by the host | Research, focuses and events are game logic that changes rarely; Terni only keeps exact totals and expiries | Every modifier change goes through the API |
| One pre-recorded command buffer per tick, batched transfers | A tick costs one GPU round trip, uploads and readbacks included | Writes and reads take effect at tick boundaries |
| Sums through integer atomics | Integer addition gives the same total in any order | Rules must not read rows other threads write in the same stage (checked) |
| Fixed capacity per entity type | Buffers and generated GLSL stay static; nothing is allocated during play | No entities created at run time; hosts use pools of rows |
| A standalone library with no engine code | It is meant to be embedded by any engine | Integrations live in the games that use it |

## What the experiments measured

Terni began as a proof of concept comparing CPU and GPU versions of the same models:

- **Data layout beats hardware.** Flat arrays with 12 CPU threads were about 30x faster
  than object-per-pop code, before any GPU was involved.
- **32-bit beats 64-bit.** 32-bit fixed point was 1.4–3x faster than 64-bit and needs no
  64-bit GPU features.
- **Pop groups are cheap; individual pops are where the GPU pays.** With pop groups
  (Stellaris-style, up to 1,000 people each), a 50-million-pop galaxy costs about 1 ms
  per month on one CPU thread. Individual pops cost 150–370x more; there an RTX 4050 was
  about 30x faster than one CPU thread and 6x faster than 12 threads (250 million pops:
  1.85 s against 60 ms per month).
- **Exact everywhere.** Every implementation gave identical bits on CPUs (1 to 12
  threads), NVIDIA and AMD, for every model and size tested.

## What real games taught it

Terni has since run games in development, from a small territory game to a grand
strategy game with 32,000 provinces and 6.5 million pop rows, with lockstep
multiplayer, saves and bots. A tick of 28,600 provinces with 15 stages takes 0.42 ms of
GPU time on a laptop RTX 4050. Every match ended with the same state hash on an RTX
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

The ones still open are on the [roadmap](roadmap.md).
