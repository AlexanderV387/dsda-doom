//
// [3DS] What is the 3DS's own (see n3ds.h). From the Crispy Doom port
// (AlexanderV387/crispy-doom, src/n3ds.c).
//

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

// On the text screens: this title's name.
static const char *picker_title = "DSDA-DOOM 3DS";

// The Heretic and Hexen titles, each its own .cia (the same program),
// known by its unique ID: only that game, with its own name and folder
// (sdmc:/3ds/heretic, sdmc:/3ds/hexen: the game, its mods, configuration,
// saves and log), nothing of Doom's.
static const struct
{
  u32 unique_id;
  const char *game; // the start of its IWADs' names
  const char *picker_title;
  const char *data_dir;
} game_titles[] =
{
  { 0xF3D59, "heretic", "HERETIC 3DS", "sdmc:/3ds/heretic" },
  { 0xF3D5A, "hexen",   "HEXEN 3DS",   "sdmc:/3ds/hexen" },
};

// NULL in DSDA-Doom's own title: every game.
static const char *only_game;
static const char *data_dir = N3DS_DATA_DIR;

const char *N3DS_DataDir(void)
{
  return data_dir;
}

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

// The memory, split between the heap (the game's own) and the linear heap
// (the GPU's: textures, vertices) before main. libctru gives the linear
// heap at most 32 MB; here 40 MB once there are 100 MB (the .cia's 124 MB
// mode on the New 3DS: the heap keeps about 78 MB, more than mods need
// since the lumps are not all read at start). With more memory (the 178
// MB mode, not asked for: the HOME Menu gets slow with it) the heap takes
// what its addresses allow (96 MB), the linear heap the rest. With less,
// as libctru.
#define LINEAR_DEFAULT (32 << 20)
#define LINEAR_MORE (40 << 20)
#define LINEAR_BIG (64 << 20)
#define HEAP_SPLIT (24 << 20)
#define MORE_MEMORY (100 << 20)
#define BIG_MEMORY (150 << 20)
#define HEAP_MAX (OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN)

extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;

void __system_allocateHeaps(void)
{
  Handle reslimit = 0;
  ResourceLimitType type = RESLIMIT_COMMIT;
  s64 max_commit = 0, current_commit = 0;
  u32 remaining;

  if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
    svcBreak(USERBREAK_PANIC);

  svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
  svcGetResourceLimitCurrentValues(&current_commit, reslimit, &type, 1);
  svcCloseHandle(reslimit);

  remaining = (u32) (max_commit - current_commit) & ~0xFFF;

  if (remaining >= BIG_MEMORY)
    __ctru_linear_heap_size = LINEAR_BIG;
  else if (remaining >= MORE_MEMORY)
    __ctru_linear_heap_size = LINEAR_MORE;
  else if (remaining / 2 > HEAP_SPLIT)
    __ctru_linear_heap_size = remaining - HEAP_SPLIT > LINEAR_DEFAULT
                            ? LINEAR_DEFAULT : (remaining - HEAP_SPLIT) & ~0xFFF;
  else
    __ctru_linear_heap_size = (remaining / 2) & ~0xFFF;

  __ctru_heap_size = remaining - __ctru_linear_heap_size;

  // The heap's addresses end at 96 MB (OS_HEAP_AREA_END): the rest goes to
  // the linear heap.
  if (__ctru_heap_size > HEAP_MAX)
  {
    __ctru_heap_size = HEAP_MAX;
    __ctru_linear_heap_size = remaining - HEAP_MAX;
  }

  if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size,
                                MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE)))
    svcBreak(USERBREAK_PANIC);

  if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size,
                                MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE)))
    svcBreak(USERBREAK_PANIC);

  mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

  fake_heap_start = (char *) __ctru_heap;
  fake_heap_end = fake_heap_start + __ctru_heap_size;
}

// ==========================================================================
// Crashes (a CPU exception: a bad pointer, often mods that do not go
// together) end on a screen of ours and back in the picker, not on Luma's
// crash screen with the console to turn off. Nothing is mended: the
// process ends. The default handler of every thread, on a stack of its own.

static u8 crash_stack[0x4000] __attribute__((aligned(8)));

static void __attribute__((noreturn)) CrashExit(void)
{
  // Services closed as at any exit (APT starts the picker again), but the
  // heap is not given back: the other threads (sound, GPU events) still
  // run on it until the process ends.
  extern void __appExit(void);

  __appExit();
  svcExitProcess();
  for (;;)
    ;
}

static void __attribute__((target("arm"), noreturn)) CrashHandler(ERRF_ExceptionInfo *excep,
                                                                   CpuRegisters *regs)
{
  static volatile int crashing = 0;
  static const char *const kinds[] = { "prefetch abort", "data abort", "undefined instruction",
                                       "VFP exception" };
  const char *kind = excep->type < 4 ? kinds[excep->type] : "exception";
  char line[200];
  int length, i;

  // A crash in here, or in another thread meanwhile: just end.
  if (crashing++)
    CrashExit();

  // The log, written straight to its file: the C library's buffers may be
  // what broke.
  if (log_file != NULL)
  {
    length = snprintf(line, sizeof(line),
                      "%6lu N3DS: crash: %s at %08lX (from %08lX), address %08lX\n",
                      (unsigned long) (osGetTime() - log_start), kind, (unsigned long) regs->pc,
                      (unsigned long) regs->lr, (unsigned long) excep->far);
    write(fileno(log_file), line, length);
  }

  RestoreBottomBacklight();

  // The GPU's last copies to the screens done first.
  for (i = 0; i < 3; ++i)
    gspWaitForVBlank();

  if (gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL) == NULL)
    gfxInitDefault();
  consoleInit(GFX_TOP, NULL);

  fprintf(console, "\x1b[2J\n %s: the game crashed\n\n"
                   " Most likely, mods that do not go\n"
                   " together: the warning before playing\n"
                   " lists the ones that may not.\n\n"
                   " (%s at %08lX)\n\n"
                   " START: back to the picker\n",
          picker_title, kind, (unsigned long) regs->pc);
  fflush(console);

  while (!aptShouldClose())
  {
    hidScanInput();

    if ((hidKeysDown() & KEY_START) != 0)
      break;

    gspWaitForVBlank();
  }

  // The installed title starts again in the picker (a .3dsx goes back to
  // the Homebrew Launcher); not when the HOME Menu is closing it.
  if (!aptShouldClose() && !envIsHomebrew())
    aptSetChainloaderToSelf();

  CrashExit();
}

void N3DS_Init(void)
{
  console = stdout;

  threadOnException(CrashHandler, crash_stack + sizeof(crash_stack), WRITE_DATA_TO_HANDLER_STACK);

  // 804 MHz and the extra cache on the New 3DS (nothing on the Old 3DS).
  osSetSpeedupEnable(true);

  {
    u64 program_id = 0;

    int i;

    if (R_SUCCEEDED(APT_GetProgramID(&program_id)))
      for (i = 0; i < (int) (sizeof(game_titles) / sizeof(game_titles[0])); ++i)
        if (((program_id >> 8) & 0xFFFFF) == game_titles[i].unique_id)
        {
          only_game = game_titles[i].game;
          picker_title = game_titles[i].picker_title;
          data_dir = game_titles[i].data_dir;
        }
  }

  // The data is the SD card's; dsda-doom.wad is in the title itself.
  romfsInit();

  mkdir("sdmc:/3ds", 0777);
  mkdir(data_dir, 0777);
  chdir(data_dir);

  aptHook(&apt_hook, AptHook, NULL);
  atexit(RestoreBottomBacklight);

  // There is no /tmp.
  setenv("TMPDIR", data_dir, 1);
}

