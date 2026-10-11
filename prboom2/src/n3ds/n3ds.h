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

// The small frame counter on the bottom screen.
extern int n3ds_show_fps;

// Shows a fatal error on the top screen until START.
void N3DS_ShowError(const char *message);

#endif

#endif
