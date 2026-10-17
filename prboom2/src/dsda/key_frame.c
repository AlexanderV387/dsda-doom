//
// Copyright(C) 2020 by Ryan Krafnick
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	DSDA Key Frame
//

#include <time.h>

#include "doomstat.h"
#include "s_advsound.h"
#include "s_sound.h"
#include "st_stuff.h"
#include "p_saveg.h"
#include "p_map.h"
#include "r_draw.h"
#include "r_fps.h"
#include "r_main.h"
#include "g_game.h"
#include "m_file.h"
#include "i_system.h"
#include "lprintf.h"
#include "e6y.h"

#include "heretic/sb_bar.h"

#include "dsda.h"
#include "dsda/args.h"
#include "dsda/build.h"
#include "dsda/configuration.h"
#include "dsda/demo.h"
#include "dsda/features.h"
#include "dsda/mapinfo.h"
#include "dsda/options.h"
#include "dsda/pause.h"
#include "dsda/playback.h"
#include "dsda/save.h"
#include "dsda/settings.h"
#include "dsda/time.h"

#include "key_frame.h"

static dboolean auto_kf_timed_out;
static int auto_kf_timeout_count;

#define TIMEOUT_LIMIT 1

static dsda_key_frame_t first_kf;
static dsda_key_frame_t temp_kf;
dsda_key_frame_t quick_kf;
auto_kf_t* auto_key_frames;
int auto_kf_size = 0;
static auto_kf_t* last_auto_kf;
dsda_key_frame_t* playback_key_frames;
int playback_kf_size = 0;
static int restore_key_frame_index = -1;

static int dsda_auto_key_frame_interval;
static int dsda_auto_key_frame_depth;
static int dsda_auto_key_frame_timeout;

static int autoKeyFrameTimeout(void) {
  return dsda_StartInBuildMode() ? 0 : dsda_auto_key_frame_timeout;
}

// On the 3DS, at most 10 seconds to rewind: a key frame is a whole save
// (in Hexen with the hub's other maps, half a MB), one a second; 60 of
// them took a third of the memory and cut the rest into pieces.
#ifdef __3DS__
#define MAX_AUTO_KEY_FRAMES 10
// And at most 6 MB of them: a map as Vestiges of Grandeur's MAP07 makes
// them big (2 MB, with the hub's other maps).
#define AUTO_KEY_FRAMES_MEMORY (6 << 20)

// The save buffer, made by RoomForAutoKeyFrame for the key frame stored next.
static dboolean save_buffer_made;
#endif

static int autoKeyFrameDepth(void) {
  if (dsda_StartInBuildMode() && dsda_auto_key_frame_depth < 60)
    return 60;

#ifdef MAX_AUTO_KEY_FRAMES
  if (dsda_auto_key_frame_depth > MAX_AUTO_KEY_FRAMES)
    return MAX_AUTO_KEY_FRAMES;
#endif

  return dsda_auto_key_frame_depth;
}

static int autoKeyFrameInterval(void) {
  if (dsda_StartInBuildMode())
    return 1;

  return dsda_auto_key_frame_interval;
}

static dboolean autoKFExists(auto_kf_t* auto_kf) {
  return auto_kf && auto_kf->auto_index && auto_kf->kf.buffer;
}

#ifdef __3DS__
// Room for the save buffer of an auto key frame, made from the oldest
// ones when there is none, or none at all: the game can do without them,
// not without the room (out of memory in Vestiges of Grandeur's MAP07).
static dboolean RoomForAutoKeyFrame(dsda_key_frame_t* key_frame) {
  static dboolean said;
  auto_kf_t* kf;
  int i;

  // The one it takes the place of goes anyway.
  Z_Free(key_frame->buffer);
  key_frame->buffer = NULL;

  while (!P_TryInitSaveBuffer()) {
    // The oldest left, after the newest.
    kf = last_auto_kf->next;
    for (i = 0; i < auto_kf_size && !kf->kf.buffer; ++i)
      kf = kf->next;

    if (i == auto_kf_size) {
      if (!said)
        lprintf(LO_WARN, "RoomForAutoKeyFrame: no room for a key frame, none made\n");
      said = true;
      return false;
    }

    Z_Free(kf->kf.buffer);
    kf->kf.buffer = NULL;
  }

  save_buffer_made = true;
  return true;
}

// A key frame being read or copied: its buffer stays when memory runs out.
static dboolean kf_in_use;