// Everything the game prints goes to a log: the 3DS has no terminal. The
// old log is written over (creating a file on the SD card takes seconds),
// and only once a game is chosen: quitting goes back to the picker (a new
// start of this title), and the log of what went wrong must still be
// there.
static void StartLog(void)
{
  char path[64];
  FILE *log;
  FILE *stream;
  long size;

  snprintf(path, sizeof(path), "%s/%s-log.txt", data_dir, only_game ? only_game : "dsda-doom");
  log = N3DS_OpenRewrite(path);

  if (log == NULL)
    return;

  // The old text is blanked: the new log may be shorter.
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

  printf("N3DS: memory: %lu MiB for the game, %lu MiB for the GPU\n",
         (unsigned long) (__ctru_heap_size >> 20), (unsigned long) (__ctru_linear_heap_size >> 20));
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

dboolean N3DS_WideSupported(void)
{
  static int supported = -1;

  if (supported < 0)
  {
    u8 model = CFG_MODEL_3DS;

    if (R_SUCCEEDED(cfguInit()))
    {
      CFGU_GetSystemModel(&model);
      cfguExit();
    }

    supported = model != CFG_MODEL_2DS;
  }

  return supported;
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
  printf("N3DS: %s\n", hook == APTHOOK_ONSUSPEND ? "to the HOME Menu"
                      : hook == APTHOOK_ONRESTORE ? "back from the HOME Menu"
                      : hook == APTHOOK_ONSLEEP ? "to sleep"
                      : hook == APTHOOK_ONWAKEUP ? "woken up"
                      : hook == APTHOOK_ONEXIT ? "closed by the system" : "APT event");

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

// A tap turns the bottom screen off or on, except while it shows something
// or the touch screen turns the view; when something appears it turns on
// by itself.
void N3DS_UpdateBottomBacklight(dboolean showing, dboolean touch_used)
{
  static dboolean was_touching = false;
  const dboolean touching = (hidKeysHeld() & KEY_TOUCH) != 0;

  if (touching && !was_touching && !touch_used && (!bottom_on || !showing))
  {
    bottom_on = !bottom_on;
    SetBottomBacklight(bottom_on);
  }
  else if (showing && !bottom_on)
  {
    bottom_on = true;
    SetBottomBacklight(true);
  }

  was_touching = touching;
}

dboolean N3DS_GetTouch(int *x, int *y)
{
  touchPosition touch;

  if ((hidKeysHeld() & KEY_TOUCH) == 0)
    return false;

  hidTouchRead(&touch);
  *x = touch.px;
  *y = touch.py;
  return true;
}

// ==========================================================================
// Profiling

static u64 profile_ticks[N3DS_PROFILE_COUNT];
static int profile_calls[N3DS_PROFILE_COUNT];

unsigned int N3DS_LinearFree(void)
{
  return linearSpaceFree();
}

void *N3DS_LinearAlloc(size_t size)
{
  return linearAlloc(size);
}

void N3DS_LinearRelease(void *p)
{
  linearFree(p);
}

unsigned long long N3DS_ProfileNow(void)
{
  return svcGetSystemTick();
}

void N3DS_ProfileAdd(int what, unsigned long long start)
{
  profile_ticks[what] += svcGetSystemTick() - start;
  ++profile_calls[what];
}

void N3DS_ProfileTake(double *ms, int *calls)
{
  int k;

  for (k = 0; k < N3DS_PROFILE_COUNT; ++k)
  {
    ms[k] = profile_ticks[k] / (CPU_TICKS_PER_MSEC);
    calls[k] = profile_calls[k];
    profile_ticks[k] = 0;
    profile_calls[k] = 0;
  }
}

// ==========================================================================
// Bottom screen (320x240, rotated like the top one): what the game draws
// (the status bar), 30 times per second, and the frame counter.

#define BOTTOM_WIDTH N3DS_BOTTOM_WIDTH

dboolean (*N3DS_DrawBottom)(byte *screen);

int n3ds_show_fps = 0;

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
  dboolean has_top;       // false in OpenGL mode: citro3d draws the top screen
  byte *top;
  int top_size;
  int width, height;
  u32 colors[256];        // palette in the framebuffer format (RGBA8)
  dboolean bottom;        // the bottom screen is updated (every other frame)
  dboolean bottom_showing; // and bottom_screen has something
  byte bottom_screen[N3DS_BOTTOM_WIDTH * N3DS_BOTTOM_HEIGHT];
} frame_t;

static frame_t frame;
static LightEvent frame_ready, frame_done;
static Thread present_thread = NULL;
static volatile dboolean present_quit = false;
static u64 compose_ticks; // composing the last frame
static dboolean presenting_started; // once, for both modes

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

  if (f->bottom_showing)
  {
    int x, y, k;

    // 8 columns at a time, as on the top screen.
    for (x = 0; x < BOTTOM_WIDTH; x += 8)
    {
      u32 *column = framebuffer + x * TOP_HEIGHT + (TOP_HEIGHT - 1);

      for (y = 0; y < N3DS_BOTTOM_HEIGHT; ++y)
      {
        const byte *source = f->bottom_screen + y * BOTTOM_WIDTH + x;

        for (k = 0; k < 8; ++k)
          column[k * TOP_HEIGHT - y] = f->colors[source[k]];
      }
    }

    drawn[buffer] = true;
  }
  else if (drawn[buffer])
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

  if (frame.has_top)
    ComposeTop(&frame);

  if (frame.bottom)
    ComposeBottom(&frame);

  compose_ticks = svcGetSystemTick() - start;

  // In OpenGL mode citro3d waits for it.
  if (frame.has_top)
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
  static int bottom_frames = 0;
  static u64 work_start = 0;
  u64 work_end;
  int k, y;

  // Closed from the HOME Menu: the screens are no longer ours.
  if (N3DS_IsClosing())
    return;

  if (!presenting_started)
  {
    StartPresenting();
    presenting_started = true;
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

  // Back from OpenGL mode: the colors were the bottom screen's.
  if (!frame.has_top)
    palette_changed = true;

  frame.has_top = true;
  frame.width = width;
  frame.height = height;

  if (palette_changed)
  {
    for (k = 0; k < 256; ++k)
      frame.colors[k] = ((u32) palette[k].r << 24) | ((u32) palette[k].g << 16)
                      | ((u32) palette[k].b << 8) | 0xFF;
  }

  // The bottom screen, 30 times per second: drawn here, with the game.
  frame.bottom = (++bottom_frames & 1) == 0;

  if (frame.bottom)
    frame.bottom_showing = N3DS_DrawBottom != NULL && N3DS_DrawBottom(frame.bottom_screen);

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

void N3DS_PresentBottom(const SDL_Color *palette, dboolean palette_changed,
                        int work_us, int gpu_us)
{
  static int bottom_frames = 0;
  int k;

  if (N3DS_IsClosing())
    return;

  if (!presenting_started)
  {
    StartPresenting();
    presenting_started = true;
    palette_changed = true;
  }

  if (present_thread != NULL)
  {
    const unsigned long long start = N3DS_ProfileNow();

    LightEvent_Wait(&frame_done);
    LightEvent_Clear(&frame_done);
    N3DS_ProfileAdd(N3DS_PROFILE_CORE1, start);
  }

  // The frame counter: the game's work per frame (green) and the GPU's
  // drawing (yellow).
  CountFrame(work_us, gpu_us);

  frame.has_top = false;

  if (palette_changed)
  {
    for (k = 0; k < 256; ++k)
      frame.colors[k] = ((u32) palette[k].r << 24) | ((u32) palette[k].g << 16)
                      | ((u32) palette[k].b << 8) | 0xFF;
  }

  frame.bottom = (++bottom_frames & 1) == 0;

  if (frame.bottom)
  {
    const unsigned long long start = N3DS_ProfileNow();

    frame.bottom_showing = N3DS_DrawBottom != NULL && N3DS_DrawBottom(frame.bottom_screen);
    N3DS_ProfileAdd(N3DS_PROFILE_BOTTOM, start);
  }

  if (present_thread != NULL)
    LightEvent_Signal(&frame_ready);
  else if (frame.bottom)
    ShowFrame();
}

// ==========================================================================
// Fatal errors, on the top screen until START: the 3DS has no message boxes.

void N3DS_ShowError(const char *message)
{
  u16 width, height;
  // The error may come before SDL starts the screens. Started twice, the
  // GPU's service would stay open at exit, and its event thread would run
  // on in freed memory: a crash when closed from the HOME Menu.
  const dboolean own_screens = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height) == NULL;

  N3DS_StopPresenting();
  if (own_screens)
    gfxInitDefault();
  consoleInit(GFX_TOP, NULL);

  fprintf(console, "\x1b[2J\n %s: error\n\n%s\n\n START: exit\n", picker_title,
          message != NULL ? message : "");
  fflush(console);

  while (aptMainLoop())
  {
    hidScanInput();

    if ((hidKeysDown() & KEY_START) != 0)
      break;

    gspWaitForVBlank();
  }

  if (own_screens)
    gfxExit();
}

// ==========================================================================
// Game and mod picker, before the game starts, on the top screen's text
// console: the IWADs in the data folder and the mods (PWADs, DeHackEd
// patches and zips) in its "mods" folder. Nothing to choose: straight into
// the game.

// How a game or a campaign names its maps: a campaign is shown only for
// the games naming theirs the same way (or always, when that is unknown).
enum
{
  MAPS_UNKNOWN,
  MAPS_EXMY,  // E1M1: Doom, Heretic
  MAPS_MAPXX, // MAP01: Doom II
  MAPS_HEXEN, // MAP01 with scripts (BEHAVIOR): Hexen
};

static const struct { const char *file, *name; int maps; const char *extra; } iwad_names[] =
{
  { "doom.wad",      "The Ultimate Doom",                   MAPS_EXMY },
  { "doom1.wad",     "Doom (shareware)",                    MAPS_EXMY },
  { "doom2.wad",     "Doom II",                             MAPS_MAPXX },
  { "doom2f.wad",    "Doom II (French)",                    MAPS_MAPXX },
  { "plutonia.wad",  "Final Doom: The Plutonia Experiment", MAPS_MAPXX },
  { "tnt.wad",       "Final Doom: TNT: Evilution",          MAPS_MAPXX },
  { "freedoom1.wad", "Freedoom: Phase 1",                   MAPS_EXMY },
  { "freedoom2.wad", "Freedoom: Phase 2",                   MAPS_MAPXX },
  { "freedm.wad",    "FreeDM",                              MAPS_MAPXX },
  { "chex.wad",      "Chex Quest",                          MAPS_EXMY },
  { "hacx.wad",      "HACX",                                MAPS_MAPXX },
  { "rekkrsa.wad",   "REKKR",                               MAPS_EXMY },
  { "heretic.wad",   "Heretic",                             MAPS_EXMY },
  { "heretic1.wad",  "Heretic (shareware)",                 MAPS_EXMY },
  // The remaster's (2025) PWADs on its heretic.wad: the maps as it fixed
  // them, and its new episode.
  { "heretic.wad",   "Heretic (Enhanced Maps)",             MAPS_EXMY, "heretic_ex.wad" },
  { "heretic.wad",   "Heretic: Faith Renewed",              MAPS_EXMY, "heretic_fr.wad" },
  { "hexen.wad",     "Hexen",                               MAPS_HEXEN },
  // The expansion is a PWAD on top of Hexen.
  { "hexen.wad",     "Hexen: Deathkings of the Dark Citadel", MAPS_HEXEN, "hexdd.wad" },
  // The remaster's (2025) PWADs on its hexen.wad: the maps as it fixed them
  // (Deathkings' with the whole expansion), and its new hub.
  { "hexen.wad",     "Hexen (Enhanced Maps)",               MAPS_HEXEN, "hexen_ex.wad" },
  { "hexen.wad",     "Deathkings (Enhanced Maps)",          MAPS_HEXEN, "hexdd_ex.wad" },
  { "hexen.wad",     "Hexen: Vestiges of Grandeur",         MAPS_HEXEN, "hexen_vog.wad" },
};

// The mods of each game in a folder of their own: Doom's in "mods",
// Heretic's in "mods/heretic", Hexen's in "mods/hexen" (the Heretic
// title's, in its own folder's "mods").
#define MODS_FOLDER "mods"
#define MAX_MODS 128
#define LAST_GAME_FILE "last-game.txt"
#define VISIBLE_ITEMS 9

#define ConsolePrintf(...) (fprintf(console, __VA_ARGS__), fflush(console))
#define ConsoleClear() ConsolePrintf("\x1b[2J")

static dboolean FileExists(const char *path)
{
  struct stat st;

  return stat(path, &st) == 0;
}

static dboolean IsModFile(const char *name)
{
  const char *dot = strrchr(name, '.');

  return dot != NULL && (!strcasecmp(dot, ".wad") || !strcasecmp(dot, ".deh")
                      || !strcasecmp(dot, ".bex") || !strcasecmp(dot, ".zip"));
}

// A mod's kind, from what is in it: a campaign (maps: one at a time) or
// an add-on (gameplay, graphics, music...: any number, over the campaign).
enum
{
  MOD_CAMPAIGN,
  MOD_GAMEPLAY,
  MOD_SPRITES,
  MOD_TEXTURES,
  MOD_MUSIC,
  MOD_SOUNDS,
  MOD_INTERFACE,
  MOD_OTHER,
  MOD_BROKEN, // not a WAD (only named like one): it cannot load
  MOD_KINDS
};

static const char *ModsFolder(const char *iwad)
{
  return only_game ? MODS_FOLDER
       : !strncasecmp(iwad, "heretic", 7) ? MODS_FOLDER "/heretic"
       : !strncasecmp(iwad, "hexen", 5) ? MODS_FOLDER "/hexen"
       : MODS_FOLDER;
}

// In the Heretic and Hexen titles, only that game.
static dboolean IsThisTitlesGame(int iwad)
{
  return only_game == NULL || !strncasecmp(iwad_names[iwad].file, only_game, strlen(only_game));
}

static const char *const mod_kind_names[MOD_KINDS] =
{
  "Campaign", "Gameplay", "Sprites", "Textures", "Music", "Sounds", "Interface", "Other", "Not a WAD"
};

typedef struct
{
  char *file;
  int kind;
  int maps; // a campaign's: MAPS_*
  dboolean deh; // a DeHackEd patch, or one in it: changes the game itself
  dboolean gzdoom; // things in GZDoom's languages only, no patch: not here
  char start[5]; // what a file that is not a WAD starts with, for the log
} mod_t;

static dboolean IsMapName(const char *name)
{
  return (name[0] == 'M' && name[1] == 'A' && name[2] == 'P' && isdigit((unsigned char) name[3])
          && isdigit((unsigned char) name[4]) && name[5] == '\0')
      || (name[0] == 'E' && isdigit((unsigned char) name[1]) && name[2] == 'M'
          && isdigit((unsigned char) name[3]) && name[4] == '\0');
}

static dboolean StartsWith(const char *name, const char *prefix)
{
  return strncmp(name, prefix, strlen(prefix)) == 0;
}

// A WAD's kind from its lump names (only its directory is read): maps make
// a campaign; a DeHackEd patch, gameplay; otherwise what most of it is. A campaign's first map tells how its
// maps are named.
static int WadKind(const char *path, int *maps, dboolean *deh, dboolean *gzdoom, char *start)
{
  typedef struct { int pos, size; char name[8]; } entry_t;
  FILE *file = fopen(path, "rb");
  struct { char id[4]; int count, offset; } header;
  int counts[MOD_KINDS] = { 0 };
  int kind = MOD_OTHER, best = 0, i;
  dboolean in_sprites = false, in_flats = false, in_patches = false, campaign = false;
  dboolean exmy = false, mapxx = false;
  entry_t *entries;

  *maps = MAPS_UNKNOWN;
  *deh = false;
  *gzdoom = false;
  memset(&header, 0, sizeof(header));

  if (file == NULL)
    return MOD_BROKEN;

  if (fread(&header, sizeof(header), 1, file) != 1
      || (memcmp(header.id, "PWAD", 4) && memcmp(header.id, "IWAD", 4))
      || header.count <= 0 || header.count > 65536
      || fseek(file, header.offset, SEEK_SET) != 0
      || (entries = malloc(header.count * sizeof(*entries))) == NULL)
  {
    for (i = 0; i < 4; ++i)
      start[i] = isprint((unsigned char) header.id[i]) ? header.id[i] : '.';
    start[4] = '\0';

    fclose(file);
    return MOD_BROKEN;
  }

  // The directory in one read: an entry at a time is slow on the SD card.
  header.count = fread(entries, sizeof(*entries), header.count, file);
  fclose(file);

  for (i = 0; i < header.count; ++i)
  {
    char name[9];

    memcpy(name, entries[i].name, 8);
    name[8] = '\0';

    if (IsMapName(name))
    {
      if (name[0] == 'E')
        exmy = true;
      else
        mapxx = true;

      if (*maps == MAPS_UNKNOWN)
      {
        int j;

        *maps = name[0] == 'E' ? MAPS_EXMY : MAPS_MAPXX;

        // A map's lumps follow its marker: scripts make it Hexen's, a
        // text map (UDMF) could be any game's.
        for (j = i + 1; j < header.count && j <= i + 12; ++j)
        {
          if (!strncmp(entries[j].name, "BEHAVIOR", 8))
            *maps = MAPS_HEXEN;
          else if (!strncmp(entries[j].name, "TEXTMAP", 8))
            *maps = MAPS_UNKNOWN;
          else
            continue;
          break;
        }
      }

      campaign = true;
    }
    // Map information alone is no campaign: gameplay mods bring it for
    // other ports (Doom 4 Vanilla), with no maps.
    else if (!strcmp(name, "TEXTMAP"))
      campaign = true;
    else if (!strcmp(name, "S_START") || !strcmp(name, "SS_START"))
      in_sprites = true;
    else if (!strcmp(name, "S_END") || !strcmp(name, "SS_END"))
      in_sprites = false;
    else if (!strcmp(name, "F_START") || !strcmp(name, "FF_START"))
      in_flats = true;
    else if (!strcmp(name, "F_END") || !strcmp(name, "FF_END"))
      in_flats = false;
    else if (!strcmp(name, "P_START") || !strcmp(name, "PP_START"))
      in_patches = true;
    else if (!strcmp(name, "P_END") || !strcmp(name, "PP_END"))
      in_patches = false;
    else if (!strcmp(name, "DEHACKED"))
      *deh = true;
    else if (!strcmp(name, "DECORATE") || !strcmp(name, "ZSCRIPT"))
      *gzdoom = true;
    else if (in_sprites)
      ++counts[MOD_SPRITES];
    else if (in_flats || in_patches || !strcmp(name, "TEXTURE1")
             || !strcmp(name, "TEXTURE2") || !strcmp(name, "PNAMES"))
      ++counts[MOD_TEXTURES];
    else if (StartsWith(name, "D_") || StartsWith(name, "MUS_"))
      ++counts[MOD_MUSIC];
    else if (StartsWith(name, "DS") || StartsWith(name, "DP"))
      ++counts[MOD_SOUNDS];
    else if (StartsWith(name, "ST") || StartsWith(name, "M_") || StartsWith(name, "WI")
             || StartsWith(name, "FONT") || !strcmp(name, "TITLEPIC")
             || !strcmp(name, "INTERPIC") || !strcmp(name, "CREDIT")
             || !strcmp(name, "HELP") || !strcmp(name, "HELP1"))
      ++counts[MOD_INTERFACE];
  }

  free(entries);

  // With a patch too, it is made for both kinds of ports.
  if (*deh)
    *gzdoom = false;

  // Maps of both namings (E1M8 and MAP07): for every game (Doom 4 Vanilla
  // fixes the boss maps of each).
  if (exmy && mapxx)
    *maps = MAPS_UNKNOWN;

  if (campaign)
    return MOD_CAMPAIGN;
  if (*deh)
    return MOD_GAMEPLAY;

  for (i = MOD_GAMEPLAY; i < MOD_OTHER; ++i)
  {
    if (counts[i] > best)
    {
      best = counts[i];
      kind = i;
    }
  }

  return kind;
}

static dboolean IsPatchFile(const char *name)
{
  const char *dot = strrchr(name, '.');

  return dot != NULL && (!strcasecmp(dot, ".deh") || !strcasecmp(dot, ".bex"));
}

static int ModKind(const char *folder, const char *name, int *maps, dboolean *deh,
                   dboolean *gzdoom, char *start)
{
  const char *dot = strrchr(name, '.');
  char path[300];

  *maps = MAPS_UNKNOWN;
  *deh = IsPatchFile(name);

  if (*deh)
    return MOD_GAMEPLAY;

  // A zip is not looked into: a campaign, the safest.
  if (!strcasecmp(dot, ".zip"))
    return MOD_CAMPAIGN;

  snprintf(path, sizeof(path), "%s/%s/%s", data_dir, folder, name);
  return WadKind(path, maps, deh, gzdoom, start);
}

static int CompareMods(const void *a, const void *b)
{
  return strcasecmp(((const mod_t *) a)->file, ((const mod_t *) b)->file);
}

static int FindMods(mod_t *mods, const char *folder)
{
  char path[128];
  DIR *dir;
  struct dirent *entry;
  int count = 0, i;

  snprintf(path, sizeof(path), "%s/%s", data_dir, folder);
  dir = opendir(path);

  if (dir == NULL)
    return 0;

  while (count < MAX_MODS && (entry = readdir(dir)) != NULL)
    if (entry->d_name[0] != '.' && IsModFile(entry->d_name))
    {
      memset(&mods[count], 0, sizeof(mods[count]));
      mods[count++].file = strdup(entry->d_name);
    }

  closedir(dir);
  qsort(mods, count, sizeof(*mods), CompareMods);

  for (i = 0; i < count; ++i)
    mods[i].kind = ModKind(folder, mods[i].file, &mods[i].maps, &mods[i].deh, &mods[i].gzdoom,
                           mods[i].start);

  // A WAD with a patch file of its name (D4V.wad and D4V.deh) is the
  // graphics of a gameplay mod: it loads with gameplay, after the campaign.
  // Maps in it are fixes of the game's own (D4V.wad's boss maps), no
  // campaign.
  for (i = 0; i < count; ++i)
  {
    const size_t length = strrchr(mods[i].file, '.') - mods[i].file;
    int j;

    if (mods[i].kind == MOD_BROKEN || IsPatchFile(mods[i].file))
      continue;

    for (j = 0; j < count; ++j)
      if (IsPatchFile(mods[j].file) && strrchr(mods[j].file, '.') - mods[j].file == length
          && !strncasecmp(mods[i].file, mods[j].file, length))
        mods[i].kind = MOD_GAMEPLAY, mods[i].maps = MAPS_UNKNOWN, mods[i].gzdoom = false;
  }

  return count;
}

// After the menu was left with a button that may still be held: wait for
// every button to be released before reading the next one.
static u32 KeysDown(dboolean *released)
{
  hidScanInput();

  if (!*released)
  {
    *released = hidKeysHeld() == 0;
    return 0;
  }

  return hidKeysDown();
}

// A list on the top screen. Returns the chosen item, -1 for B (back) and
// -2 for START (quit).
static int ChooseItem(const char *title, const char *const *items, int count,
                      int selected, const char *help)
{
  dboolean released = false;
  int drawn = -1, first = 0, i;

  while (aptMainLoop())
  {
    const u32 down = KeysDown(&released);

    if (down & (KEY_DOWN | KEY_CPAD_DOWN))
      selected = (selected + 1) % count;
    if (down & (KEY_UP | KEY_CPAD_UP))
      selected = (selected + count - 1) % count;
    if (down & KEY_A)
      return selected;
    if (down & KEY_B)
      return -1;
    if (down & KEY_START)
      return -2;

    if (selected != drawn)
    {
      if (selected < first)
        first = selected;
      else if (selected >= first + VISIBLE_ITEMS)
        first = selected - VISIBLE_ITEMS + 1;

      ConsoleClear();
      ConsolePrintf("\n  \x1b[31m%s\x1b[0m\n\n  %s\n\n", picker_title, title);
      ConsolePrintf("  %s\n", first > 0 ? "  ..." : "");

      for (i = first; i < count && i < first + VISIBLE_ITEMS; ++i)
        ConsolePrintf(i == selected ? "  \x1b[33m> %.44s\x1b[0m\n\n"
                                    : "    %.44s\n\n", items[i]);

      ConsolePrintf("  %s\n", i < count ? "  ..." : "");
      ConsolePrintf("\n  %s\n", help);
      drawn = selected;
    }

    gspWaitForVBlank();
  }

  return -2;
}

// The add-ons, any number of them: A marks or unmarks one. Returns 1 to
// start, -1 for B (back) and -2 to quit (the HOME Menu closing the game).
static int ChooseAddons(const char *title, mod_t *const *addons, dboolean *checked, int count)
{
  dboolean released = false, redraw = true;
  int selected = 0, first = 0, i;

  while (aptMainLoop())
  {
    const u32 down = KeysDown(&released);

    if (down & (KEY_DOWN | KEY_CPAD_DOWN))
      selected = (selected + 1) % count, redraw = true;
    if (down & (KEY_UP | KEY_CPAD_UP))
      selected = (selected + count - 1) % count, redraw = true;
    if ((down & KEY_A) && addons[selected]->kind != MOD_BROKEN)
      checked[selected] = !checked[selected], redraw = true;
    if (down & KEY_START)
      return 1;
    if (down & KEY_B)
      return -1;

    if (redraw)
    {
      if (selected < first)
        first = selected;
      else if (selected >= first + VISIBLE_ITEMS)
        first = selected - VISIBLE_ITEMS + 1;

      ConsoleClear();
      ConsolePrintf("\n  \x1b[31m%s\x1b[0m\n\n  %.44s\n  Add-ons (any number):\n",
                    picker_title, title);
      ConsolePrintf("  %s\n", first > 0 ? "  ..." : "");

      for (i = first; i < count && i < first + VISIBLE_ITEMS; ++i)
        ConsolePrintf(i == selected ? "  \x1b[33m> [%c] %-10s %.27s\x1b[0m\n\n"
                                    : "    [%c] %-10s %.27s\n\n",
                      addons[i]->kind == MOD_BROKEN ? '-' : checked[i] ? 'x' : ' ', mod_kind_names[addons[i]->kind], addons[i]->file);

      ConsolePrintf("  %s\n", i < count ? "  ..." : "");
      ConsolePrintf("\n  A: mark  START: play  B: back\n");
      redraw = false;
    }

    gspWaitForVBlank();
  }

  return -2;
}

// A patch made for another mod is named after it: SD21-BloodPatch.wad
// for SD21.wad. It loads right after that mod, when that one is chosen too
// (and both are WADs, or both patch files).
static mod_t *ParentOf(const mod_t *mod, mod_t *const *chosen, int count)
{
  int i;

  for (i = 0; i < count; ++i)
  {
    const size_t length = strrchr(chosen[i]->file, '.') - chosen[i]->file;

    if (chosen[i] != mod && length < strlen(mod->file)
        && !strncasecmp(mod->file, chosen[i]->file, length)
        && strchr("-_ ", mod->file[length]) != NULL
        && IsPatchFile(mod->file) == IsPatchFile(chosen[i]->file))
      return chosen[i];
  }

  return NULL;
}

// A mod and, after it, the patches made for it.
static void AddWithPatches(mod_t *mod, mod_t *const *chosen, int count, mod_t **order, int *num)
{
  int i;

  order[(*num)++] = mod;

  for (i = 0; i < count; ++i)
    if (ParentOf(chosen[i], chosen, count) == mod)
      AddWithPatches(chosen[i], chosen, count, order, num);
}

// The order the game loads the chosen mods in. Fixes and looks (sprites,
// textures, sounds, the interface) before the campaign: what it brings of
// its own wins, as their authors ask. What is meant to replace (gameplay,
// music, the rest) after it. DeHackEd patch files last: the game reads
// them after the patches in WADs. First of them, the ones of a chosen WAD
// of their name (D4V.deh of D4V.wad): the others (extra weapons) go over.
// A mod's twin: the WAD of a patch file of its name, or the other way.
static mod_t *FindTwin(const mod_t *mod, mod_t *const *list, int count)
{
  const size_t length = strrchr(mod->file, '.') - mod->file;
  int i;

  for (i = 0; i < count; ++i)
    if (IsPatchFile(list[i]->file) != IsPatchFile(mod->file)
        && list[i]->kind != MOD_BROKEN
        && strrchr(list[i]->file, '.') - list[i]->file == length
        && !strncasecmp(list[i]->file, mod->file, length))
      return list[i];

  return NULL;
}

static dboolean HasTwin(const mod_t *patch, mod_t *const *chosen, int count)
{
  return FindTwin(patch, chosen, count) != NULL;
}

static int LoadOrder(mod_t *campaign, mod_t *const *addons, const dboolean *checked,
                     int count, mod_t **order)
{
  mod_t *chosen[MAX_MODS + 1];
  int num_chosen = 0, num = 0, pass, i;

  if (campaign != NULL)
    chosen[num_chosen++] = campaign;
  for (i = 0; i < count; ++i)
    if (checked[i])
      chosen[num_chosen++] = addons[i];

  for (pass = 0; pass < 4; ++pass)
  {
    if (pass == 1 && campaign != NULL)
      AddWithPatches(campaign, chosen, num_chosen, order, &num);

    for (i = 0; i < count; ++i)
    {
      const int kind = addons[i]->kind;
      const dboolean first = kind == MOD_SPRITES || kind == MOD_TEXTURES
                          || kind == MOD_SOUNDS || kind == MOD_INTERFACE;
      const int wanted = !IsPatchFile(addons[i]->file) ? (first ? 0 : 1)
                       : HasTwin(addons[i], chosen, num_chosen) ? 2 : 3;

      if (checked[i] && wanted == pass && ParentOf(addons[i], chosen, num_chosen) == NULL)
        AddWithPatches(addons[i], chosen, num_chosen, order, &num);
    }
  }

  return num;
}

// A mod with a patch that is not part of another chosen one: a patch made
// for it (SD21-BloodPatch), or the patch file of a WAD (D4V.deh of D4V.wad).
static dboolean IsLonePatch(const mod_t *mod, mod_t *const *chosen, int count)
{
  return mod->deh && ParentOf(mod, chosen, count) == NULL
      && !(IsPatchFile(mod->file) && HasTwin(mod, chosen, count));
}

static dboolean IsIn(const mod_t *mod, mod_t *const *list, int count)
{
  int i;

  for (i = 0; i < count; ++i)
    if (list[i] == mod)
      return true;

  return false;
}

#define MAX_WARNED 4

// What may not work in the chosen mods, on one screen. A warning, not a
// block: the picker only guesses from the names inside the files.
// - Mods changing the game itself (DeHackEd: weapons, monsters) step on
//   each other: where both change the same, the last one loaded wins.
//   Some go well together (a fix with a campaign); a patch made for
//   another mod goes with it.
// - A piece without the rest of its mod (D4V.deh without D4V.wad: its
//   graphics are missing, the game stops at the first of them).
// - Mods for GZDoom only (DECORATE, ZSCRIPT, no patch): their new things
//   do not show here.
// 1 to play anyway, -1 to go back, -2 when closed.
static int WarnChoice(mod_t *const *order, int count, mod_t *const *all, int num_all)
{
  dboolean released = false;
  int patches = 0, missing = 0, gzdoom = 0, shown, i;

  for (i = 0; i < count; ++i)
  {
    const mod_t *twin = FindTwin(order[i], all, num_all);
    const mod_t *parent = ParentOf(order[i], all, num_all);

    patches += IsLonePatch(order[i], order, count);
    missing += (twin != NULL && !IsIn(twin, order, count))
            || (parent != NULL && !IsIn(parent, order, count));
    gzdoom += order[i]->gzdoom;
  }

  if (patches < 2 && missing == 0 && gzdoom == 0)
    return 1;

  ConsoleClear();
  ConsolePrintf("\n  \x1b[31m%s\x1b[0m\n\n  \x1b[33mWarning:\x1b[0m these mods may not work.\n",
                picker_title);

  if (missing > 0)
  {
    ConsolePrintf("\n  Part of a mod, without the rest of it:\n");

    for (i = 0, shown = 0; i < count && shown < MAX_WARNED; ++i)
    {
      const mod_t *twin = FindTwin(order[i], all, num_all);
      const mod_t *parent = ParentOf(order[i], all, num_all);
      const mod_t *needed = twin != NULL && !IsIn(twin, order, count) ? twin
                          : parent != NULL && !IsIn(parent, order, count) ? parent : NULL;

      if (needed != NULL)
      {
        ConsolePrintf("    %.20s: mark %.20s\n", order[i]->file, needed->file);
        ++shown;
      }
    }
  }

  if (gzdoom > 0)
  {
    ConsolePrintf("\n  For GZDoom: their new things won't show:\n");

    for (i = 0, shown = 0; i < count && shown < MAX_WARNED; ++i)
      if (order[i]->gzdoom)
      {
        ConsolePrintf("    %.44s\n", order[i]->file);
        ++shown;
      }
  }

  if (patches >= 2)
  {
    // Three or more rarely go together: the animations of one end up with
    // the actions of another (a weapon's frame running a monster's action
    // crashed the game).
    if (patches >= 3)
      ConsolePrintf("\n  All change the game itself (DeHackEd):\n"
                    "  \x1b[31mso many together can crash the game:\x1b[0m\n");
    else
      ConsolePrintf("\n  All change the game itself (DeHackEd);\n"
                    "  the last one wins where they meet:\n");

    for (i = 0, shown = 0; i < count && shown < MAX_WARNED; ++i)
      if (IsLonePatch(order[i], order, count))
      {
        ConsolePrintf("    %-10s %.32s\n", mod_kind_names[order[i]->kind], order[i]->file);
        ++shown;
      }
  }

  ConsolePrintf("\n  A: play anyway  B: back\n");

  while (aptMainLoop())
  {
    const u32 down = KeysDown(&released);

    if (down & KEY_A)
      return 1;
    if (down & KEY_B)
      return -1;

    gspWaitForVBlank();
  }

  return -2;
}

// A game's IWAD left in a folder of the data folder (heretic/, mods/...)
// instead of in it, or for the Heretic and Hexen titles in DSDA-Doom's
// folder: where, for the screen.
static dboolean FindMisplacedIwad(char *found, size_t size)
{
  static const char *const folders[] = { "heretic", "hexen", "doom", "doom2", "wads", "iwads",
                                         MODS_FOLDER, MODS_FOLDER "/heretic",
                                         MODS_FOLDER "/hexen", N3DS_DATA_DIR,
                                         N3DS_DATA_DIR "/heretic",
                                         N3DS_DATA_DIR "/" MODS_FOLDER "/heretic",
                                         N3DS_DATA_DIR "/hexen",
                                         N3DS_DATA_DIR "/" MODS_FOLDER "/hexen" };
  enum { num_iwads = sizeof(iwad_names) / sizeof(iwad_names[0]) };
  int f, i;

  for (f = 0; f < (int) (sizeof(folders) / sizeof(folders[0])); ++f)
    for (i = 0; i < num_iwads; ++i)
    {
      if (!IsThisTitlesGame(i))
        continue;

      // Absolute (another title's folder) or in this one's.
      if (!strncmp(folders[f], "sdmc:", 5))
      {
        if (only_game == NULL)
          continue;
        snprintf(found, size, "%s/%s", folders[f], iwad_names[i].file);
      }
      else
        snprintf(found, size, "%s/%s/%s", data_dir, folders[f], iwad_names[i].file);

      if (FileExists(found))
        return true;
    }

  return false;
}

static void ShowNoGame(void)
{
  dboolean released = false;
  char misplaced[160];

  ConsoleClear();

  // Found, in the wrong place: the most likely mistake, said first.
  if (FindMisplacedIwad(misplaced, sizeof(misplaced)))
  {
    ConsolePrintf("\n  \x1b[31m%s\x1b[0m\n\n"
                  "  The game is in another folder:\n\n"
                  "  \x1b[33m%.88s\x1b[0m\n\n"
                  "  Move it directly to\n"
                  "  %s/\n"
                  "  Only mods go in a folder there (mods/).\n\n"
                  "  START: exit\n", picker_title, misplaced, data_dir);

    while (aptMainLoop() && (KeysDown(&released) & KEY_START) == 0)
      gspWaitForVBlank();
    return;
  }

  if (only_game != NULL && !strcmp(only_game, "hexen"))
  {
    ConsolePrintf("\n  \x1b[31m%s\x1b[0m\n\n"
                  "  No game data found. Copy Hexen directly\n"
                  "  to %s/\n\n"
                  "  hexen.wad (the full game: the demo\n"
                  "  does not work), and hexdd.wad for\n"
                  "  Deathkings of the Dark Citadel.\n\n"
                  "  Mods (.wad, .zip) go in\n"
                  "  %s/" MODS_FOLDER "/\n\n"
                  "  START: exit\n", picker_title, data_dir, data_dir);

    while (aptMainLoop() && (KeysDown(&released) & KEY_START) == 0)
      gspWaitForVBlank();
    return;
  }

  if (only_game != NULL)
  {
    ConsolePrintf("\n  \x1b[31m%s\x1b[0m\n\n"
                  "  No game data found. Copy Heretic directly\n"
                  "  to %s/\n\n"
                  "  heretic.wad (or heretic1.wad, the\n"
                  "  shareware).\n\n"
                  "  Mods (.wad, .zip) go in\n"
                  "  %s/" MODS_FOLDER "/\n\n"
                  "  START: exit\n", picker_title, data_dir, data_dir);

    while (aptMainLoop() && (KeysDown(&released) & KEY_START) == 0)
      gspWaitForVBlank();
    return;
  }

  ConsolePrintf("\n  \x1b[31m%s\x1b[0m\n\n"
                "  No game data found. Copy an IWAD directly\n"
                "  to %s/\n\n"
                "  doom.wad, doom1.wad (shareware), doom2.wad,\n"
                "  plutonia.wad, tnt.wad, freedoom1.wad,\n"
                "  heretic.wad, hexen.wad...\n\n"
                "  Mods (.wad, .deh, .zip) go in\n"
                "  %s/" MODS_FOLDER "/ (Heretic's\n"
                "  in heretic/ there, Hexen's in hexen/)\n\n"
                "  START: exit\n", picker_title, data_dir, data_dir);

  while (aptMainLoop() && (KeysDown(&released) & KEY_START) == 0)
    gspWaitForVBlank();
}

// Quit in the game: this title opens again, with the picker. Not when the
// HOME Menu closes it.
static void BackToPicker(void)
{
  if (!N3DS_IsClosing())
    aptSetChainloaderToSelf();
}

static void AddArgFile(char **args, int *count, const char *folder, const char *file)
{
  char path[300];

  snprintf(path, sizeof(path), "%s/%s", folder, file);
  args[(*count)++] = strdup(path);
}

// The chosen WADs whose patch file was chosen too: their own patches are
// skipped (as -nodeh would, for them only: a campaign's patch stays).
static const char *skip_deh_wads[MAX_MODS];
static int num_skip_deh_wads;

dboolean N3DS_SkipDehLump(const char *wad_path)
{
  const char *slash = strrchr(wad_path, '/');
  const char *name = slash != NULL ? slash + 1 : wad_path;
  int i;

  for (i = 0; i < num_skip_deh_wads; ++i)
    if (!strcasecmp(name, skip_deh_wads[i]))
    {
      printf("Skipping the DEH lump of %s: its own patch file replaces it\n", name);
      return true;
    }

  return false;
}

// The campaigns for a game (the first item is none), the chosen one
// selected when it is there.
static void ListCampaigns(int maps, mod_t *const *all, int count, const char *chosen,
                          mod_t **campaigns, const char **names, int *num, int *selected)
{
  int i;

  names[0] = "No campaign (the game itself)";
  campaigns[0] = NULL;
  *num = 1;
  *selected = 0;

  for (i = 0; i < count; ++i)
  {
    if (all[i]->maps != MAPS_UNKNOWN && all[i]->maps != maps)
      continue;

    if (!strcasecmp(all[i]->file, chosen))
      *selected = *num;

    names[*num] = all[i]->file;
    campaigns[(*num)++] = all[i];
  }
}

// The picker's mods: those of the chosen game's folder, loaded again when
// the game changes to one with another folder. The add-ons marked last
// time stay marked.
static mod_t mods[MAX_MODS];
static mod_t *all_mods[MAX_MODS], *all_campaigns[MAX_MODS], *addons[MAX_MODS];
static dboolean checked[MAX_MODS];
static int num_mods, num_all_campaigns, num_addons;
static const char *mods_folder;
static char last_addons[MAX_MODS][256];
static int num_last_addons;

static void LoadMods(const char *folder)
{
  int i, k;

  if (mods_folder != NULL && !strcmp(mods_folder, folder))
    return;

  for (i = 0; i < num_mods; ++i)
    free(mods[i].file);

  mods_folder = folder;
  num_mods = FindMods(mods, folder);
  num_all_campaigns = num_addons = 0;

  for (i = 0; i < num_mods; ++i)
  {
    all_mods[i] = &mods[i];

    if (mods[i].kind == MOD_CAMPAIGN)
      all_campaigns[num_all_campaigns++] = &mods[i];
    else
    {
      checked[num_addons] = false;

      for (k = 0; k < num_last_addons; ++k)
        if (!strcasecmp(mods[i].file, last_addons[k]) && mods[i].kind != MOD_BROKEN)
          checked[num_addons] = true;

      addons[num_addons++] = &mods[i];
    }
  }
}

dboolean N3DS_PickGame(int *argc, char ***argv)
{
  enum { num_iwads = sizeof(iwad_names) / sizeof(iwad_names[0]) };
  static char *args[16 + MAX_MODS];
  mod_t *order[MAX_MODS + 1];
  const char *games[num_iwads];
  int game_index[num_iwads];
  const char *campaign_names[MAX_MODS + 1];
  mod_t *campaigns[MAX_MODS + 1];
  char last_game[96] = "", chosen[256] = "", line[256];
  int num_games = 0, num_campaigns = 1;
  int game = 0, campaign = 0, step = 0, num_order, i;
  dboolean shown = false;
  FILE *file;

  for (i = 0; i < num_iwads; ++i)
  {
    char path[128];

    snprintf(path, sizeof(path), "%s/%s", data_dir,
             iwad_names[i].extra != NULL ? iwad_names[i].extra : iwad_names[i].file);

    if (!IsThisTitlesGame(i))
      continue;

    if (FileExists(path)
        && (iwad_names[i].extra == NULL
            || FileExists(iwad_names[i].file)))
    {
      games[num_games] = iwad_names[i].name;
      game_index[num_games++] = i;
    }
  }

  // The last choice comes first: the game, the campaign, the add-ons.
  file = fopen(LAST_GAME_FILE, "r");

  if (file != NULL)
  {
    if (fgets(last_game, sizeof(last_game), file) != NULL)
      last_game[strcspn(last_game, "\r\n")] = '\0';
    if (fgets(chosen, sizeof(chosen), file) != NULL)
      chosen[strcspn(chosen, "\r\n")] = '\0';

    while (num_last_addons < MAX_MODS && fgets(line, sizeof(line), file) != NULL)
    {
      line[strcspn(line, "\r\n")] = '\0';
      snprintf(last_addons[num_last_addons++], sizeof(last_addons[0]), "%s", line);
    }

    fclose(file);
  }

  for (i = 0; i < num_games; ++i)
    if (!strcmp(games[i], last_game))
      game = i;

  // The mods of that game: campaigns and add-ons.
  LoadMods(ModsFolder(num_games > 0 ? iwad_names[game_index[game]].file : ""));

  ListCampaigns(iwad_names[game_index[game]].maps, all_campaigns, num_all_campaigns,
                chosen, campaigns, campaign_names, &num_campaigns, &campaign);

  if (num_games != 1 || num_mods > 0)
  {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    shown = true;

    if (num_games == 0)
    {
      ShowNoGame();
      gfxExit();
      return false;
    }

    // Step by step: the game, the campaign, the add-ons. B goes a step
    // back (where there was a choice).
    for (;;)
    {
      if (step == 0)
      {
        if (num_games > 1)
        {
          game = ChooseItem("Choose a game:", games, num_games, game, "A: choose  START: exit");

          if (game < 0)
          {
            gfxExit();
            return false;
          }

          LoadMods(ModsFolder(iwad_names[game_index[game]].file));
          ListCampaigns(iwad_names[game_index[game]].maps, all_campaigns, num_all_campaigns,
                        chosen, campaigns, campaign_names, &num_campaigns, &campaign);
        }

        step = 1;
      }

      if (step == 1)
      {
        if (num_campaigns > 1)
        {
          const int choice = ChooseItem(games[game], campaign_names, num_campaigns, campaign,
                                        num_addons > 0 ? "A: next  B: back  START: exit"
                                                       : "A: start  B: back  START: exit");

          if (choice == -2)
          {
            gfxExit();
            return false;
          }

          if (choice == -1)
          {
            if (num_games > 1)
              step = 0;
            continue;
          }

          campaign = choice;
          snprintf(chosen, sizeof(chosen), "%s", campaign > 0 ? campaigns[campaign]->file : "");
        }

        step = 2;
      }

      if (step == 2)
      {
        char title[96];
        int choice;

        if (num_addons == 0)
          break;

        snprintf(title, sizeof(title), "%s%s%s", games[game], campaign > 0 ? " + " : "",
                 campaign > 0 ? campaigns[campaign]->file : "");
        choice = ChooseAddons(title, addons, checked, num_addons);

        if (choice == 1)
        {
          choice = WarnChoice(order, LoadOrder(campaigns[campaign], addons, checked,
                                               num_addons, order),
                              all_mods, num_mods);

          if (choice == -1)
            continue; // back to the add-ons
        }

        if (choice == -2)
        {
          gfxExit();
          return false;
        }

        if (choice == -1)
        {
          step = num_campaigns > 1 ? 1 : num_games > 1 ? 0 : 2;
          continue;
        }

        break;
      }
    }

    gfxExit(); // SDL sets the screens up again
  }

  file = N3DS_OpenRewrite(LAST_GAME_FILE);

  if (file != NULL)
  {
    fprintf(file, "%s\n%s\n", games[game], campaign > 0 ? campaigns[campaign]->file : "");

    for (i = 0; i < num_addons; ++i)
      if (checked[i])
        fprintf(file, "%s\n", addons[i]->file);

    N3DS_CloseRewrite(file);
  }

  // What is played, in the order it loads.
  num_order = LoadOrder(campaigns[campaign], addons, checked, num_addons, order);

  for (i = 0; i < num_order; ++i)
    if (IsPatchFile(order[i]->file) && HasTwin(order[i], order, num_order))
    {
      const size_t length = strrchr(order[i]->file, '.') - order[i]->file;

      for (int k = 0; k < num_order; ++k)
        if (!IsPatchFile(order[k]->file) && !strncasecmp(order[k]->file, order[i]->file, length)
            && strrchr(order[k]->file, '.') - order[k]->file == length)
          skip_deh_wads[num_skip_deh_wads++] = order[k]->file;
    }

  StartLog();
  printf("N3DS_PickGame: %s", games[game]);
  for (i = 0; i < num_order; ++i)
    printf(" + %s (%s)", order[i]->file, mod_kind_names[order[i]->kind]);
  printf("\n");
  if (num_mods > 0)
  {
    printf("N3DS_PickGame: mods found:");
    for (i = 0; i < num_mods; ++i)
      printf(" %s (%s)%s", mods[i].file, mod_kind_names[mods[i].kind], i + 1 < num_mods ? "," : "\n");
  }
  for (i = 0; i < num_addons; ++i)
    if (addons[i]->kind == MOD_BROKEN)
      printf("N3DS_PickGame: %s is not a WAD: it starts with \"%s\", not \"PWAD\"\n",
             addons[i]->file, addons[i]->start);

  // The program's name, the game, its expansion and the mods in their
  // order.
  {
    const int entry = game_index[game];

    i = 0;
    // A .cia starts with no arguments at all (no program name either).
    args[i++] = (*argc > 0 && *argv != NULL && (*argv)[0] != NULL) ? (*argv)[0] : "dsda-doom";
    args[i++] = "-iwad";
    args[i++] = (char *) iwad_names[entry].file;
    args[i++] = "-file";

    if (iwad_names[entry].extra != NULL)
      args[i++] = (char *) iwad_names[entry].extra;

    for (int k = 0; k < num_order; ++k)
    {
      // The patch files are last, after -deh. Nothing after -file: it goes.
      if (IsPatchFile(order[k]->file) && !IsPatchFile(args[i - 1]))
      {
        if (!strcmp(args[i - 1], "-file"))
          --i;
        args[i++] = "-deh";
      }

      AddArgFile(args, &i, mods_folder, order[k]->file);
    }

    if (!strcmp(args[i - 1], "-file"))
      --i;
  }

  args[i] = NULL;
  *argc = i;
  *argv = args;

  // Quit goes back to the picker when there was something to pick and this
  // is the installed .cia (relaunching a .3dsx would restart the Homebrew
  // Launcher instead).
  if (shown && !envIsHomebrew())
    atexit(BackToPicker);

  return true;
}
