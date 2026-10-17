Heretic for the New Nintendo 3DS, on [DSDA-Doom](https://github.com/kraflab/dsda-doom) 0.30.1: Heretic, its remaster's Enhanced Maps and its new episode, Faith Renewed, drawn by the 3DS's GPU, with the status bar and the map on the bottom screen. Its own `.cia`, folder and HOME Menu icon.

**New 3DS and New 2DS XL only.** Developed and tested on a New 3DS. It was neither designed nor tested for the Old 3DS or 2DS, where it may run very slowly, run out of memory on big maps, or not start.

## Downloads

| File | Use |
|---|---|
| `heretic.cia` | Install with FBI; it appears on the HOME Menu |

Copy the game data to `/3ds/heretic/` (game data is not included: use your own copy):

| Game | File |
|---|---|
| Heretic (registered or Shadow of the Serpent Riders) | `heretic.wad` |
| Heretic shareware | `heretic1.wad` |
| Enhanced Maps (2025 remaster) | `heretic.wad` and `heretic_ex.wad` |
| Faith Renewed (2025 remaster) | `heretic.wad` and `heretic_fr.wad` |

The remaster's files come from its install folder (Steam, GOG). Mods go in `/3ds/heretic/mods/`. At startup a menu chooses what to play.

## Features

- **GPU renderer:** OpenGL on the 3DS's GPU (citro3d), 60 FPS in most maps.
- **Bottom screen:** status bar, map and weapon buttons (tap a number to switch weapons).
- **Faith Renewed:** its new monsters, weapons, items and intermission map.
- Settings, saves and the log are kept in `/3ds/heretic/`.

## Known issues

- Faith Renewed's secret map (E1M9) drops to around 10 FPS with its hordes.
- Faith Renewed's ending text is blank (it lives in a language file not included in the remaster's WADs).

Port for [n3ds-ports](https://github.com/AlexanderV387/n3ds-ports): direction, design and hardware testing by AlexanderV387; code written with Claude (AI). Original engine: DSDA-Doom (Ryan Krafnick (kraflab) and contributors), based on PrBoom+; GPL-2.0. Heretic icon and banner: AlexanderV387, made with AI tools.