// Out of memory (z_zone.c): the auto key frames go, before the GPU's
// memory is used or the game stops; rewinding is what can be done without.
static void PurgeAutoKeyFrames(void) {
  auto_kf_t* kf = last_auto_kf;
  int i;

  if (kf_in_use)
    return;

  for (i = 0; kf && i < auto_kf_size; ++i, kf = kf->next)
    if (kf->kf.buffer) {
      Z_Free(kf->kf.buffer);
      kf->kf.buffer = NULL;
    }

  if (first_kf.buffer) {
    Z_Free(first_kf.buffer);
    first_kf.buffer = NULL;
  }
}
#endif

void dsda_ForgetAutoKeyFrames(void) {
  if (last_auto_kf)
    last_auto_kf->auto_index = 0;
}

static void dsda_ResetParentKF(dsda_key_frame_t* kf) {
  kf->parent.auto_kf = NULL;
  kf->parent.buffer = NULL;
}

static void dsda_AttachAutoKF(dsda_key_frame_t* kf) {
  if (autoKFExists(last_auto_kf)) {
    kf->parent.auto_kf = last_auto_kf;
    kf->parent.buffer = last_auto_kf->kf.buffer;
  }
  else
    dsda_ResetParentKF(kf);
}

static void dsda_ResolveParentKF(dsda_key_frame_t* kf) {
  if (autoKFExists(kf->parent.auto_kf) && kf->parent.auto_kf->kf.buffer == kf->parent.buffer)
    last_auto_kf = kf->parent.auto_kf;
  else {
    dsda_ResetParentKF(kf);
    dsda_ForgetAutoKeyFrames();
  }
}

static void dsda_RewindKF(auto_kf_t** current) {
  auto_kf_t* auto_kf;

  auto_kf = *current;

  if (auto_kf &&
      auto_kf->auto_index && auto_kf->prev->auto_index &&
      auto_kf->auto_index == auto_kf->prev->auto_index + 1)
    *current = auto_kf->prev;
  else
    *current = NULL;
}

static dsda_key_frame_t* dsda_ClosestKeyFrame(int target_tic_count) {
  dsda_key_frame_t* closest = NULL;

  if (auto_key_frames)
    for (int i = 0; i < auto_kf_size; i++)
      if (auto_key_frames[i].kf.game_tic_count <= target_tic_count)
        if (!closest || auto_key_frames[i].kf.game_tic_count > closest->game_tic_count)
          closest = &auto_key_frames[i].kf;

  if (playback_key_frames)
    for (int i = 0; i < playback_kf_size; i++)
      if (playback_key_frames[i].game_tic_count <= target_tic_count)
        if (!closest || playback_key_frames[i].game_tic_count > closest->game_tic_count)
          closest = &playback_key_frames[i];

  if (!demorecording && temp_kf.buffer)
    if (temp_kf.game_tic_count <= target_tic_count)
      if (!closest || temp_kf.game_tic_count > closest->game_tic_count)
        closest = &temp_kf;

  if (!demorecording && quick_kf.buffer)
    if (quick_kf.game_tic_count <= target_tic_count)
      if (!closest || quick_kf.game_tic_count > closest->game_tic_count)
        closest = &quick_kf;

  if (first_kf.buffer)
    if (first_kf.game_tic_count <= target_tic_count)
      if (!closest || first_kf.game_tic_count > closest->game_tic_count)
        closest = &first_kf;

  return closest;
}

void dsda_CopyKeyFrame(dsda_key_frame_t* dest, dsda_key_frame_t* source) {
  *dest = *source;
  dest->buffer = Z_Malloc(dest->buffer_length);
  memcpy(dest->buffer, source->buffer, dest->buffer_length);
}

void dsda_InitAutoKeyFrames(void) {
  int i;

  dsda_auto_key_frame_interval = dsda_IntConfig(dsda_config_auto_key_frame_interval);
  dsda_auto_key_frame_depth = dsda_IntConfig(dsda_config_auto_key_frame_depth);
  dsda_auto_key_frame_timeout = dsda_IntConfig(dsda_config_auto_key_frame_timeout);

  auto_kf_size = autoKeyFrameDepth();

#ifdef __3DS__
  Z_SetLowMemoryHandler(PurgeAutoKeyFrames);
#endif

  if (!auto_kf_size) {
    last_auto_kf = NULL;
    return;
  }

  ++auto_kf_size; // chain includes a terminator

  if (auto_key_frames != NULL)
    Z_Free(auto_key_frames);

  auto_key_frames = Z_Calloc(auto_kf_size, sizeof(auto_kf_t));

  auto_key_frames[0].prev = &auto_key_frames[auto_kf_size - 1];
  auto_key_frames[auto_kf_size - 1].next = &auto_key_frames[0];

  for (i = 0; i < auto_kf_size - 1; ++i)
    auto_key_frames[i].next = &auto_key_frames[i + 1];

  for (i = 1; i < auto_kf_size; ++i)
    auto_key_frames[i].prev = &auto_key_frames[i - 1];

  last_auto_kf = &auto_key_frames[auto_kf_size - 1];
}

