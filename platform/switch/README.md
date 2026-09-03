# Fresh Adventures — Nintendo Switch homebrew target

This directory contains the first playable Switch layer for the port. It links
the real Fresh Adventures menu/game entry point, uses libnx's linear 800×600
RGB565 framebuffer, and maps two Npads into the engine's controller model.

Build inside the pinned devkitPro Docker image from the repository root:

```sh
make -C platform/switch
```

The output is `platform/switch/freshadventures.nro`.

## Assets and deploy

Keep the user's own game assets separate from the repository and copy the NRO
to:

```text
sdmc:/switch/freshadventures/freshadventures.nro
```

Copy the user's legally-owned `GData` directory next to it:

```text
sdmc:/switch/freshadventures/GData/...
```

Launch it from hbmenu in application mode. The game first opens its title /
world-select menu; use the D-pad or left stick and A to select a world. Plus
exits. If the directory install cannot be written, generated saves fall back to
`sdmc:/switch/freshadventures/save`.

Audio is routed through Switch SDL2's queued-audio API: the engine mixer
remains at 44.1 kHz and `SDL_AudioStream` converts it to the device's stereo
PCM16 format (normally 48 kHz).
