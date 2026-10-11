//
// [3DS] DSDA-Doom on the 3DS's screens: the bottom screen. The part of
// the 3DS code that knows the game (n3ds.c does not).
//

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "r_main.h"
#include "st_stuff.h"
#include "v_video.h"
#include "dsda/stretch.h"

#include "n3ds/n3ds.h"
#include "n3ds/n3ds_dsda.h"

dboolean n3ds_force_statusbar;

// The status bar on the bottom screen, at its top, in the full screen
// view (the 3D view keeps the whole top screen). It is drawn with the
// game's own code into a buffer of the top screen's size (the status bar
// at scale 1, as DSDA-Doom draws it at 400x240), and its rows copied.
static dboolean DrawStatusBar(byte *bottom)
{
  static byte *buffer;
  static int buffer_size;
  const int height = (raven ? 42 : 32) * patches_scaley;
  const int width = MIN(N3DS_BOTTOM_WIDTH, SCREENWIDTH);
  const int left = (SCREENWIDTH - width) / 2;
  byte *saved;
  int y;

  if (gamestate != GS_LEVEL || !R_FullView() || height > SCREENHEIGHT)
    return false;

  if (buffer_size < screens[0].pitch * SCREENHEIGHT)
  {
    free(buffer);
    buffer_size = screens[0].pitch * SCREENHEIGHT;
    buffer = malloc(buffer_size);
  }

  saved = screens[0].data;
  screens[0].data = buffer;
  n3ds_force_statusbar = true;
  ST_Drawer(true);
  n3ds_force_statusbar = false;
  screens[0].data = saved;

  for (y = 0; y < height; ++y)
    memcpy(bottom + y * N3DS_BOTTOM_WIDTH,
           buffer + (SCREENHEIGHT - height + y) * screens[0].pitch + left, width);

  return true;
}

static dboolean DrawBottom(byte *bottom)
{
  memset(bottom, 0, N3DS_BOTTOM_WIDTH * N3DS_BOTTOM_HEIGHT);

  return DrawStatusBar(bottom);
}

void N3DS_InitGame(void)
{
  N3DS_DrawBottom = DrawBottom;
}
