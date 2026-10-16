//
// [3DS] DSDA-Doom on the 3DS's screens: the bottom screen (status bar, map,
// on-screen keyboard) and the touch screen. The part of the 3DS code that
// knows the game (n3ds.c does not). From the Crispy Doom port.
//

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <3ds.h>

#include "doomstat.h"
#include "d_event.h"
#include "d_main.h"
#include "am_map.h"
#include "m_menu.h"
#include "r_main.h"
#include "st_stuff.h"
#include "v_video.h"
#include "w_wad.h"
#include "dsda/configuration.h"
#include "dsda/stretch.h"

#include "n3ds/n3ds.h"
#include "n3ds/n3ds_dsda.h"

#define BOTTOM_WIDTH N3DS_BOTTOM_WIDTH
#define BOTTOM_HEIGHT N3DS_BOTTOM_HEIGHT

dboolean n3ds_force_statusbar;

// The keyboard on the bottom screen is open.
static dboolean keyboard_open;

static dboolean InGame(void);
static void DrawText(byte *screen, int x, int y, const char *text, int scale, int color);

// The bottom screen shows something (it stays on).
static dboolean bottom_showing;

// Colors of the game's palette nearest to these, for the buttons and the
// keyboard.
static int ui_black, ui_dark, ui_gray, ui_white;

static void FindColors(void)
{
  static int found_for = -1;
  const int lump = W_CheckNumForName("PLAYPAL");
  const byte *palette;

  if (lump < 0 || found_for == lump)
    return;

  palette = W_LumpByNum(lump);
  ui_black = V_BestColor(palette, 0, 0, 0);
  ui_dark = V_BestColor(palette, 48, 48, 48);
  ui_gray = V_BestColor(palette, 128, 128, 128);
  ui_white = V_BestColor(palette, 255, 255, 255);
  found_for = lump;
}

static void FillRect(byte *screen, int x, int y, int w, int h, int color)
{
  int i;

  for (i = y; i < y + h; ++i)
    memset(screen + i * BOTTOM_WIDTH + x, color, w);
}

// ==========================================================================
// The game's drawing into a buffer of the top screen's size: screens[0]
// points there meanwhile, and the rectangle drawn is copied to the bottom
// screen.

static byte *offscreen;

static void BeginOffscreen(void)
{
  static int size;

  if (size < screens[0].pitch * SCREENHEIGHT)
  {
    free(offscreen);
    size = screens[0].pitch * SCREENHEIGHT;
    offscreen = malloc(size);
  }
}

static void CopyOffscreen(byte *bottom, int src_x, int src_y, int dst_y,
                          int width, int height)
{
  int y;

  for (y = 0; y < height; ++y)
    memcpy(bottom + (dst_y + y) * BOTTOM_WIDTH,
           offscreen + (src_y + y) * screens[0].pitch + src_x, width);
}

// The status bar, at scale 1 (as DSDA-Doom draws it at 400x240).
static int StatusBarHeight(void)
{
  return (raven ? 42 : 32) * patches_scaley;
}

static void DrawStatusBar(byte *bottom, int y)
{
  const int height = StatusBarHeight();
  const int width = MIN(BOTTOM_WIDTH, SCREENWIDTH);
  byte *saved = screens[0].data;

  BeginOffscreen();
  screens[0].data = offscreen;
  n3ds_force_statusbar = true;
  ST_Drawer(true);
  n3ds_force_statusbar = false;
  screens[0].data = saved;

  CopyOffscreen(bottom, (SCREENWIDTH - width) / 2, SCREENHEIGHT - height, y,
                width, height);
}

// The map, drawn by the automap's code at the bottom screen's size.
static void DrawMap(byte *bottom, int y, int height)
{
  const int width = MIN(BOTTOM_WIDTH, SCREENWIDTH);
  byte *saved = screens[0].data;

  BeginOffscreen();
  screens[0].data = offscreen;
  AM_DrawBottom(0, 0, width, height, dsda_IntConfig(dsda_config_n3ds_map_zoom));
  screens[0].data = saved;

  CopyOffscreen(bottom, 0, 0, y, width, height);
}

// ==========================================================================
// The map's buttons, on its left: the keyboard at the top, zoom "+" above
// "-" at the bottom. A tap zooms by a quarter; holding a button zooms on.
// Touches there do not turn the view.

#define BUTTON_SIZE 24
#define BUTTON_X 4
#define STRIP_WIDTH (BUTTON_X + BUTTON_SIZE + 8)

static int map_top, map_height; // where the map was last drawn (0: none)