void dsda_InitPlaybackKeyFrames() {
  // Max of 60 keyframes are saved, and they need to have 1 minute in between each
  playback_kf_size = demo_tics_count * demo_playerscount / TICRATE / 60;
  if (playback_kf_size > 60)
    playback_kf_size = 60;

  if (playback_key_frames != NULL)
    Z_Free(playback_key_frames);

  playback_key_frames = Z_Calloc(playback_kf_size, sizeof(dsda_key_frame_t));
}

void dsda_ExportKeyFrame(byte* buffer, int length) {
  char name[40];
  int timestamp;

  timestamp = totalleveltimes + leveltime;

  snprintf(name, sizeof(name), "backup-%010d.kf", timestamp);

  if (M_FileExists(name))
    snprintf(name, sizeof(name), "backup-%010d-%lld.kf", timestamp, (long long) time(NULL));

  if (!M_WriteFile(name, buffer, length))
    I_Error("dsda_ExportKeyFrame: Failed to write key frame.");
}

// Stripped down version of G_DoSaveGame
void dsda_StoreKeyFrame(dsda_key_frame_t* key_frame, byte complete, byte export) {
#ifdef __3DS__
  jmp_buf abort_save;
#endif

  key_frame->game_tic_count = true_logictic;

#ifdef __3DS__
  if (save_buffer_made) {
    save_buffer_made = false;

    // An auto key frame: if the save buffer cannot grow (Vestiges of
    // Grandeur's MAP07, 3.7 MB in one piece), none; the thinkers' links,
    // used for their numbers while saving, made right again.
    if (setjmp(abort_save)) {
      P_SetSaveAbort(NULL);
      P_IndexToThinker();
      P_FreeSaveBuffer();
      key_frame->buffer = NULL;
      PurgeAutoKeyFrames();
      return;
    }
    P_SetSaveAbort(&abort_save);
  }
  else
#endif
  P_InitSaveBuffer();

  P_SAVE_BYTE(complete);
  P_SAVE_X(key_frame->game_tic_count);

  // Store state of demo playback buffer
  dsda_StorePlaybackPosition();

  // Store state of demo recording buffer
  dsda_StoreDemoData(complete);

  dsda_ArchiveAll();
#ifdef __3DS__
  P_SetSaveAbort(NULL);
#endif

  if (key_frame->buffer != NULL) Z_Free(key_frame->buffer);

  key_frame->buffer = savebuffer;
  key_frame->buffer_length = save_p - savebuffer;
#ifdef __3DS__
  // Only what it holds (the save buffer was made as big as the biggest).
  key_frame->buffer = Z_Realloc(key_frame->buffer, key_frame->buffer_length);
#endif

  P_ForgetSaveBuffer();

  dsda_AttachAutoKF(key_frame);

  if (complete) {
    if (demorecording && export)
      dsda_ExportKeyFrame(key_frame->buffer, key_frame->buffer_length);

    doom_printf("Stored key frame");
  }
}

// Stripped down version of G_DoLoadGame
void dsda_RestoreKeyFrame(dsda_key_frame_t* key_frame, dboolean skip_wipe) {
  void G_AfterLoad(void);

  byte complete;

  if (key_frame->buffer == NULL) {
    doom_printf("No key frame found");
    return;
  }

  dsda_TrackFeature(uf_keyframe);

  if (skip_wipe || dsda_BuildMode())
    dsda_SkipNextWipe();

  save_p = key_frame->buffer;
#ifdef __3DS__
  kf_in_use = true;
#endif

  P_LOAD_BYTE(complete);
  P_LOAD_X(key_frame->game_tic_count);

  // Restore state of demo playback buffer
  dsda_RestorePlaybackPosition();

  // Restore state of demo recording buffer
  dsda_RestoreDemoData(complete);

  dsda_UnArchiveAll();
#ifdef __3DS__
  kf_in_use = false;
#endif

  dsda_RestoreCommandHistory();

  restore_key_frame_index = (totalleveltimes + leveltime) / (35 * autoKeyFrameInterval());

  G_AfterLoad();

  dsda_QueueJoin();

  dsda_ResolveParentKF(key_frame);

  doom_printf("Restored key frame");
}

void dsda_StoreTempKeyFrame(void) {
  dsda_StoreKeyFrame(&temp_kf, true, true);
}

void dsda_StoreQuickKeyFrame(void) {
  dsda_StoreKeyFrame(&quick_kf, true, true);
}

void dsda_RestoreQuickKeyFrame(void) {
  dsda_RestoreKeyFrame(&quick_kf, false);
}

