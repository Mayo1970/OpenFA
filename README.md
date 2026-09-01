# OpenFA

Open source engine that supports Kinder & Ferrero - Fresh Adventures. You need to own the game for OpenFA to play it.

## Game status
- [x] Loads assets
- [x] Physics match
- [x] Levels load
- [x] Pickups work as intended
- [ ] Boss fights
- [ ] Scoreboard
- [ ] Polish
- [ ] Console ports

## Extras:
- [ ] Controller support
- [ ] Easier sprites converter
- [ ] Level editor
- [ ] Local co-op
- [ ] Widescreen support




## Build

No external dependencies for the core.

```sh
./build-and-test.sh          # clang by default; CC=gcc also works
```

or with CMake:

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

## Layout

```
include/fa/      public engine headers (the RRR-33 abstraction contract)
  fa_loop.h        fixed-timestep loop
  fa_time.h        monotonic + wall clocks
  fa_surface.h     RGB565 software surface + blitter (RRR-35)
  fa_script.h      Lua 4.0 assignment-subset evaluator (RRR-36)
  fa_aom.h         animated-object contract + runtime (RRR-37)
  fa_res.h         resource lifetime + streaming policy (RRR-38)
src/core/        platform-independent engine code
src/platform/    one clock backend per OS (win32, posix; SDL/console later)
tests/           core tests; refsim.h is a deterministic stand-in simulation
tools/           sim_replay: headless deterministic driver (RRR-102 will use it)
```

## Asset source

The engine reads the original root folder at run time
The decoders are linked in and run at load; path lookups apply the case/encoding normalisation.

The engine refuses to start without a valid `GData` tree. Nothing in this
repository contains or reproduces game assets.# OpenFA
