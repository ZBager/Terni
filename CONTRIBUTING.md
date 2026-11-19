# Contributing to Terni

Thank you for helping. Bug reports, parity results from hardware we have not tested,
documentation fixes and code are all welcome.

## Reporting a problem

Please include the Terni version or commit, your operating system, the output of
`vulkaninfo --summary`, and the smallest world or test that shows the problem. For a
determinism problem (different results on different devices), `sim.field_hashes()` on
each device says which field diverged, and `terni_diff` on two saves of the same tick
names the rows.

## Building and testing

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build
```

The tests run on every usable Vulkan device in the machine. Install Mesa's lavapipe as
well, so every change is checked against a CPU driver too. A change is ready when the
whole suite passes on every device you have. `CMakePresets.json` has presets for Linux,
Visual Studio and MSYS2 (see [getting started](docs/getting-started.md)).

Continuous integration (`.github/workflows/ci.yml`) builds every push with GCC on Linux,
with Visual Studio 2022 and 2026, and with MinGW-w64, and runs the whole suite on
lavapipe, the C# wrapper's test included. Keep the code standard C++20: no compiler extensions such as `__int128`
(tests may use it as a reference, built only where the compiler has it), and only ASCII
in string literals and generated text.

## Ground rules

- **Determinism first.** No floating point in simulation state, integer sums only, and
  identical results on every device. A change to arithmetic needs a parity test.
- **Terni stays engine-independent.** No code, names or assumptions from a particular
  engine or game. Engine plugins live in
  [TerniIntegrations](https://github.com/ZBager/TerniIntegrations), and game-specific code in the games.
- **Public API changes** need a note in `CHANGELOG.md` and in the documentation in
  `docs/`.
- **Style**: follow the surrounding code (4 spaces, lines up to 120 columns, comments
  that say why, not what).

## Making a release

1. Decide the version from what changed since the last one
   ([Releases and compatibility](docs/releases.md)): a breaking change needs a new minor
   version before 1.0, anything that changes simulation results too.
2. Set it in `project(Terni VERSION ...)` in `CMakeLists.txt`, the one place it lives
   (`terni/version.hpp` is made from it), and give `CHANGELOG.md` a section
   `## X.Y.Z (YYYY-MM-DD)` with every change since the last release.
3. Optionally rehearse: run the Release workflow by hand (Actions, "Release", "Run
   workflow"). It builds and tests the three packages and keeps them as artifacts.
4. Commit, then tag and push the tag: `git tag vX.Y.Z && git push origin vX.Y.Z`. The
   workflow checks that the tag, the project version and the changelog agree, builds
   and tests the packages, and makes a draft release with them, `SHA256SUMS` and the
   changelog section as notes.
5. Check the draft (download a package, run `bin/terni_run` on `examples/mini_galaxy`
   from the source tree) and publish it.

## Licence

Terni is licensed under the LGPL 2.1 with the Terni Linking Exception (`LICENSE`,
`LICENSE-EXCEPTION`). By contributing, you agree that your contribution is licensed under
the same terms.
