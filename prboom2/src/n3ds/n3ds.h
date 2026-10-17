//
// [3DS] What is the 3DS's own: startup, the log, the screens, the HOME
// Menu and fatal errors. From the Crispy Doom port.
//

#ifndef __N3DS__
#define __N3DS__

#ifdef __3DS__

#include <stdio.h>

#include "SDL.h"
#include "doomtype.h"

// Before anything else in main: the New 3DS's full speed, the data folder
// (N3DS_DATA_DIR) as the current directory, the log with the time of each
// line (N3DS_DATA_DIR/dsda-doom-log.txt).
void N3DS_Init(void);

// Shows the 8-bit frame on the top screen (400x240), composed on another
// core at the vertical blank, and the bottom screen. SDL's software
// renderer on the 3DS scales and rotates with the CPU and ignores vsync.
void N3DS_Present(const byte *buffer, int width, int height, int pitch,
                  const SDL_Color *palette, dboolean palette_changed);

// In OpenGL mode (citro3d draws the top screen): only the bottom screen,
// and the frame counter with the game's and the GPU's time per frame.
void N3DS_PresentBottom(const SDL_Color *palette, dboolean palette_changed,
                        int work_us, int gpu_us);

// The bottom screen: 320x240, 8-bit with the game's palette.
#define N3DS_BOTTOM_WIDTH 320
#define N3DS_BOTTOM_HEIGHT 240

// Draws the bottom screen (set by the game), every other frame; returns
// whether it has anything (otherwise it stays black).
extern dboolean (*N3DS_DrawBottom)(byte *screen);

// Stops the thread that composes the screens (before SDL closes them).
void N3DS_StopPresenting(void);

// New 3DS (804 MHz, the third core) or Old 3DS (268 MHz, two cores).
dboolean N3DS_IsNew3DS(void);

// The HOME Menu is closing the game.
dboolean N3DS_IsClosing(void);

// Called when the HOME Menu opens (the game may be closed from there):
// saves the configuration. N3DS_ConfigAlreadySaved then says not to save it
// again while closing.
extern void (*N3DS_SaveOnSuspend)(void);
dboolean N3DS_ConfigAlreadySaved(void);

// Writing over an existing file instead of creating it: creating a file on
// the SD card takes seconds. N3DS_CloseRewrite cuts what is left of a
// longer old file.
FILE *N3DS_OpenRewrite(const char *path);
void N3DS_CloseRewrite(FILE *file);

// The touch screen: whether it is touched, and where (bottom screen pixels).
dboolean N3DS_GetTouch(int *x, int *y);

// Once per frame: a tap turns the bottom screen off or on, unless it shows
// something (showing) or the touch was used by the game (touch_used).
void N3DS_UpdateBottomBacklight(dboolean showing, dboolean touch_used);

// Where the time goes, in the log every 10 seconds (n3ds_gl.c reports it):
// the game's logic and the drawing, per frame.
enum
{
  N3DS_PROFILE_TICS,     // G_Ticker
  N3DS_PROFILE_DISPLAY,  // D_Display (the drawing, pgl included)
  N3DS_PROFILE_BSP,      // R_RenderBSPNodes: walking the map, what is seen
  N3DS_PROFILE_SCENE,    // gld_DrawScene: sorting and drawing it (pgl included)
  N3DS_PROFILE_VIEW,     // R_RenderPlayerView: the whole 3D view (map, scene...)
  N3DS_PROFILE_BOTTOM,   // the bottom screen (status bar, map)
  N3DS_PROFILE_CORE1,    // waiting for core 1 to finish showing the last frame
  N3DS_PROFILE_SIGHT,    // P_CheckSight: whether monsters see (in the logic)
  N3DS_PROFILE_MOVE,     // P_TryMove: moving things (in the logic)
  N3DS_PROFILE_CHASE,    // A_Chase: the monsters chasing (in the logic)
  N3DS_PROFILE_THINKERS, // P_RunThinkers: all the things and sectors (in the logic)
  N3DS_PROFILE_XYMOVE,   // P_XYMovement: things moving across (in the thinkers)
  N3DS_PROFILE_ZMOVE,    // P_ZMovement: things moving up and down (in the thinkers)
  N3DS_PROFILE_SPAWN,    // P_SpawnMobj: new things (missiles, puffs...)
  N3DS_PROFILE_SOUND,    // S_StartSoundAtVolume: sounds started
  N3DS_PROFILE_KEYFRAME, // the automatic key frames and saves (in the logic)
  N3DS_PROFILE_COUNT
};

unsigned long long N3DS_ProfileNow(void);
unsigned int N3DS_LinearFree(void);
// The GPU's memory, for what the heap cannot give (z_zone.c).
void *N3DS_LinearAlloc(size_t size);
void N3DS_LinearRelease(void *p);
void N3DS_ProfileAdd(int what, unsigned long long start);
// Milliseconds per call so far, and clears them.
void N3DS_ProfileTake(double *ms, int *calls);

// The small frame counter on the bottom screen.
extern int n3ds_show_fps;

// Chooses the game (IWAD) and the mod in the data folder, with a menu on
// the top screen when there is a choice, and gives them as the command
// line. Returns false to quit (START, or no game found).
dboolean N3DS_PickGame(int *argc, char ***argv);
// The title's own folder on the SD card: sdmc:/3ds/dsda-doom, or
// sdmc:/3ds/heretic for the Heretic title.
const char *N3DS_DataDir(void);
// A chosen WAD whose own patch file was chosen too (D4V.wad and D4V.deh):
// the patch in the WAD is not for this port, and the game skips it.
dboolean N3DS_SkipDehLump(const char *wad_path);

// Shows a fatal error on the top screen until START.
void N3DS_ShowError(const char *message);

#endif

#endif