dboolean dsda_RestoreClosestKeyFrame(int tic) {
  dsda_key_frame_t* key_frame;

  key_frame = dsda_ClosestKeyFrame(tic);

  if (!key_frame)
    return false;

  dsda_RestoreKeyFrame(key_frame, true);

  return true;
}

void dsda_RestoreKeyFrameFile(const char* name) {
  char *filename;
  dsda_key_frame_t key_frame = { 0 };

  filename = I_RequireFile(name, ".kf");
  M_ReadFile(filename, &key_frame.buffer);
  Z_Free(filename);

  dsda_RestoreKeyFrame(&key_frame, false);
  Z_Free(key_frame.buffer);
}

void dsda_ContinueKeyFrame(void) {
  dsda_arg_t* arg;

  arg = dsda_Arg(dsda_arg_from_key_frame);
  if (arg->found) {
    dsda_RestoreKeyFrameFile(arg->value.v_string);
  }
}

void dsda_RewindAutoKeyFrame(void) {
  auto_kf_t* load_kf;

  load_kf = last_auto_kf;
  dsda_RewindKF(&load_kf);

  if (load_kf)
    dsda_RestoreKeyFrame(&load_kf->kf, true);
  else
    doom_printf("No key frame found"); // rewind past the depth limit
}

void dsda_ResetAutoKeyFrameTimeout(void) {
  auto_kf_timed_out = false;
  auto_kf_timeout_count = 0;
}

void dsda_UpdateAutoKeyFrames(void) {
  int key_frame_index;
  int current_time;
  int interval_tics;
  dsda_key_frame_t* current_key_frame;

  if (
    auto_kf_timed_out ||
    auto_kf_size == 0 ||
    gamestate != GS_LEVEL ||
    gameaction != ga_nothing
  ) return;

  current_time = totalleveltimes + leveltime;
  interval_tics = 35 * autoKeyFrameInterval();

  // Automatically save a key frame each interval
  if (current_time % interval_tics == 0) {
    key_frame_index = current_time / interval_tics;

    // Don't duplicate on rewind
    if (key_frame_index == restore_key_frame_index) {
      restore_key_frame_index = -1;
      return;
    }

    last_auto_kf = last_auto_kf->next;
    last_auto_kf->next->auto_index = 0;
    last_auto_kf->auto_index = last_auto_kf->prev->auto_index + 1;

    current_key_frame = &last_auto_kf->kf;

#ifdef __3DS__
    if (!RoomForAutoKeyFrame(current_key_frame))
      return;
#endif

    {
      unsigned long long elapsed_time;

      dsda_StartTimer(dsda_timer_key_frame);
      dsda_StoreKeyFrame(current_key_frame, false, false);
      elapsed_time = dsda_ElapsedTimeMS(dsda_timer_key_frame);

      if (autoKeyFrameTimeout()) {
        if (elapsed_time > autoKeyFrameTimeout()) {
          ++auto_kf_timeout_count;

          if (auto_kf_timeout_count > TIMEOUT_LIMIT) {
            auto_kf_timed_out = true;
            doom_printf("Slow key framing: rewind disabled");
          }
        }
        else
          auto_kf_timeout_count = 0;
      }
    }

    if (!first_kf.buffer && current_key_frame->buffer) {
#ifdef __3DS__
      kf_in_use = true;
#endif
      dsda_CopyKeyFrame(&first_kf, current_key_frame);
#ifdef __3DS__
      kf_in_use = false;
#endif
    }

#ifdef AUTO_KEY_FRAMES_MEMORY
    // The oldest go when they take too much (a big map's are big).
    {
      auto_kf_t *kf = last_auto_kf;
      int total = 0;
      int i;

      for (i = 0; i < auto_kf_size; ++i, kf = kf->prev)
      {
        if (kf->kf.buffer == NULL)
          continue;

        total += kf->kf.buffer_length;
        if (total > AUTO_KEY_FRAMES_MEMORY && kf != last_auto_kf)
        {
          Z_Free(kf->kf.buffer);
          kf->kf.buffer = NULL;
        }
      }
    }
#endif
  }
}

void dsda_UpdatePlaybackKeyFrames(void) {
  int current_time;
  int interval_tics;

  if (gameaction != ga_nothing || playback_kf_size == 0) return;

  current_time = totalleveltimes + leveltime;
  interval_tics = (demo_tics_count * demo_playerscount) / playback_kf_size;

  // Automatically save a key frame each interval
  if (current_time % interval_tics == 0 &&
      current_time / interval_tics < playback_kf_size) {

    dsda_key_frame_t* current_key_frame = &playback_key_frames[current_time / interval_tics];

    if (!current_key_frame->buffer)
      dsda_StoreKeyFrame(current_key_frame, false, false);
  }
}
