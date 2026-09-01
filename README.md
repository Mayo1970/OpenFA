# OpenFA

Open source engine that supports Kinder & Ferrero - Fresh Adventures. 
You need to own the game for OpenFA to play it.

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

Just run this.

```sh
./make-windows-build.sh
```
## Asset source

The engine reads the original root folder at run time
The decoders are linked in and run at load; path lookups apply the case/encoding normalisation.

The engine refuses to start without a valid `GData` tree. Nothing in this
repository contains or reproduces game assets.# OpenFA
