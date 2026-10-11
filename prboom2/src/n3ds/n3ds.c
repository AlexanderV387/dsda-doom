//
// [3DS] What is the 3DS's own (see n3ds.h). From the Crispy Doom port
// (AlexanderV387/crispy-doom, src/n3ds.c).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>

#include "n3ds/n3ds.h"

// libctru's main thread stack is 32 KiB by default: too small for Doom.
u32 __stacksize__ = 1024 * 1024;

#define TOP_WIDTH 400
#define TOP_HEIGHT 240

// ==========================================================================
// Startup and the log

// Each line with the milliseconds since startup, written at its end (so it
// survives a hang).
static FILE *log_file;
static u64 log_start;

// stdout before the log took its place: libctru's text console writes
// there (fatal errors).
static FILE *console;

static int LogWrite(void *cookie, const char *buffer, _READ_WRITE_BUFSIZE_TYPE size)
{
  static dboolean at_line_start = true;
  int i;

  for (i = 0; i < (int) size; ++i)
  {
    if (at_line_start)
    {
      fprintf(log_file, "%6llu ", (unsigned long long) (osGetTime() - log_start));
      at_line_start = false;
    }

    fputc(buffer[i], log_file);

    if (buffer[i] == '\n')
      at_line_start = true;
  }

  if (at_line_start)
    fflush(log_file);

  return (int) size;
}

static aptHookCookie apt_hook;
static void AptHook(APT_HookType hook, void *param);
static void RestoreBottomBacklight(void);

void N3DS_Init(void)
{
  console = stdout;

  // 804 MHz and the extra cache on the New 3DS (nothing on the Old 3DS).
  osSetSpeedupEnable(true);

  mkdir("sdmc:/3ds", 0777);
  mkdir(N3DS_DATA_DIR, 0777);
  chdir(N3DS_DATA_DIR);

  aptHook(&apt_hook, AptHook, NULL);
  atexit(RestoreBottomBacklight);

  // There is no /tmp.
  setenv("TMPDIR", N3DS_DATA_DIR, 1);

  // Everything the game prints goes to a log: the 3DS has no terminal. The
  // old log is written over (creating a file on the SD card takes seconds).
  {
    const char *path = N3DS_DATA_DIR "/dsda-doom-log.txt";
    FILE *log = N3DS_OpenRewrite(path);

    if (log != NULL)
    {
      FILE *stream;

      // The old text is blanked: the new log may be shorter.
      long size;

      fseek(log, 0, SEEK_END);
      size = ftell(log);
      rewind(log);

      while (size-- > 0)
        fputc(size % 64 == 0 ? '\n' : ' ', log);

      rewind(log);

      log_file = log;
      log_start = osGetTime();
      stream = funopen(NULL, NULL, LogWrite, NULL, NULL);

      if (stream != NULL)
      {
        setvbuf(stream, NULL, _IOLBF, 1024);
        stdout = stream;
        stderr = stream;
      }
    }
  }
}

dboolean N3DS_IsNew3DS(void)
{
  static int is_new = -1;

  if (is_new < 0)
  {
    bool result = false;

    APT_CheckNew3DS(&result);
    is_new = result;
    printf("N3DS: %s 3DS\n", is_new ? "New" : "Old");
  }

  return is_new;
}

dboolean N3DS_IsClosing(void)
{
  return aptShouldClose();
}

FILE *N3DS_OpenRewrite(const char *path)
{
  FILE *file = fopen(path, "r+b");

  return file != NULL ? file : fopen(path, "wb");
}

void N3DS_CloseRewrite(FILE *file)
{
  fflush(file);

  if (ftruncate(fileno(file), ftell(file)) != 0)
  {
    // Complete anyway; only an old longer tail stays.
  }

  fclose(file);
}

// ==========================================================================
// HOME Menu, sleep mode and the bottom screen's backlight.
//
// The gsp::Lcd session is opened only around each change: keeping it open
// froze the console at the HOME Menu (in Wolfenstein), which needs it.

void (*N3DS_SaveOnSuspend)(void);

static dboolean config_saved;
static dboolean bottom_on = true;

dboolean N3DS_ConfigAlreadySaved(void)
{
  return config_saved && N3DS_IsClosing();
}

