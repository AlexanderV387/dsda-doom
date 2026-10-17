/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2001 by
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
 *      Handles in-memory caching of WAD lumps
 *
 *-----------------------------------------------------------------------------
 */

// use config.h if autoconf made one -- josh
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "doomstat.h"
#include "doomtype.h"

#include "w_wad.h"
#include "z_zone.h"
#include "lprintf.h"

static void **lump_data;

#ifdef __3DS__
// The lumps read stay until the end. On the 3DS the small ones go side by
// side in big blocks of their own, not one by one all over the heap
// between what comes and goes (a level's data, the textures' copies): the
// holes those leave would be cut into pieces too small for what comes
// after (Hexen, a few maps in: 30 MB free, no 512 KB in one piece).
#define LUMP_BLOCK_SIZE (1 << 20)
#define LUMP_IN_BLOCK_MAX (64 << 10)

static void *LumpAlloc(size_t size)
{
  static byte *block;
  static size_t block_left;
  void *p;

  if (size == 0 || size > LUMP_IN_BLOCK_MAX)
    return Z_Malloc(size);

  size = (size + 7) & ~(size_t) 7;

  if (size > block_left)
  {
    block = Z_Malloc(LUMP_BLOCK_SIZE);
    block_left = LUMP_BLOCK_SIZE;
  }

  p = block;
  block += size;
  block_left -= size;
  return p;
}
#else
#define LumpAlloc Z_Malloc
#endif

/* W_InitCache
 *
 * cph 2001/07/07 - split from W_Init
 */
void W_InitCache(void)
{
  // set up caching
  lump_data = calloc(sizeof *lump_data, numlumps);
  if (!lump_data)
    I_Error ("W_Init: Couldn't allocate lump data");
}

void W_DoneCache(void)
{
}

/* W_LumpByNum
 * killough 4/25/98: simplified
 * CPhipps - modified for new lump locking scheme
 *           returns a const*
 */

const void *W_LumpByNum(int lump)
{
#ifdef RANGECHECK
  if ((unsigned)lump >= (unsigned)numlumps)
    I_Error ("W_LumpByNum: %i >= numlumps",lump);
#endif

  // read the lump in
  if (!lump_data[lump]) {
    lump_data[lump] = LumpAlloc(W_LumpLength(lump));
    W_ReadLump(lump, lump_data[lump]);
  }

  return lump_data[lump];
}

#ifdef __3DS__
// A lump read only while a level is set up (its lines, sides...): its
// memory back after; read again if wanted. Lumps in a shared block stay.
void W_ReleaseLump(int lump)
{
  if (lump < 0 || lump >= numlumps || !lump_data[lump] || W_LumpLength(lump) <= LUMP_IN_BLOCK_MAX)
    return;

  Z_Free(lump_data[lump]);
  lump_data[lump] = NULL;
}
#endif

const void *W_LockLumpNum(int lump)
{
  return W_LumpByNum(lump);
}

void *W_GetModifiableLumpData(int lump)
{
  return lump_data[lump];
}
