/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Zone Memory Allocation. Neat.
 *
 * Neat enough to be rewritten by Lee Killough...
 *
 * Must not have been real neat :)
 *
 * Made faster and more general, and added wrappers for all of Doom's
 * memory allocation functions, including malloc() and similar functions.
 * Added line and file numbers, in case of error. Added performance
 * statistics and tunables.
 *-----------------------------------------------------------------------------
 */


// use config.h if autoconf made one -- josh
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>

#include "z_zone.h"
#include "doomstat.h"
#include "v_video.h"
#include "g_game.h"
#include "lprintf.h"

#ifdef __3DS__
#include <malloc.h>
#endif

#ifdef DJGPP
#include <dpmi.h>
#endif

#ifdef __3DS__
#include "n3ds/n3ds.h"
#endif

#define ZONE_SIGNATURE 0x931d4a11

enum {
  ZONE_STATIC,
  ZONE_LEVEL,
  ZONE_MAX
};

typedef struct memblock {
  unsigned signature;
  struct memblock *next,*prev;
  size_t size;
  unsigned char tag;
#ifdef __3DS__
  unsigned char linear; // from the GPU's memory (LinearBlock)
  void *caller;         // who asked (Z_LogUsage)
#endif
} memblock_t;

static const size_t HEADER_SIZE = sizeof(memblock_t);

static memblock_t *blockbytag[ZONE_MAX];

/* Z_Malloc
 * cph - the algorithm here was a very simple first-fit round-robin
 *  one - just keep looping around, freeing everything we can until
 *  we get a large enough space
 *
 * This has been changed now; we still do the round-robin first-fit,
 * but we only free the blocks we actually end up using; we don't
 * free all the stuff we just pass on the way.
 */

#ifdef __3DS__
// Out of memory on the 3DS: how the heap was, in the log (all of it used,
// or free but in pieces too small), and who asked (for addr2line).
static void LogHeap(size_t size, void *caller)
{
  const struct mallinfo info = mallinfo();
  size_t low = 0, high = size;

  // The largest block there is, to the KiB.
  while (high - low > 1024)
  {
    const size_t mid = low + (high - low) / 2;
    void *p = malloc(mid);

    if (p)
    {
      free(p);
      low = mid;
    }
    else
      high = mid;
  }

  lprintf(LO_ERROR, "Z_Malloc: %lu bytes for %p: heap %u KiB taken from the system, %u KiB used, "
          "%u KiB free in %u pieces (the largest about %u KiB); %u KiB free for the GPU\n",
          (unsigned long) size, caller, (unsigned) (info.arena / 1024),
          (unsigned) (info.uordblks / 1024), (unsigned) (info.fordblks / 1024),
          (unsigned) info.ordblks, (unsigned) (low / 1024), N3DS_LinearFree() / 1024);
}

#define CALLER __builtin_return_address(0)
static void *alloc_caller;
#define SET_CALLER() (alloc_caller = CALLER)

// What the heap cannot give, from the GPU's memory (normal memory, as fast
// for the CPU) while that keeps room for the textures: after a few maps of
// a hub (Vestiges of Grandeur's MAP07) the heap had 16 MB free in pieces
// and none of 1.5 MB, the GPU's 29 MB.
#define LINEAR_RESERVE (8u << 20)

static memblock_t *LinearBlock(size_t size)
{
  memblock_t *block;

  if (N3DS_LinearFree() < size + LINEAR_RESERVE)
    return NULL;

  block = N3DS_LinearAlloc(size);
  if (block)
    block->linear = true;
  return block;
}

static void (*low_memory_handler)(void);

void Z_SetLowMemoryHandler(void (*handler)(void))
{
  low_memory_handler = handler;
}

// When the heap has no room: what can go (the auto key frames) goes first,
// then the GPU's memory.
static memblock_t *LowMemoryBlock(size_t size)
{
  memblock_t *block;

  if (low_memory_handler)
  {
    low_memory_handler();
    if ((block = malloc(size)))
    {
      block->linear = false;
      return block;
    }
  }

  return LinearBlock(size);
}