static void SetBottomBacklight(dboolean on)
{
  if (R_FAILED(gspLcdInit()))
    return;

  if (on)
    GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
  else
    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);

  gspLcdExit();
}

// Never leave the HOME Menu or sleep mode with the bottom screen off. The
// configuration is saved when the HOME Menu opens: the game may be closed
// from there, and then the SD card is much slower.
static void AptHook(APT_HookType hook, void *param)
{
  switch (hook)
  {
    case APTHOOK_ONSUSPEND:
      if (N3DS_SaveOnSuspend != NULL)
      {
        N3DS_SaveOnSuspend();
        config_saved = true;
      }
      // fall through
    case APTHOOK_ONSLEEP:
    case APTHOOK_ONEXIT:
      if (!bottom_on)
        SetBottomBacklight(true);
      break;

    case APTHOOK_ONRESTORE:
      config_saved = false;
      // fall through
    case APTHOOK_ONWAKEUP:
      if (!bottom_on)
        SetBottomBacklight(false);
      break;

    default:
      break;
  }
}

static void RestoreBottomBacklight(void)
{
  if (!bottom_on)
    SetBottomBacklight(true);
}

// ==========================================================================
// Bottom screen (320x240, rotated like the top one): the frame counter for
// now.

#define BOTTOM_WIDTH 320

int n3ds_show_fps = 1;

static void FillBottom(u32 *framebuffer, int x, int y, int w, int h, u32 color)
{
  int i, j;

  for (i = x; i < x + w; ++i)
    for (j = y; j < y + h; ++j)
      framebuffer[i * TOP_HEIGHT + (TOP_HEIGHT - 1 - j)] = color;
}

