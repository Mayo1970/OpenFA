# OpenFA

Open source engine that supports Kinder & Ferrero - Fresh Adventures. 
You need to own the game for OpenFA to play it.

## Game status
- [x] Loads assets
- [x] Physics match
- [x] Levels load
- [x] Pickups work as intended
- [x] Boss fights
- [x] Scoreboard
- [x] Polish

## Extras:
- [x] Controller support
- [x] Local co-op
- [ ] Easier sprites converter
- [ ] Level editor
- [ ] Console ports

## Additional options:
- [ ] Widescreen support
- [ ] Keys rebind
- [ ] Hard mode
- [ ] Super Hard mode 
- [ ] Swap between old and new sound files for some actions
- [ ] IT/DE dual audio


## Why
This engine was made in order to play *Kinder & Ferrero - Fresh Adventures* in modern computers as well as being able to be ported to modern platforms. Besides that, this project also aims to take this game up to modern standards, with Widescreen and Gamepad support.

## Widescreen
The game itself isn't meant to run in 16:9, even though the levels would benefit greatly from it. However, boss fights' arenas aren't fit for that format, so I need to think if I should keep the game letterboxed, or apply the letterbox in the boss arenas only due to limitations.

## Co-op
The game itself is used to swap between the two characters in order to clear some obstacles, but in this build if the user hits ENTER or START in their controller, Kinder Fettalatte (or Kinder Milchschnitte if you're from Germany/Austria) will spawn and can be controlled by another player. Ammo and health are shared to both characters. Since Fettalatte/Milchsnitte can't float, you can use T or LB to instantly teleport near Kinder Pinguì.

## Hard mode and Super hard mode
The game is moderately simple, It's not too difficult, but it could get frustrating at times. One of the "improvements" I'd like to add in future is a *Hard mode* and a *Super Hard mode*. Some enemies will throw between 1-3 projectiles randomly, however, in **Hard mode**, they can throw them between 1-5 times. This won't be applied to bosses. Not only that, but you will consume **twice** the ammos (the only weapon available in this game) but you will get a 1.5x score multiplier. In **Super Hard mode**, besides the changes you'll see in hard mode, Super Hard mode, the projectile change will also be applied to bosses and you will take **double** damage from all sources, but your score will have a 5x multiplier. *May be subject to changes*

## Audio stuff
This game had **two** releases: one with each level you could find separately (for a total of 4 CDs), and one with all the 4 levels alltogether, which came out one year after. Earlier versions had some sound files changed on the full pack, but the files are still available in the full release. Users can freely choose between the old and new files. Besides that, the game got released in Germany, Austria and Italy. This means that there's **two** dubs an user can choose from, assuming they have both language audios: German and Italian.

## Known issues
These issues will be addressed in the polish phase, but still worth noting.
- Some things feel wrong (ex. climbing)

## Build

Just run this.

```sh
./make-windows-build.sh
```

## AI disclosure
Parts of this project were developed with AI assistance (Claude).
I reviewed, tested, and approved all changes before they were merged. Played through every level from start to finish to ensure they played as close as the original game, as well as applying a few changes myself to improve the overall user experience.

## Credits
- *Kinder & Ferrero - Fresh Adventures* — original game by its respective rights holders. All game assets remain their property.

## License
The OpenFA engine code is released under the MIT License. See the `LICENSE` file for the full text.

This license covers only the engine source in this repository.
It does not cover any game assets, which you must own separately.