// Who has the zone's memory, the most first, in the log: by who asked
// (for addr2line), with how many blocks.
#define USAGE_CALLERS 128
#define USAGE_SHOWN 8

void Z_LogUsage(const char *why)
{
  static struct { void *caller; size_t bytes; unsigned count; } usage[USAGE_CALLERS];
  static char line[512];
  size_t total = 0, linear = 0;
  unsigned blocks = 0;
  int used = 0, tag, i, j, len;

  for (tag = 0; tag < ZONE_MAX; ++tag)
  {
    memblock_t *block = blockbytag[tag];

    if (!block)
      continue;

    do
    {
      for (i = 0; i < used && usage[i].caller != block->caller; ++i)
        ;
      if (i == used && used < USAGE_CALLERS)
      {
        usage[i].caller = block->caller;
        usage[i].bytes = 0;
        usage[i].count = 0;
        ++used;
      }
      if (i < used)
      {
        usage[i].bytes += block->size;
        ++usage[i].count;
      }
      total += block->size;
      if (block->linear)
        linear += block->size;
      ++blocks;
      block = block->next;
    } while (block != blockbytag[tag]);
  }

  // One line (the log times each write).
  len = snprintf(line, sizeof(line), "zone (%s): %u KiB in %u blocks (%u KiB in the GPU's memory); the most:",
                 why, (unsigned) (total / 1024), blocks, (unsigned) (linear / 1024));
  for (j = 0; j < USAGE_SHOWN && j < used && len < (int) sizeof(line); ++j)
  {
    int most = j;

    for (i = j + 1; i < used; ++i)
      if (usage[i].bytes > usage[most].bytes)
        most = i;

    if (most != j)
    {
      const size_t bytes = usage[j].bytes;
      const unsigned count = usage[j].count;
      void *caller = usage[j].caller;

      usage[j] = usage[most];
      usage[most].bytes = bytes;
      usage[most].count = count;
      usage[most].caller = caller;
    }

    len += snprintf(line + len, sizeof(line) - len, " %p %u KiB/%u", usage[j].caller,
                    (unsigned) (usage[j].bytes / 1024), usage[j].count);
  }
  lprintf(LO_INFO, "%s\n", line);
}
#else
#define SET_CALLER() ((void) 0)
#endif

static void *Z_MallocTagIf(size_t size, int tag, dboolean must)
{
  memblock_t *block = NULL;

  if (!size)
    return NULL; // malloc(0) returns NULL

  if ((block = malloc(size + HEADER_SIZE)))
  {
#ifdef __3DS__
    block->linear = false;
#endif
  }
  else
  {
    if (!must)
      return NULL;
#ifdef __3DS__
    if (!(block = LowMemoryBlock(size + HEADER_SIZE)))
    {
      LogHeap(size + HEADER_SIZE, alloc_caller);
      Z_LogUsage("out of memory");
#endif
    I_Error ("Z_Malloc: Failure trying to allocate %lu bytes", (unsigned long) size);
#ifdef __3DS__
    }
#endif
  }

  if (!blockbytag[tag])
  {
    blockbytag[tag] = block;
    block->next = block->prev = block;
  }
  else
  {
    blockbytag[tag]->prev->next = block;
    block->prev = blockbytag[tag]->prev;
    block->next = blockbytag[tag];
    blockbytag[tag]->prev = block;
  }

  block->size = size;
  block->signature = ZONE_SIGNATURE;
#ifdef __3DS__
  block->caller = alloc_caller;
#endif
  block->tag = tag;           // tag
  block = (memblock_t *)((char *) block + HEADER_SIZE);

  return block;
}

static void *Z_MallocTag(size_t size, int tag)
{
  return Z_MallocTagIf(size, tag, true);
}

void Z_Free(void *p)
{
  memblock_t *block = (memblock_t *)((char *) p - HEADER_SIZE);

  if (!p)
    return;

  if (block->signature != ZONE_SIGNATURE)
    I_Error("Z_Free: freed a non-zone pointer");
  block->signature = 0;       // Nullify signature so another free fails

  if (block == block->next)
    blockbytag[block->tag] = NULL;
  else
    if (blockbytag[block->tag] == block)
      blockbytag[block->tag] = block->next;
  block->prev->next = block->next;
  block->next->prev = block->prev;

#ifdef __3DS__
  if (block->linear)
  {
    N3DS_LinearRelease(block);
    return;
  }
#endif
  free(block);
}

