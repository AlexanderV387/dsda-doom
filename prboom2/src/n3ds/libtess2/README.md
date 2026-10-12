libtess2 (https://github.com/memononen/libtess2, commit 8dbd648; Include and
Source as they are there), SGI Free Software License B: see LICENSE.txt. The
polygon tessellator of the SGI OpenGL sample implementation, cleaned up by
Mikko Mononen.

[3DS] DSDA-Doom's OpenGL renderer cuts sectors into triangles with GLU's
tessellator; the 3DS has no GLU, so n3ds/pgl_glu.c offers those GLU calls on
top of this library.