static void MapButtonRows(int *plus_y, int *minus_y)
{
  *minus_y = map_top + map_height - BUTTON_SIZE - 4;
  *plus_y = *minus_y - BUTTON_SIZE - 4;
}

static void DrawButtonFrame(byte *screen, int y)
{
  FillRect(screen, BUTTON_X, y, BUTTON_SIZE, BUTTON_SIZE, ui_gray);
  FillRect(screen, BUTTON_X + 2, y + 2, BUTTON_SIZE - 4, BUTTON_SIZE - 4, ui_black);
}

static void DrawMapButtons(byte *screen)
{
  const int center = BUTTON_SIZE / 2;
  int plus_y, minus_y, row, key;

  MapButtonRows(&plus_y, &minus_y);

  // A small keyboard: three rows of keys and the space bar.
  DrawButtonFrame(screen, map_top + 4);
  for (row = 0; row < 3; ++row)
    for (key = 0; key < 4 - (row == 2); ++key)
      FillRect(screen, BUTTON_X + 5 + row + key * 4, map_top + 9 + row * 4, 2, 2, ui_white);
  FillRect(screen, BUTTON_X + 8, map_top + 21, 8, 2, ui_white);

  DrawButtonFrame(screen, plus_y);
  FillRect(screen, BUTTON_X + 5, plus_y + center - 1, BUTTON_SIZE - 10, 2, ui_white);
  FillRect(screen, BUTTON_X + center - 1, plus_y + 5, 2, BUTTON_SIZE - 10, ui_white);

  DrawButtonFrame(screen, minus_y);
  FillRect(screen, BUTTON_X + 5, minus_y + center - 1, BUTTON_SIZE - 10, 2, ui_white);
}

// Every tic. Returns whether the touch is on the buttons' strip.
static dboolean UpdateMapButtons(int x, int y, dboolean touching, dboolean pressed)
{
  static int held;
  int plus_y, minus_y, direction = 0;

  if (map_height == 0 || !touching || x >= STRIP_WIDTH
      || y < map_top || y >= map_top + map_height)
  {
    held = 0;
    return false;
  }

  MapButtonRows(&plus_y, &minus_y);

  if (pressed && y < map_top + BUTTON_SIZE + 8)
    N3DS_ShowKeyboard(true);
  else if (y >= plus_y - 4 && y < plus_y + BUTTON_SIZE + 2)
    direction = 1;
  else if (y >= minus_y - 2 && y < minus_y + BUTTON_SIZE + 4)
    direction = -1;

  if (direction != 0)
  {
    if (held == 0 || held >= 10)
    {
      const int step = held == 0 ? 125 : 104;
      int zoom = dsda_IntConfig(dsda_config_n3ds_map_zoom);

      zoom = direction > 0 ? zoom * step / 100 : zoom * 100 / step;
      dsda_UpdateIntConfig(dsda_config_n3ds_map_zoom, BETWEEN(25, 800, zoom), true);
    }

    ++held;
  }

  return true;
}

// ==========================================================================
// The weapon buttons, a row at the bottom: a tap chooses the weapon of that
// number, as the PC's number keys (pressed for a tic). The weapons held are
// white, the one in hand on gray, the others dark.

#define WEAPON_BAR_HEIGHT 24
#define MAX_SLOTS 7

static int weapon_bar_y; // where the row was last drawn (0: none)

// The weapons of each number: Doom's, Heretic's (with the same numbers),
// Hexen's four.
static const int slot_weapons[2][MAX_SLOTS][2] =
{
  { { wp_fist, wp_chainsaw }, { wp_pistol, -1 }, { wp_shotgun, wp_supershotgun },
    { wp_chaingun, -1 }, { wp_missile, -1 }, { wp_plasma, -1 }, { wp_bfg, -1 } },
  { { wp_staff, wp_gauntlets }, { wp_goldwand, wp_tempest }, { wp_crossbow, -1 },
    { wp_blaster, -1 }, { wp_skullrod, -1 }, { wp_phoenixrod, -1 }, { wp_mace, -1 } },
};

static int WeaponSlots(void)
{
  return hexen ? 4 : MAX_SLOTS;
}

static dboolean SlotHas(int slot, int weapon)
{
  if (hexen)
    return weapon == slot;

  return slot_weapons[heretic][slot][0] == weapon || slot_weapons[heretic][slot][1] == weapon;
}

static dboolean SlotOwned(const player_t *player, int slot)
{
  int w;

  for (w = 0; w < NUMWEAPONS; ++w)
    if (player->weaponowned[w] && SlotHas(slot, w))
      return true;

  return false;
}