static void Z_FreeTag(int tag)
{
  memblock_t *block, *end_block;

  if (tag < 0 || tag >= ZONE_MAX)
    I_Error("Z_FreeTag: Tag %i does not exist", tag);

  block = blockbytag[tag];
  if (!block)
    return;
  end_block = block->prev;
  while (1)
  {
    memblock_t *next = block->next;
    Z_Free((char *) block + HEADER_SIZE);
    if (block == end_block)
      break;
    block = next;               // Advance to next block
  }
}

static void *Z_ReallocTag(void *ptr, size_t n, int tag)
{
  memblock_t *block = ptr ? (memblock_t *)((char *) ptr - HEADER_SIZE) : NULL;
  memblock_t *moved;
  void *p;

  // A block kept in its tag grows in place when it can, by realloc: a
  // new one each time, the old freed, left holes the next could not use
  // (a table grown little by little, on the 3DS: its heap in pieces).
  // (On the 3DS, when the heap has no room, or the block is from the GPU's
  // memory, a new one as below: from either.)
  if (block && n && block->signature == ZONE_SIGNATURE && block->tag == tag
#ifdef __3DS__
      && !block->linear && (moved = realloc(block, n + HEADER_SIZE))
#endif
     )
  {
#ifndef __3DS__
    moved = realloc(block, n + HEADER_SIZE);
    if (!moved)
      I_Error("Z_Realloc: Failure trying to allocate %lu bytes", (unsigned long) n);
#endif

    if (moved != block)
    {
      // The list's links to it, to its new place.
      if (moved->next == block)
        moved->next = moved->prev = moved;
      else
      {
        moved->prev->next = moved;
        moved->next->prev = moved;
      }
      if (blockbytag[tag] == block)
        blockbytag[tag] = moved;
    }
    moved->size = n;
    return (char *) moved + HEADER_SIZE;
  }

  p = Z_MallocTag(n, tag);
  if (ptr)
    {
      memcpy(p, ptr, n <= block->size ? n : block->size);
      Z_Free(ptr);
    }
  return p;
}

static void *Z_CallocTag(size_t n1, size_t n2, int tag)
{
  return
    (n1*=n2) ? memset(Z_MallocTag(n1, tag), 0, n1) : NULL;
}

static char *Z_StrdupTag(const char *s, int tag)
{
  return strcpy(Z_MallocTag(strlen(s)+1, tag), s);
}

void *Z_Malloc(size_t size)
{
  SET_CALLER();
  return Z_MallocTag(size, ZONE_STATIC);
}

// NULL when there is no room, for what the game can do without.
void *Z_TryMalloc(size_t size)
{
  return Z_MallocTagIf(size, ZONE_STATIC, false);
}

void *Z_Calloc(size_t n, size_t n2)
{
  SET_CALLER();
  return Z_CallocTag(n, n2, ZONE_STATIC);
}

void *Z_Realloc(void *p, size_t n)
{
  SET_CALLER();
  return Z_ReallocTag(p, n, ZONE_STATIC);
}

char *Z_Strdup(const char *s)
{
  SET_CALLER();
  return Z_StrdupTag(s, ZONE_STATIC);
}

void Z_FreeLevel(void)
{
  return Z_FreeTag(ZONE_LEVEL);
}

void *Z_MallocLevel(size_t size)
{
  SET_CALLER();
  return Z_MallocTag(size, ZONE_LEVEL);
}

void *Z_CallocLevel(size_t n, size_t n2)
{
  SET_CALLER();
  return Z_CallocTag(n, n2, ZONE_LEVEL);
}

void *Z_ReallocLevel(void *p, size_t n)
{
  SET_CALLER();
  return Z_ReallocTag(p, n, ZONE_LEVEL);
}

char *Z_StrdupLevel(const char *s)
{
  SET_CALLER();
  return Z_StrdupTag(s, ZONE_LEVEL);
}
