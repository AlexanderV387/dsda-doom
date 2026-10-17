DSDA-Doom for the New Nintendo 3DS, based on [DSDA-Doom](https://github.com/kraflab/dsda-doom) 0.30.1: Doom and its mods (vanilla, limit-removing, Boom, MBF, MBF21, UMAPINFO), drawn by the 3DS's GPU, with the status bar and the map on the bottom screen.

**New 3DS and New 2DS XL only.** Developed and tested on a New 3DS. It was neither designed nor tested for the Old 3DS or 2DS, where it may run very slowly, run out of memory on big maps, or not start.

Heretic and Hexen have their own releases (`heretic.cia`, `hexen.cia`), each with its own folder and HOME Menu icon. This `.cia` can play them too.

## Downloads

| File | Use |
|---|---|
| `dsda-doom.cia` | Install with FBI; it appears on the HOME Menu |
| `dsda-doom.3dsx` | Copy to `/3ds/` and open it from the Homebrew Launcher |

Copy the game data (IWADs) to `/3ds/dsda-doom/` (game data is not included: use your own copy, the free shareware `doom1.wad` or [Freedoom](https://freedoom.github.io/)):

| Game | File |
|---|---|
| The Ultimate Doom | `doom.wad` |
| Doom shareware (free) | `doom1.wad` |
| Doom II | `doom2.wad` |
| Final Doom | `plutonia.wad`, `tnt.wad` |
| Freedoom (free) | `freedoom1.wad`, `freedoom2.wad`, `freedm.wad` |
| Also | `chex.wad`, `hacx.wad`, `rekkrsa.wad` |

Mods (`.wad`, `.deh`, `.bex`, `.zip`) go in `/3ds/dsda-doom/mods/`. At startup a menu chooses the game, a campaign and add-ons (gameplay, sprites, music), and warns about mods that may not work.

## Features

- **GPU renderer:** OpenGL on the 3DS's GPU (citro3d), 60 FPS in most maps.
- **Bottom screen:** status bar, map and weapon buttons (tap a number to switch weapons).
- **Picker** for games, campaigns and add-ons, with warnings for mods made for other ports (GZDoom).
- **Big maps:** loads maps far larger than the original game's, within the 3DS's memory.
- Settings, saves and the log (`dsda-doom-log.txt`) are kept in `/3ds/dsda-doom/`.

## Known issues

- Maps with large hordes drop to around 10 FPS; heavy gameplay mods (Smooth Doom, for example) also slow down in big fights.
- Very large maps can look grainy at a distance.

Port for [n3ds-ports](https://github.com/AlexanderV387/n3ds-ports): direction, design and hardware testing by AlexanderV387; code written with Claude (AI). Original engine: DSDA-Doom (Ryan Krafnick (kraflab) and contributors), based on PrBoom+; GPL-2.0.
