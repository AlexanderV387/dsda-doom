//
// [3DS] DSDA-Doom on the 3DS's screens (see n3ds_dsda.c).
//

#ifndef __N3DS_DSDA__
#define __N3DS_DSDA__

#ifdef __3DS__

#include "doomtype.h"

// While true, the status bar is drawn even in the full screen view: it is
// being drawn for the bottom screen.
extern dboolean n3ds_force_statusbar;

// Sets up the bottom screen.
void N3DS_InitGame(void);

// Every tic, with the events: the touch screen (keyboard, the map's
// buttons, touch turning).
void N3DS_StartTic(void);

// Once per tic, with the run button's state: whether the player runs, by
// the run mode (n3ds_run_mode).
dboolean N3DS_UpdateRunning(dboolean run_button);

// Opens or closes the keyboard on the bottom screen.
void N3DS_ShowKeyboard(dboolean show);

#endif

#endif
