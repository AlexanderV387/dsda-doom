//
// [3DS] The 3DS has no OpenGL: DSDA-Doom draws in software there and the
// OpenGL renderer (gl_*.c, dsda/gl/render_scale.c) is not compiled. The
// rest of the game only calls it in OpenGL mode, which is never on; these
// empty versions let it link. A few things the software code also uses
// (the animated texture tables) are the real ones.
//

#include "config.h"
#include "doomtype.h"
#include "doomstat.h"
#include "v_video.h"
#include "r_main.h"
#include "r_plane.h"
#include "r_things.h"
#include "p_spec.h"
#include "gl_struct.h"
#include "dsda/gl/render_scale.h"

#include <SDL_opengl.h>

// From gl_main.c: also filled by the software code (p_spec.c).
TAnimItemParam *anim_flats = NULL;
TAnimItemParam *anim_textures = NULL;

int gl_drawskys;
dboolean gl_use_stencil;
const int gl_colorbuffer_bits = 32;
const int gl_depthbuffer_bits = 24;

// dsda/gl/render_scale.c
int gl_statusbar_height;
int gl_scene_width;
int gl_scene_height;
float gl_scale_x = 1;
float gl_scale_y = 1;
int gl_letterbox_clear_required;

void dsda_GLSetRenderViewportParams(void) {}
void dsda_GLSetRenderViewport(void) {}
void dsda_GLUpdateStatusBarVisible(void) {}
void dsda_GLLetterboxClear(void) {}
void dsda_GLStartMeltRenderTexture(void) {}
void dsda_GLEndMeltRenderTexture(void) {}

// gl_missingtexture.c
sector_t* GetBestBleedSector(sector_t* source, enum bleedtype type) { return NULL; }
sector_t* GetBestFake(sector_t *sector, int ceiling, int validcount) { return NULL; }

// gl_main.c and the rest of the renderer
void gld_AddPlane(int subsectornum, visplane_t *floor, visplane_t *ceiling) {}
void gld_AddWall(seg_t *seg) {}
void gld_BeginAutomapDraw(void) {}
void gld_BeginMenuDraw(void) {}
void gld_BeginUIDraw(void) {}
void gld_CleanMemory(void) {}
void gld_CleanStaticMemory(void) {}
void gld_DrawLine(int x0, int y0, int x1, int y1, int BaseColor) {}
void gld_DrawLine_f(float x0, float y0, float x1, float y1, int BaseColor) {}
void gld_DrawMapLines(void) {}
void gld_DrawNumPatch(int x, int y, int lump, dboolean center, int cm, enum patch_translation_e flags) {}
void gld_DrawNumPatch_f(float x, float y, int lump, dboolean center, int cm, enum patch_translation_e flags) {}
void gld_DrawScene(player_t *player) {}
void gld_DrawShaded(int x, int y, int width, int height, int shade) {}
void gld_DrawWeapon(int weaponlump, vissprite_t *vis, int lightlevel) {}
void gld_EndAutomapDraw(void) {}
void gld_EndDrawScene(void) {}
void gld_EndMenuDraw(void) {}
void gld_EndUIDraw(void) {}
void gld_FillBlock(int x, int y, int width, int height, int col) {}
void gld_FillPatch(int lump, int x, int y, int width, int height, enum patch_translation_e flags) {}
void gld_FillRaw(int lump, int x, int y, int src_width, int src_height, int dst_width, int dst_height, enum patch_translation_e flags) {}
void gld_Finish() {}
void gld_FlushTextures(void) {}
void gld_FrustumSetup(void) {}
void gld_Init(int width, int height) {}
void gld_InitCommandLine(void) {}
void gld_InitDrawScene(void) {}
void gld_MapDrawSubsectors(player_t *plr, int fx, int fy, fixed_t mx, fixed_t my, int fw, int fh, fixed_t scale) {}
void gld_MultisamplingInit(void) {}
void gld_MultisamplingSet(void) {}
void gld_PreprocessLevel(void) {}
void gld_ProcessTexturedMap(void) {}
void gld_ProjectSprite(mobj_t* thing, int lightlevel) {}
unsigned char *gld_ReadScreen(void) { return NULL; }
void gld_ResetAutomapTransparency(void) {}
void gld_ResetTexturedAutomap(void) {}
void gld_SetPalette(int palette) {}
void gld_StartDrawScene(void) {}
void gld_UpdateSplitData(sector_t *sector) {}
void gld_clipper_SafeAddClipRange(angle_t startangle, angle_t endangle) {}
dboolean gld_clipper_SafeCheckRange(angle_t startAngle, angle_t endAngle) { return false; }
int gld_wipe_EndScreen(void) { return 0; }
int gld_wipe_StartScreen(void) { return 0; }
int gld_wipe_doMelt(int ticks, int *y_lookup) { return 0; }
int gld_wipe_exitMelt(int ticks) { return 0; }

// OpenGL itself: the textscreen (ENDOOM) draws with it.
void glBegin(GLenum mode) {}
void glBindTexture(GLenum target, GLuint texture) {}
void glClear(GLbitfield mask) {}
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {}
void glDeleteTextures(GLsizei n, const GLuint *textures) {}
void glDisable(GLenum cap) {}
void glEnable(GLenum cap) {}
void glEnd(void) {}
void glGenTextures(GLsizei n, GLuint *textures) {}
void glLoadIdentity(void) {}
void glMatrixMode(GLenum mode) {}
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val) {}
void glScalef(GLfloat x, GLfloat y, GLfloat z) {}
void glTexCoord2f(GLfloat s, GLfloat t) {}
void glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {}
void glTexParameteri(GLenum target, GLenum pname, GLint param) {}
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {}
void glVertex2f(GLfloat x, GLfloat y) {}
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {}

// i_capture.c (video capture through external programs): no processes.
#include <errno.h>
#include <sys/types.h>

int pipe(int fds[2]) { errno = ENOSYS; return -1; }
int execl(const char *path, const char *arg, ...) { errno = ENOSYS; return -1; }
pid_t waitpid(pid_t pid, int *status, int options) { errno = ENOSYS; return -1; }
