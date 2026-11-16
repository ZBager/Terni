# Licensing

Terni is licensed under the **GNU Lesser General Public License, version 2.1 only**
([`LICENSE`](../LICENSE)), **with the Terni Linking Exception**
([`LICENSE-EXCEPTION`](../LICENSE-EXCEPTION)). The short version: use Terni in any
game, open or closed, free or paid, on any platform; keep your game's code to
yourself; publish your changes to Terni itself; and credit Terni.

This page explains the licence in plain words. The licence texts are what count; if
your situation is unusual, ask a lawyer.

## What you may do

- Use Terni in a commercial, closed-source game, and sell it.
- Link Terni statically or dynamically. You do not have to ship object files, let
  players relink your game, or use a shared library.
- Ship on platforms where players cannot replace files, such as consoles.
- Keep your end-user licence as it is: Terni does not require you to allow reverse
  engineering of your game.
- Ship the shaders Terni compiles from your rules (SPIR-V caches) under your own terms.
  Your rules and data stay yours.
- Write modules, plug-ins and libraries that use Terni's API, and keep them closed.

## What you must do

1. **Publish your changes to Terni itself.** If you ship a game that contains a
   modified Terni (edited files, or files and modules you added to Terni's source),
   make the source of that modified Terni available under the LGPL to everyone who
   receives your game, for example in a public repository.
2. **Credit Terni.** In your game's credits, documentation or a notices file:
   - say that the game uses Terni;
   - include Terni's copyright notice: `Copyright (C) 2026 Rafał Zając`;
   - say where Terni's source can be obtained: <https://github.com/ZBager/Terni>;
   - include, or link to, the LGPL 2.1 and the Terni Linking Exception.

A notices file like this is enough:

```
This game uses Terni (https://github.com/ZBager/Terni).
Copyright (C) 2026 Rafał Zając.
Licensed under the GNU LGPL 2.1 with the Terni Linking Exception:
https://github.com/ZBager/Terni/blob/main/LICENSE
https://github.com/ZBager/Terni/blob/main/LICENSE-EXCEPTION
```

## Where the line is

| Your work | Licence |
| --- | --- |
| Game code that calls Terni's API | Yours: any terms |
| Your rules (`rules/*.glsl`), schema and data | Yours: any terms |
| Compiled shaders and SPIR-V caches made from your rules | Yours: any terms |
| A separate library, plug-in or tool that uses Terni's API | Yours: any terms |
| A bug fix or feature in Terni's source files | LGPL 2.1: publish it if you ship it |
| A new file or module added to Terni's source tree | LGPL 2.1: publish it if you ship it |
| A fork of Terni | LGPL 2.1; keeping the exception is your choice |

If you only use Terni unchanged, rule 1 does not apply to you: there is nothing to
publish.

## Why not a permissive licence?

Terni is meant to be free to use, including in commercial games, while improvements to
Terni itself flow back to everyone. The LGPL keeps changes to Terni open; the
exception removes the parts of the LGPL that are hard for games (relinking, reverse
engineering, shaders compiled into the game), so studios can use Terni without
lawyers having to rule on them.

## Contributing

Contributions to Terni are accepted under the same terms: the LGPL 2.1 with the Terni
Linking Exception. See [CONTRIBUTING.md](../CONTRIBUTING.md).