static void DrawWeaponBar(byte *screen, int y)
{
  const player_t *player = &players[displayplayer];
  const int slots = WeaponSlots();
  const int pitch = BOTTOM_WIDTH / slots;
  int slot;

  weapon_bar_y = y;

  for (slot = 0; slot < slots; ++slot)
  {
    const int x = slot * pitch + 2;
    const int w = pitch - 4;
    const dboolean in_hand = SlotHas(slot, player->readyweapon);
    char digit[2] = { (char) ('1' + slot), 0 };

    FillRect(screen, x, y + 2, w, WEAPON_BAR_HEIGHT - 4, ui_gray);
    FillRect(screen, x + 2, y + 4, w - 4, WEAPON_BAR_HEIGHT - 8,
             in_hand ? ui_gray : ui_black);
    DrawText(screen, x + (w - 10) / 2, y + (WEAPON_BAR_HEIGHT - 14) / 2, digit, 2,
             in_hand ? ui_black : SlotOwned(player, slot) ? ui_white : ui_dark);
  }
}

// Every tic. Returns whether the touch is on the row.
static dboolean UpdateWeaponBar(int x, int y, dboolean touching, dboolean pressed)
{
  static int key_down;
  event_t ev;

  memset(&ev, 0, sizeof(ev));

  // Held for one tic, to be seen by the game.
  if (key_down)
  {
    ev.type = ev_keyup;
    ev.data1.i = key_down;
    D_PostEvent(&ev);
    key_down = 0;
  }

  if (weapon_bar_y == 0 || !touching || y < weapon_bar_y)
    return false;

  if (pressed && InGame())
  {
    key_down = '1' + BETWEEN(0, WeaponSlots() - 1, x * WeaponSlots() / BOTTOM_WIDTH);
    ev.type = ev_keydown;
    ev.data1.i = key_down;
    D_PostEvent(&ev);
  }

  return true;
}

// ==========================================================================
// The keyboard on the bottom screen (the system's keyboard is an applet that
// stops the game and leaves the screens in another mode). Each key reaches
// the game as a key press, as on a PC keyboard: names, cheats, the console.
// It opens when the game asks for text and with the map's keyboard button.