// 3x5 digits, as in the n3ds-ports hello world.
static void DrawNumber(u32 *framebuffer, int number, int x, int y, int scale,
                       u32 color)
{
  static const byte digits[10][5] =
  {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
    {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
  };
  char text[12];
  int i, row, col;

  snprintf(text, sizeof(text), "%d", number);

  for (i = 0; text[i] != '\0'; ++i)
    for (row = 0; row < 5; ++row)
      for (col = 0; col < 3; ++col)
        if (digits[text[i] - '0'][row] & (4 >> col))
          FillBottom(framebuffer, x + (i * 4 + col) * scale,
                     y + row * scale, scale, scale, color);
}

// Frames per second, the game's time per frame without waiting for the
// screen (how much of the 16.7 ms of a 60 FPS frame it uses), and the time
// composing the screens on the other core. Averaged over each second.
static int counter_fps, counter_work_ms, counter_compose_ms;

static void CountFrame(int work_us, int compose_us)
{
  static int frames = 0;
  static u64 start = 0, work_total = 0, compose_total = 0;
  const u64 now = osGetTime();

  ++frames;
  work_total += work_us;
  compose_total += compose_us;

  if (now - start >= 1000)
  {
    counter_fps = (int) (frames * 1000 / (now - start));
    counter_work_ms = (int) ((work_total / frames + 500) / 1000);
    counter_compose_ms = (int) ((compose_total / frames + 500) / 1000);
    frames = 0;
    work_total = 0;
    compose_total = 0;
    start = now;
  }
}

// Frames per second (white), milliseconds of the game per frame (green)
// and of composing the screens (yellow).
static void DrawFrameCounter(u32 *framebuffer)
{
  const int y = 2;

  FillBottom(framebuffer, BOTTOM_WIDTH - 94, y, 92, 14, 0x000000FF);
  DrawNumber(framebuffer, counter_fps, BOTTOM_WIDTH - 91, y + 2, 2, 0xFFFFFFFF);
  DrawNumber(framebuffer, counter_work_ms, BOTTOM_WIDTH - 61, y + 2, 2, 0x80FF80FF);
  DrawNumber(framebuffer, counter_compose_ms, BOTTOM_WIDTH - 31, y + 2, 2, 0xFFE060FF);
}

// ==========================================================================
// Presenting. The game's thread hands each frame (a copy) to a thread on
// another core, which composes both screens, swaps them and waits for the
// vertical blank, while the game makes the next frame. The game waits only
// if that thread has not finished the previous one, so the vertical blank
// still sets the pace.

typedef struct
{
  byte *top;
  int top_size;
  int width, height;
  u32 colors[256];        // palette in the framebuffer format (RGBA8)
  dboolean bottom;        // the bottom screen is updated (every other frame)
} frame_t;

static frame_t frame;
static LightEvent frame_ready, frame_done;
static Thread present_thread = NULL;
static volatile dboolean present_quit = false;
static u64 compose_ticks; // composing the last frame

// The top screen (400x240): the frame centered (DSDA-Doom draws at the
// screen's own size; a smaller frame is centered, a taller one cut).
static void ComposeTop(const frame_t *f)
{
  u32 *framebuffer = (u32 *) gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
  const int width = f->width < TOP_WIDTH ? f->width : TOP_WIDTH;
  const int height = f->height < TOP_HEIGHT ? f->height : TOP_HEIGHT;
  const int left = (TOP_WIDTH - width) / 2;
  const int top = (TOP_HEIGHT - height) / 2;
  int x, y, k;

  if (width < TOP_WIDTH || height < TOP_HEIGHT)
    memset(framebuffer, 0, TOP_WIDTH * TOP_HEIGHT * sizeof(u32));

  // The framebuffer is rotated: screen pixel (x, y) is at
  // [x * 240 + (239 - y)]. Copied 8 columns at a time, so the writes of
  // each source row stay in a few cache lines.
  for (x = 0; x < width; x += 8)
  {
    const int columns = width - x < 8 ? width - x : 8;
    u32 *column = framebuffer + (left + x) * TOP_HEIGHT + (TOP_HEIGHT - 1 - top);

    for (y = 0; y < height; ++y)
    {
      const byte *source = f->top + y * f->width + x;

      for (k = 0; k < columns; ++k)
        column[k * TOP_HEIGHT - y] = f->colors[source[k]];
    }
  }

  GSPGPU_FlushDataCache(framebuffer, TOP_WIDTH * TOP_HEIGHT * sizeof(u32));
  gfxScreenSwapBuffers(GFX_TOP, false);
}

static void ComposeBottom(const frame_t *f)
{
  // Whether each of the two buffers has anything but black.
  static dboolean drawn[2] = {true, true};
  static int buffer = 0;
  u32 *framebuffer;

  framebuffer = (u32 *) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

  if (drawn[buffer])
  {
    memset(framebuffer, 0, BOTTOM_WIDTH * TOP_HEIGHT * sizeof(u32));
    drawn[buffer] = false;
  }
  else if (!n3ds_show_fps)
  {
    return; // already black
  }

  if (n3ds_show_fps)
  {
    DrawFrameCounter(framebuffer);
    drawn[buffer] = true;
  }

  GSPGPU_FlushDataCache(framebuffer, BOTTOM_WIDTH * TOP_HEIGHT * sizeof(u32));
  gfxScreenSwapBuffers(GFX_BOTTOM, false);
  buffer ^= 1;
}

static void ShowFrame(void)
{
  const u64 start = svcGetSystemTick();

  ComposeTop(&frame);

  if (frame.bottom)
    ComposeBottom(&frame);

  compose_ticks = svcGetSystemTick() - start;
  gspWaitForVBlank();
}

static void PresentThread(void *arg)
{
  for (;;)
  {
    LightEvent_Wait(&frame_ready);

    if (present_quit)
      break;

    // Closed from the HOME Menu: the screens are no longer ours.
    if (!N3DS_IsClosing())
      ShowFrame();

    LightEvent_Signal(&frame_done);
  }
}

// On the system core (1), which the game may use up to 80% of the time;
// without it, everything stays on the game's core. Not on the Old 3DS: its
// system core already has the audio thread (no third core).
static void StartPresenting(void)
{
  s32 priority = 0x30;
  Result result = 0;
  int core, limit;

  LightEvent_Init(&frame_ready, RESET_ONESHOT);
  LightEvent_Init(&frame_done, RESET_STICKY);
  LightEvent_Signal(&frame_done);

  if (!N3DS_IsNew3DS())
  {
    printf("N3DS_Present: composing the screens on the game's core\n");
    return;
  }

  svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);

  // The .3dsx gets 80% (the Homebrew Launcher's limits); a .cia only up to
  // what its exheader allows: the most it accepts.
  for (limit = 80; limit > 0; limit -= 10)
  {
    result = APT_SetAppCpuTimeLimit(limit);

    if (R_SUCCEEDED(result))
      break;
  }

  present_thread = threadCreate(PresentThread, NULL, 32 * 1024, priority, 1, false);
  core = 1;

  // Otherwise the third core, with the audio thread.
  if (present_thread == NULL)
  {
    present_thread = threadCreate(PresentThread, NULL, 32 * 1024, priority, 2, false);
    core = 2;
  }

  if (present_thread != NULL)
    printf("N3DS_Present: composing the screens on core %d "
           "(core 1 time limit: %d%%, %08lx)\n", core, limit, (unsigned long) result);
  else
    printf("N3DS_Present: composing the screens on the game's core "
           "(core 1 time limit: %d%%, %08lx)\n", limit, (unsigned long) result);
}

