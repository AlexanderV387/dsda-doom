Hexen for the New Nintendo 3DS, on [DSDA-Doom](https://github.com/kraflab/dsda-doom) 0.30.1: Hexen, Deathkings of the Dark Citadel, the remaster's Enhanced Maps and its new hub, Vestiges of Grandeur, drawn by the 3DS's GPU, with the status bar and the map on the bottom screen. Its own `.cia`, folder and HOME Menu icon.

**New 3DS and New 2DS XL only.** Developed and tested on a New 3DS. It was neither designed nor tested for the Old 3DS or 2DS, where it may run very slowly, run out of memory on big maps, or not start.

## New in 1.1.0

- **800x240:** with the GPU renderer (OpenGL), the top screen in its wide mode (not on the original 2DS), on by default: twice the pixels across, sharper edges and distant detail. *Options > General > Video > Screen Resolution* chooses 400x240 or 800x240. The software renderer stays at 400x240.
- **No PC-only options in the menus:** the fullscreen ones (turning them on crashed the game), the window, mouse wheel, MIDI player and audio device ones. The Mouse page is now called Touch: its sensitivities are touch turning's.

## New in 1.0.1

- **Settings are saved when a menu closes** (if anything changed), not only when quitting or at the HOME Menu, so a crash no longer loses them (the weapon buttons, for example).

## Downloads

| File | Use |
|---|---|
| `hexen.cia` | Install with FBI; it appears on the HOME Menu |

Copy the game data to `/3ds/hexen/` (game data is not included: use your own copy):

| Game | File |
|---|---|
| Hexen | `hexen.wad` |
| Deathkings of the Dark Citadel | `hexen.wad` and `hexdd.wad` |
| Hexen (Enhanced Maps, 2025 remaster) | `hexen.wad` and `hexen_ex.wad` |
| Deathkings (Enhanced Maps, 2025 remaster) | `hexen.wad` and `hexdd_ex.wad` |
| Vestiges of Grandeur (2025 remaster) | `hexen.wad` and `hexen_vog.wad` |

The remaster's files come from its install folder (Steam, GOG). Mods go in `/3ds/hexen/mods/`. At startup a menu chooses what to play.

## Features

- **GPU renderer:** OpenGL on the 3DS's GPU (citro3d), 60 FPS in most maps.
- **Bottom screen:** status bar, map and weapon buttons (tap a number to switch weapons).
- **The three classes and the hubs**, with the maps you leave kept as you left them.
- **Vestiges of Grandeur:** its new puzzle items, pedestals, class changes and monsters, and its very large maps.
- Settings, saves and the log are kept in `/3ds/hexen/`.

## Known issues

- Vestiges of Grandeur's MAP07 takes a while to load and drops to around 10 FPS with its hordes; its walls and floors look grainy at a distance.

Port for [n3ds-ports](https://github.com/AlexanderV387/n3ds-ports): direction, design and hardware testing by AlexanderV387; code written with Claude (AI). Original engine: DSDA-Doom (Ryan Krafnick (kraflab) and contributors), based on PrBoom+; GPL-2.0. Hexen icon and banner: AlexanderV387, made with AI tools.