// 5x7 letters: the rows' 5 low bits, the leftmost pixel the highest.
static const struct { char ch; byte rows[7]; } font[] =
{
  {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
  {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
  {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
  {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
  {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
  {'A', {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}}, {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
  {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
  {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
  {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
  {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
  {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
  {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N', {0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
  {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
  {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
  {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
  {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}, {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
  {'Y', {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}}, {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
  {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
  {'_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
};

static void DrawChar(byte *screen, int x, int y, char ch, int scale, int color)
{
  int i, row, col;

  for (i = 0; i < sizeof(font) / sizeof(font[0]); ++i)
  {
    if (font[i].ch != ch)
      continue;

    for (row = 0; row < 7; ++row)
      for (col = 0; col < 5; ++col)
        if (font[i].rows[row] & (0x10 >> col))
          FillRect(screen, x + col * scale, y + row * scale, scale, scale, color);
    return;
  }
}

static void DrawText(byte *screen, int x, int y, const char *text, int scale, int color)
{
  for (; *text != '\0'; ++text, x += 6 * scale)
    DrawChar(screen, x, y, *text, scale, color);
}

#define KEY_W 28
#define KEY_H 30
#define KEY_PITCH_X 31
#define KEY_PITCH_Y 34
#define KEYS_X 6
#define KEYS_Y 50

typedef struct
{
  int x, y, w;        // in keys
  int key;            // the key it presses, or 0 to close
  const char *label;
} vkey_t;

static vkey_t vkeys[64];
static int num_vkeys;

static void AddKey(int x, int y, int w, int key, const char *label)
{
  vkeys[num_vkeys].x = x;
  vkeys[num_vkeys].y = y;
  vkeys[num_vkeys].w = w;
  vkeys[num_vkeys].key = key;
  vkeys[num_vkeys].label = label;
  ++num_vkeys;
}

static void SetUpKeys(void)
{
  static const char *const rows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM."};
  static char labels[40][2];
  int row, i, n = 0;

  for (row = 0; row < sizeof(rows) / sizeof(rows[0]); ++row)
  {
    for (i = 0; rows[row][i] != '\0'; ++i, ++n)
    {
      labels[n][0] = rows[row][i];
      labels[n][1] = '\0';
      AddKey(i, row, 1, tolower((unsigned char) rows[row][i]), labels[n]);
    }
  }

  AddKey(8, 3, 2, KEYD_BACKSPACE, "DEL");
  AddKey(0, 4, 3, 0, "CLOSE");
  AddKey(3, 4, 4, ' ', "SPACE");
  AddKey(7, 4, 3, KEYD_ENTER, "ENTER");
}

static void KeyRect(const vkey_t *k, int *x, int *y, int *w)
{
  *x = KEYS_X + k->x * KEY_PITCH_X;
  *y = KEYS_Y + k->y * KEY_PITCH_Y;
  *w = k->w * KEY_PITCH_X - (KEY_PITCH_X - KEY_W);
}

static int pressed_key = -1; // being touched, drawn lit

static void DrawKeyboard(byte *screen)
{
  int i, x, y, w;

  memset(screen, ui_black, BOTTOM_WIDTH * BOTTOM_HEIGHT);
  DrawText(screen, 10, 14, "KEYBOARD", 2, ui_gray);

  for (i = 0; i < num_vkeys; ++i)
  {
    const vkey_t *k = &vkeys[i];
    const int len = (int) strlen(k->label);
    const int scale = len == 1 ? 2 : 1;

    KeyRect(k, &x, &y, &w);
    FillRect(screen, x, y, w, KEY_H, i == pressed_key ? ui_white : ui_gray);
    FillRect(screen, x + 1, y + 1, w - 2, KEY_H - 2, i == pressed_key ? ui_gray : ui_dark);
    DrawText(screen, x + (w - len * 6 * scale + scale) / 2,
             y + (KEY_H - 7 * scale) / 2, k->label, scale, ui_white);
  }
}

static void PostKey(int key)
{
  event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = ev_keydown;
  ev.data1.i = key;
  D_PostEvent(&ev);
  ev.type = ev_keyup;
  D_PostEvent(&ev);

  // The console reads text, not keys.
  if (SDL_IsTextInputActive() && key >= ' ' && key < 127)
  {
    static char text[2];

    text[0] = (char) key;
    memset(&ev, 0, sizeof(ev));
    ev.type = ev_text;
    ev.text = text;
    D_PostEvent(&ev);
  }
}

static void UpdateKeyboard(int x, int y, dboolean touching, dboolean pressed)
{
  int i, kx, ky, kw;

  if (num_vkeys == 0)
    SetUpKeys();

  if (!touching)
  {
    pressed_key = -1;
    return;
  }

  if (!pressed)
    return;

  pressed_key = -1;

  for (i = 0; i < num_vkeys; ++i)
  {
    KeyRect(&vkeys[i], &kx, &ky, &kw);

    if (x >= kx && x < kx + kw && y >= ky && y < ky + KEY_H)
    {
      pressed_key = i;

      if (vkeys[i].key == 0)
        keyboard_open = false;
      else
        PostKey(vkeys[i].key);
      break;
    }
  }
}

void N3DS_ShowKeyboard(dboolean show)
{
  keyboard_open = show;
}

// ==========================================================================
// Touch turning: sliding on the bottom screen turns (and looks up and down
// with free look), as a mouse. Only in the game, and not from the map's
// buttons.

static void UpdateTouchTurn(int x, int y, dboolean touching, dboolean pressed)
{
  static int last_x, last_y;
  const int speed = dsda_IntConfig(dsda_config_n3ds_touch_speed);
  event_t ev;

  if (!touching)
    return;

  if (!pressed && (x != last_x || y != last_y))
  {
    memset(&ev, 0, sizeof(ev));
    ev.type = ev_mousemotion;
    ev.data1.i = (x - last_x) * speed;
    ev.data2.i = (last_y - y) * speed;
    D_PostEvent(&ev);
  }

  last_x = x;
  last_y = y;
}

// ==========================================================================
// Running: while the button is held, press it once (until the player stops
// moving), or also with the Circle Pad fully pushed.

dboolean N3DS_UpdateRunning(dboolean run_button)
{
  static dboolean was_held, toggled, has_moved;
  const dboolean pressed = run_button && !was_held;
  circlePosition pad;
  int length2;

  was_held = run_button;
  hidCircleRead(&pad);
  length2 = pad.dx * pad.dx + pad.dy * pad.dy;

  switch (dsda_IntConfig(dsda_config_n3ds_run_mode))
  {
    case 1:
    {
      // Moving: the Circle Pad, or the D-pad up or down.
      const dboolean moving = length2 >= 30 * 30
                           || (hidKeysHeld() & (KEY_DUP | KEY_DDOWN)) != 0;

      if (pressed)
      {
        toggled = !toggled;
        has_moved = false;
      }

      // Stopping after moving stops running.
      if (toggled && moving)
        has_moved = true;
      else if (toggled && has_moved && !moving)
        toggled = false;

      return toggled;
    }

    case 2:
      return run_button || length2 >= 140 * 140;

    default:
      return run_button;
  }
}

static dboolean InGame(void)
{
  return gamestate == GS_LEVEL && !menuactive && !automap_full;
}

// ==========================================================================

static dboolean DrawBottomContents(byte *bottom);

static dboolean DrawBottom(byte *bottom)
{
  // In OpenGL mode the game's drawing functions go to the GPU (the top
  // screen): the software ones for the bottom screen.
  if (V_IsOpenGLMode())
  {
    V_InitMode(VID_MODESW);
    bottom_showing = DrawBottomContents(bottom);
    V_InitMode(VID_MODEGL);
  }
  else
  {
    bottom_showing = DrawBottomContents(bottom);
  }

  return bottom_showing;
}

static dboolean DrawBottomContents(byte *bottom)
{
  const dboolean in_level = gamestate == GS_LEVEL && players[displayplayer].mo != NULL
                           && ST_Ready();
  const int status_height = StatusBarHeight();
  int status_y = 0;

  FindColors();
  n3ds_show_fps = dsda_IntConfig(dsda_config_n3ds_show_fps);

  if (keyboard_open)
  {
    DrawKeyboard(bottom);
    return true;
  }

  memset(bottom, 0, BOTTOM_WIDTH * BOTTOM_HEIGHT);
  map_height = 0;
  weapon_bar_y = 0;

  if (!in_level)
    return false;

  // From the bottom up: the weapon buttons, the status bar (in the full
  // screen view: otherwise it is on the top screen) above or below the
  // map, the map in the rest.
  {
    const dboolean weapon_bar = dsda_IntConfig(dsda_config_n3ds_weapon_bar);
    const dboolean status = R_FullView() && status_height <= BOTTOM_HEIGHT;
    const dboolean below = dsda_IntConfig(dsda_config_n3ds_status_below);
    int top = 0, end = BOTTOM_HEIGHT;

    if (weapon_bar)
    {
      end -= WEAPON_BAR_HEIGHT;
      DrawWeaponBar(bottom, end);
    }

    if (status)
    {
      if (below)
      {
        end -= status_height;
        status_y = end;
      }
      else
      {
        status_y = 0;
        top = status_height;
      }
    }

    if (dsda_IntConfig(dsda_config_n3ds_bottom_map) && end - top >= 3 * BUTTON_SIZE + 16)
    {
      map_top = top;
      map_height = end - top;
      DrawMap(bottom, map_top, map_height);
      DrawMapButtons(bottom);
    }

    if (status)
      DrawStatusBar(bottom, status_y);

    return weapon_bar || status || map_height > 0;
  }
}

// The status bar on the top screen: the view a size smaller (screenblocks
// 10); off, the full view again, the status bar on the bottom screen.
void N3DS_StatusTopChanged(void)
{
  dsda_UpdateIntConfig(dsda_config_screenblocks,
                       dsda_IntConfig(dsda_config_n3ds_status_top) ? 10 : 11, true);
}

void N3DS_StartTic(void)
{
  static dboolean was_touching;
  int x = 0, y = 0;
  const dboolean touching = N3DS_GetTouch(&x, &y);
  const dboolean pressed = touching && !was_touching;
  dboolean used = false;

  was_touching = touching;

  // The game asks for text (a saved game's name, the console): the
  // keyboard opens, and closes when it is done.
  {
    extern int saveStringEnter;
    static dboolean asked;
    const dboolean asking = saveStringEnter || SDL_IsTextInputActive();

    if (asking != asked)
    {
      keyboard_open = asking;
      asked = asking;
    }
  }

  if (keyboard_open)
  {
    UpdateKeyboard(x, y, touching, pressed);
    used = true;
  }
  else if (UpdateWeaponBar(x, y, touching, pressed))
  {
    used = true;
  }
  else if (UpdateMapButtons(x, y, touching, pressed))
  {
    used = true;
  }
  else if (dsda_IntConfig(dsda_config_n3ds_touch_turn) && InGame())
  {
    UpdateTouchTurn(x, y, touching, pressed);
    used = true;
  }

  N3DS_UpdateBottomBacklight(keyboard_open || bottom_showing, used);
}

void N3DS_InitGame(void)
{
  N3DS_DrawBottom = DrawBottom;
}