void N3DS_StopPresenting(void)
{
  if (present_thread != NULL)
  {
    LightEvent_Wait(&frame_done);
    present_quit = true;
    LightEvent_Signal(&frame_ready);
    threadJoin(present_thread, U64_MAX);
    threadFree(present_thread);
    present_thread = NULL;
  }
}

void N3DS_Present(const byte *buffer, int width, int height, int pitch,
                  const SDL_Color *palette, dboolean palette_changed)
{
  static dboolean started = false;
  static int bottom_frames = 0;
  static u64 work_start = 0;
  u64 work_end;
  int k, y;

  // Closed from the HOME Menu: the screens are no longer ours.
  if (N3DS_IsClosing())
    return;

  if (!started)
  {
    StartPresenting();
    started = true;
    palette_changed = true;
  }

  // Wait until the previous frame is on its way to the screen.
  work_end = svcGetSystemTick();

  if (present_thread != NULL)
  {
    LightEvent_Wait(&frame_done);
    LightEvent_Clear(&frame_done);
    CountFrame((int) ((work_end - work_start) / CPU_TICKS_PER_USEC),
               (int) (compose_ticks / CPU_TICKS_PER_USEC));
    work_start = svcGetSystemTick();
  }

  // The other thread is idle: the frame is free to fill.
  if (frame.top_size < width * height)
  {
    free(frame.top);
    frame.top_size = width * height;
    frame.top = malloc(frame.top_size);
  }

  for (y = 0; y < height; ++y)
    memcpy(frame.top + y * width, buffer + y * pitch, width);

  frame.width = width;
  frame.height = height;

  if (palette_changed)
  {
    for (k = 0; k < 256; ++k)
      frame.colors[k] = ((u32) palette[k].r << 24) | ((u32) palette[k].g << 16)
                      | ((u32) palette[k].b << 8) | 0xFF;
  }

  // The bottom screen, 30 times per second.
  frame.bottom = (++bottom_frames & 1) == 0;

  if (present_thread != NULL)
  {
    LightEvent_Signal(&frame_ready);
  }
  else
  {
    ShowFrame();
    CountFrame((int) ((work_end - work_start + compose_ticks) / CPU_TICKS_PER_USEC),
               (int) (compose_ticks / CPU_TICKS_PER_USEC));
    work_start = svcGetSystemTick();
  }

  // In the log: the first frame reaches the screen.
  {
    static dboolean first = true;

    if (first)
    {
      printf("N3DS_Present: first frame (%dx%d)\n", width, height);
      first = false;
    }
  }
}

// ==========================================================================
// Fatal errors, on the top screen until START: the 3DS has no message boxes.

void N3DS_ShowError(const char *message)
{
  N3DS_StopPresenting();
  gfxInitDefault(); // the error may come before SDL starts the screens
  consoleInit(GFX_TOP, NULL);

  fprintf(console, "\x1b[2J\n DSDA-DOOM 3DS: error\n\n%s\n\n START: exit\n",
          message != NULL ? message : "");
  fflush(console);

  while (aptMainLoop())
  {
    hidScanInput();

    if ((hidKeysDown() & KEY_START) != 0)
      break;

    gspWaitForVBlank();
  }
}
