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
 *
 *---------------------------------------------------------------------
 */

/*
** gl_vertex.cpp
**
**---------------------------------------------------------------------------
** Copyright 2006 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
** 4. When not used as part of GZDoom or a GZDoom derivative, this code will be
**    covered by the terms of the GNU Lesser General Public License as published
**    by the Free Software Foundation; either version 2.1 of the License, or (at
**    your option) any later version.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

// Adapted for prboom(-plus) by entryway

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gl_opengl.h"

#include "z_zone.h"
#include "v_video.h"
#include "gl_intern.h"
#include "r_main.h"

typedef struct vertexsplit_info_s
{
  dboolean changed;
  int numheights;
  int numsectors;
  sector_t **sectors;
  float *heightlist;
  byte validcount;
} vertexsplit_info_t;

static vertexsplit_info_t * gl_vertexsplit = NULL;

typedef struct splitsbysector_s
{
  int numsplits;
  vertexsplit_info_t **splits;
} splitsbysector_t;
static splitsbysector_t * gl_splitsbysector = NULL;
static vertexsplit_info_t ** gl_splitsbysector_data = NULL;

//==========================================================================
//
// Split left edge of wall
//
//==========================================================================
void gld_SplitLeftEdge(const GLWall *wall)
{
  vertex_t *v;
  vertexsplit_info_t *vi;

  v = wall->seg->linedef->v1;

  if (v == NULL)
    return;

  vi = &gl_vertexsplit[v - vertexes];

  if (vi->numheights)
  {
    int i = 0;

    float polyh1 = wall->ytop - wall->ybottom;
    float factv1 = (polyh1 ? (wall->vt - wall->vb) / polyh1 : 0);
    float factu1 = (polyh1 ? (wall->ul - wall->ul) / polyh1 : 0);

    while (i < vi->numheights && vi->heightlist[i] <= wall->ybottom)
      i++;

    while (i < vi->numheights && vi->heightlist[i] < wall->ytop)
    {
      GLfloat s = factu1 * (vi->heightlist[i] - wall->ytop) + wall->ul;
      GLfloat t = factv1 * (vi->heightlist[i] - wall->ytop) + wall->vt;

      glTexCoord2f(s, t);

      glVertex3f(wall->glseg->x1, vi->heightlist[i], wall->glseg->z1);
      i++;
    }
  }
}

//==========================================================================
//
// Split right edge of wall
//
//==========================================================================
void gld_SplitRightEdge(const GLWall *wall)
{
  vertex_t *v;
  vertexsplit_info_t * vi;

  v = wall->seg->linedef->v2;

  if (v == NULL)
    return;

  vi = &gl_vertexsplit[v - vertexes];

  if (vi->numheights)
  {
    int i = vi->numheights - 1;

    float polyh2 = wall->ytop - wall->ybottom;
    float factv2 = (polyh2 ? (wall->vt - wall->vb) / polyh2 : 0);
    float factu2 = (polyh2 ? (wall->ur - wall->ur) / polyh2 : 0);

    while (i > 0 && vi->heightlist[i] >= wall->ytop)
      i--;

    while (i > 0 && vi->heightlist[i] > wall->ybottom)
    {
      GLfloat s = factu2 * (vi->heightlist[i] - wall->ytop) + wall->ur;
      GLfloat t = factv2 * (vi->heightlist[i] - wall->ytop) + wall->vt;

      glTexCoord2f(s, t);

      glVertex3f(wall->glseg->x2, vi->heightlist[i], wall->glseg->z2);
      i--;
    }
  }
}

//==========================================================================
//
// Recalculate all heights affectting this vertex.
//
//==========================================================================
void gld_RecalcVertexHeights(const vertex_t *v)
{
  int i,j,k;
  float height;
  vertexsplit_info_t *vi;

  vi = &gl_vertexsplit[v - vertexes];

  if (vi->validcount == rendermarker)
    return;

  vi->validcount = rendermarker;

  if (!vi->changed)
    return;

  vi->changed = false;

  vi->numheights = 0;
  for(i = 0; i < vi->numsectors; i++)
  {
    for(j = 0; j < 2; j++)
    {
      if (j == 0)
        height = (float)vi->sectors[i]->ceilingheight/MAP_SCALE;
      else
        height = (float)vi->sectors[i]->floorheight/MAP_SCALE;

      for(k = 0; k < vi->numheights; k++)
      {
        if (height == vi->heightlist[k])
          break;

        if (height < vi->heightlist[k])
        {
          memmove(&vi->heightlist[k + 1], &vi->heightlist[k], sizeof(vi->heightlist[0]) * (vi->numheights - k));
          vi->heightlist[k] = height;
          vi->numheights++;
          break;
        }
      }
      if (k == vi->numheights)
        vi->heightlist[vi->numheights++] = height;
    }
  }

  if (vi->numheights <= 2)
    vi->numheights = 0;  // is not in need of any special attention
}

//==========================================================================
//
//
//
//==========================================================================
static void AddToVertex(const sector_t *sec, int *list, unsigned int *size)
{
  unsigned int i;
  int secno = sec->iSectorID;

  for(i = 0; i < (*size); i++)
  {
    if (list[i] == secno)
      return;
  }
  list[(*size)] = secno;
  (*size)++;
}

//==========================================================================
//
//
//
//==========================================================================
void gld_InitVertexData()
{
  int i, j, k;
  int vertexes_count, gl_vertexsplit_size, pos;
  int * vt_sectors;
  int * vt_start;
  unsigned int * vt_sectorlists_size;
  vertexsplit_info_t ** splits;

  if (gl_vertexsplit)
    return;

  // The sectors at each vertex in one block, from vt_start[vertex]: room
  // for the two sides of each line at it, with their height sectors. Not
  // a list grown one by one for each vertex, never freed: a big map's
  // (Vestiges of Grandeur's MAP07: 34,000) cut the heap into pieces.
  vt_start = Z_Calloc(sizeof(vt_start[0]), numvertexes + 1);
  vt_sectorlists_size = Z_Calloc(sizeof(vt_sectorlists_size[0]), numvertexes);

  for(i = 0; i < numlines; i++)
  {
    vt_start[lines[i].v1 - vertexes + 1] += 4;
    vt_start[lines[i].v2 - vertexes + 1] += 4;
  }
  for(i = 0; i < numvertexes; i++)
    vt_start[i + 1] += vt_start[i];

  vt_sectors = Z_Malloc(sizeof(vt_sectors[0]) * MAX(vt_start[numvertexes], 1));

  for(i = 0; i < numlines; i++)
  {
    line_t *line = &lines[i];

    for(j = 0; j < 2; j++)
    {
      vertex_t *v = (j==0 ? line->v1 : line->v2);

      for(k = 0; k < 2; k++)
      {
        sector_t *sec = (k == 0 ? line->frontsector : line->backsector);

        if (sec)
        {
          AddToVertex(sec, &vt_sectors[vt_start[v-vertexes]], &vt_sectorlists_size[v-vertexes]);
          if (sec->heightsec >= 0)
          {
            AddToVertex(&sectors[sec->heightsec], &vt_sectors[vt_start[v-vertexes]], &vt_sectorlists_size[v-vertexes]);
          }
        }
      }
    }
  }

  vertexes_count = 0;
  for(i = 0; i < numvertexes; i++)
  {
    if (vt_sectorlists_size[i] > 1)
    {
      vertexes_count += vt_sectorlists_size[i];
    }
  }

  gl_vertexsplit_size =
    numvertexes * sizeof(vertexsplit_info_t) +
    vertexes_count * sizeof(gl_vertexsplit->sectors[0]) +
    2 * vertexes_count * sizeof(gl_vertexsplit->heightlist[0]);

  gl_vertexsplit = Z_Malloc(gl_vertexsplit_size);
  memset(gl_vertexsplit, 0, gl_vertexsplit_size);

  pos = numvertexes * sizeof(vertexsplit_info_t);
  for(i = 0; i < numvertexes; i++)
  {
    int cnt = vt_sectorlists_size[i];
    vertexsplit_info_t *vi = &gl_vertexsplit[i];

    vi->validcount = -1;
    vi->numheights = 0;
    if (cnt > 1)
    {
      vi->changed = true;
      vi->numsectors = cnt;

      vi->sectors = (sector_t **)((unsigned char*)gl_vertexsplit + pos);
      pos += sizeof(vi->sectors[0]) * cnt;
      vi->heightlist = (float *)((unsigned char*)gl_vertexsplit + pos);
      pos += sizeof(vi->heightlist[0]) * cnt * 2;

      for(j = 0; j < cnt; j++)
      {
        vi->sectors[j] = &sectors[vt_sectors[vt_start[i] + j]];
      }
    }
    else
    {
      vi->numsectors=0;
    }
  }

  // Each sector's splits, in one block: counted, then filled, from each
  // vertex's sectors (each one there once). It was each sector against
  // each vertex: in MAP07, 240 million turns.
  gl_splitsbysector = Z_Malloc(sizeof(gl_splitsbysector[0]) * numsectors);
  memset(gl_splitsbysector, 0, sizeof(gl_splitsbysector[0]) * numsectors);

  pos = 0;
  for(j = 0; j < numvertexes; j++)
  {
    for(k = 0; k < gl_vertexsplit[j].numsectors; k++)
    {
      gl_splitsbysector[gl_vertexsplit[j].sectors[k] - sectors].numsplits++;
      pos++;
    }
  }

  gl_splitsbysector_data = splits = Z_Malloc(sizeof(splits[0]) * MAX(pos, 1));
  for(i = 0; i < numsectors; i++)
  {
    gl_splitsbysector[i].splits = splits;
    splits += gl_splitsbysector[i].numsplits;
    gl_splitsbysector[i].numsplits = 0;
  }

  for(j = 0; j < numvertexes; j++)
  {
    vertexsplit_info_t *vi = &gl_vertexsplit[j];

    for(k = 0; k < vi->numsectors; k++)
    {
      splitsbysector_t *splitsbysector = &gl_splitsbysector[vi->sectors[k] - sectors];

      splitsbysector->splits[splitsbysector->numsplits++] = vi;
    }
  }

  for(i = 0; i < numvertexes; i++)
    gld_RecalcVertexHeights(&vertexes[i]);

  Z_Free(vt_sectors);
  Z_Free(vt_start);
  Z_Free(vt_sectorlists_size);
}

//==========================================================================
//
//
//
//==========================================================================
void gld_UpdateSplitData(sector_t *sector)
{
  int i;

  if (gl_splitsbysector)
  {
    splitsbysector_t *splitsbysector = &gl_splitsbysector[sector->iSectorID];
    for (i = 0; i < splitsbysector->numsplits; i++)
    {
      splitsbysector->splits[i]->changed = true;
    }
  }
}

//==========================================================================
//
//
//
//==========================================================================
void gld_CleanVertexData()
{
  if (gl_vertexsplit)
  {
    Z_Free(gl_vertexsplit);
    gl_vertexsplit = NULL;
  }

  if (gl_splitsbysector)
  {
    Z_Free(gl_splitsbysector_data);
    gl_splitsbysector_data = NULL;
    Z_Free(gl_splitsbysector);
    gl_splitsbysector = NULL;
  }
}
