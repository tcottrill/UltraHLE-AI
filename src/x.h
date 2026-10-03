#ifndef _X_H_
#define _X_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef  __cplusplus
extern "C" {
#endif

#ifndef EXPORT
#define EXPORT extern
#endif

/****************************************************************************
** misc stuff
*/

#pragma warning(disable:4244)
#pragma warning(disable:4305)

typedef unsigned char   uchar;
typedef unsigned short  ushort;
typedef unsigned int    uint;

#define PI 3.1415926535897932384626

#define X_RGB(r,g,b)    ( (((r)<<16) & 0xff0000) | (((g)<<8) & 0xff00) | (((b)<<0) & 0xff) )
#define X_RGBA(r,g,b,a) ( (((a)<<24) & 0xff000000) | (((r)<<16) & 0xff0000) | (((g)<<8) & 0xff00) | (((b)<<0) & 0xff) )
#define X_B(c)          ( ((c)&0xff) >> 0 )
#define X_G(c)          ( ((c)&0xff00) >> 8 )
#define X_R(c)          ( ((c)&0xff0000) >> 16 )

#define X_DEG(a)      ((a)*(65536.0/360.0))

#define X_RAD(a)      ((a)*(32768.0/PI))

#define MAXTEXTURES 1024

/****************************************************************************
** public structs
*/

// input to geometry pipe

typedef struct
{
    int   flags;
    float r,g,b,a;
    float t1s,t1t,t1w;
    float t2s,t2t,t2w;
    float zs;        // screen depth 0..1 for this vertex; <0 = from w.
                     // x_vx uses it once and sets it back to -1
    int   t1clamp;   // texture 1 is sampled inside t1c only (x_vxtexclamp).
    float t1c[4];    // s,t min and s,t max; x_vx uses it once and clears it
} xt_data;

typedef struct
{
    union
    {
        struct
        {
            float x,y,z;
        };
        float v[3];
    };
} xt_pos;

typedef struct
{
    float x,y,z;
    float invz;
    int   clip;
} xt_xfpos;

#define X_CLIPX1   0x00000100
#define X_CLIPX2   0x00000200
#define X_CLIPY1   0x00000400
#define X_CLIPY2   0x00000800
#define X_CLIPZ1   0x00001000
#define X_CLIPZ2   0x00002000

typedef struct
{
    float s,t;
} xt_tex;

// types for matrix/vector (xm_, xv_) routines

typedef struct
{
    float  xax,yax,zax,  x;
    float  xay,yay,zay,  y;
    float  xaz,yaz,zaz,  z;
    float  r1 ,r2 ,r3 , r4;
    int    type;
} xt_matrix;
#define X_MATRIX_IDENT 0
#define X_MATRIX_XFORM 1
#define X_MATRIX_4X4   2

typedef xt_pos xt_vector;

typedef struct
{
    // timing
    int   frametime; // us
    // geometry input
    int   in_vx;
    int   in_tri;    // quads count as 2 etc
    // draw output (feedback not included)
    int   out_tri;
    // state changes
    int   chg_mode;  // mode changes
    int   chg_text;  // texture changes
    // texture
    int   text_total;    // bytes in host memory
    int   text_uploaded; // bytes uploaded since last call
    int   text_resident; // bytes resident last frame
    int   text_used;     // bytes used this last frame
    // extra space
    int   RESERVED[16];
} xt_stats;

/****************************************************************************
** xmain.c
**
** - first call x_init() and before shutdown call x_deinit()
** - x_version returns a description text for the driver
** - x_clear can be used to clear the screen
** - x_finish should be called at end of frame (flips buffers etc)
**
** 3DFX:
** - init screen with call to x_open (specify res)
** - deinit screen with call to x_close
** - x_resize and x_select have no effect
**
** OPENGL:
** - init with call to x_open, note returned context handle
** - if window is resized, call x_resize
** - close context with x_close (use right handle!)
** - if multiple contexts, x_select selects the active one
*/

EXPORT void    x_init(void);
EXPORT void    x_deinit(void);
EXPORT char   *x_version(void);

EXPORT int     x_query(void *hdc,void *hwnd);
#define X_VOODOO1 0x01
#define X_VOODOO2 0x02
EXPORT int     x_open(void *hdc,void *hwnd,int width,int height,int buffers,int vsync);
EXPORT void    x_select(int which);
EXPORT void    x_resize(int width,int height);
EXPORT void    x_close(int which);
EXPORT void    x_fullscreen(int fullscreen);

EXPORT void    x_clear(int writecolor,int writedepth,float cr,float cg,float cb);
EXPORT int     x_readfb(int fb,int x,int y,int xs,int ys,char *buffer,int bufrowlen);
EXPORT int     x_readdepth(float *buffer,int width,int height);
EXPORT int     x_hascontext(void); // calling thread owns the GL context
EXPORT int     x_writefb(int fb,int x,int y,int xs,int ys,char *buffer,int bufrowlen);
#define        X_FB_RGB565   0x11   // 16 bit rgb color
#define        X_FB_RGBA8888 0x12   // 32 bit rgba color
#define        X_FB_FRONT    0x100  // from front buffer
#define        X_FB_BACK     0x200  // from back buffer
EXPORT void    x_finish(void);

EXPORT void    x_getstats(xt_stats *s,int ssize);

/* Utilities (xmain.c) - mostly used internally
**
** - x_fastfpu(1) sets the fpu to 32-bit accuracy mode (faster).
** - x_fastfpu(0) returns the fpu to mode before call to x_fastfpu(1)
** - the above calls can be nested
**
** - x_timems and x_timeus can be used for profiling etc. They return
**   times from the Pentium Performance Counters in milliseconds and
**   microseconds (respectively). Only use for timing intervals.
**   These do take a a few thousand cycles, so don't time small loops.
**
** - x_log is a simple way to output to the file 'x.log'
**   Used for errors and for debugging
*/

EXPORT void    x_fatal(char *txt,...);
EXPORT void    x_log(char *txt,...);
EXPORT void   *x_alloc(int size); // clears memory
EXPORT void   *x_allocfast(int size); // no clearing
EXPORT void   *x_realloc(void *p,int newsize); // no clearing
EXPORT void    x_free(void *blk);
EXPORT void    x_fastfpu(int fast);

EXPORT void    x_timereset(void);
EXPORT int     x_timems(void);
EXPORT int     x_timeus(void);
EXPORT void    x_sleep(int ms);

/****************************************************************************
** xmode.c
**
** All nongeometry settings are controlled with modes. A mode describes
** the full state of the 3D-card. All modes have to be coded into the
** engine, so just pick a free mode number and add the code to xmode.c
**
** This means every mode has to be done separately for every driver,
** but also makes it easy to optimize the modes separately to use the
** features of each card. Also, a mode usually requires a certain amount
** of textures and data (color, alpha, coordinates) to work correctly.
**
** There is a separate mode call for no textures, 1 textures or 2 textures.
** The point here is that the engine wants to know ALL the textures at
** the same time to properly setup multiple tmus. If only one texture
** changes from the last call, only that is respecified to the hardware.
**
*/

// this sets default settings
EXPORT void    x_reset(void);

// flags
EXPORT void    x_forcegeometry(int forceon,int forceoff);
EXPORT void    x_geometry(int flags);
#define X_WIRE       0x00000001 // for debugging, not fast
#define X_CULLFRONT  0x00000002 // NOTE: culling done with glide
#define X_CULLBACK   0x00000004 // so no geometry savings yet
#define X_NOCLIP     0x00000008
#define X_PROJTEXT   0x00000010 // projective texture 1
#define X_DUMPDATA   0x00000020 // debugdump (for full projection only)

// These control rendering settings:
// - all return 0 if mode supported, 1 otherwise
// - in practice all modes we use WILL be supported, somehow :)
// - combine2 and texture2 always return 1 if only one tmu present

EXPORT int     x_mask(int colormask,int depthmask,int depthtest);
#define X_ENABLE      0x1001
#define X_DISABLE     0x1002
// these also allowed for depthtest
#define X_TESTEQ      0x10d1
#define X_TESTNE      0x10d2
#define X_TESTGE      0x10d3
#define X_TESTLE      0x10d4
#define X_TESTGT      0x10d5
#define X_TESTLT      0x10d6

EXPORT int     x_dither(int type); // ENABLE / DISABLE

// Fog settings:
// for X_LINEAR   min/max = eye-z-distances for 0% and 100% of fog
// for X_EXPONENTIAL  max = distance with 90% fog density, min ignored
EXPORT int     x_fog(int type,float min,float max,float r,float g,float b);
//      X_DISABLE
#define X_LINEAR      0x1f01
#define X_EXPONENTIAL 0x1f02
#define X_LINEARADD   0x1f03 // second pass add mode
#define X_FOGSHADE    0x1f04 // the vertex alpha is the amount of fog (RDP blender, raw triangles)
#define X_FOGSHADEINV 0x1f05 // ... of the pixel: 1 - alpha is the fog's

EXPORT int     x_zrange(float znear,float zfar);
EXPORT int     x_zdecal(float factor);

EXPORT int     x_blend(int src,int dst);
#define X_ZERO          0x1201
#define X_ONE           0x1202
#define X_OTHER         0x1203
#define X_ALPHA         0x1204
#define X_OTHERALPHA    0x1205
#define X_INVOTHER      0x1206
#define X_INVALPHA      0x1207
#define X_INVOTHERALPHA 0x1208
#define X_CONSTALPHA    0x1209 // the x_blendalpha value
#define X_INVCONSTALPHA 0x120A
#define X_LASTBLEND     0x120A
EXPORT int     x_blendalpha(float a);

EXPORT int     x_alphatest(float limit); // >=1.0 disabled

EXPORT int     x_combine(int colortext1); // single texture
EXPORT int     x_combine2(int colortext1,int text1text2,int sametex); // dual texture
#define X_WHITE         0x1301 // result is white
#define X_COLOR         0x1302 // just the gouraud rgba
#define X_TEXTURE       0x1303 // just the texture
#define X_ADD           0x1304 // gouraud + texture
#define X_MUL           0x1305 // gouraud * texture
#define X_DECAL         0x1306 // blend(gouraud,texture,texturealpha)
#define X_MULADD        0x1307 // gouraud * texture + gouraud [temp!]
#define X_TEXTURE_IA    0x1308 // texture, alpha=iterated
#define X_MUL_TA        0x1309 // gouraud * texture
#define X_MUL_IA        0x130a // gouraud * texture
#define X_TEXTUREBLEND  0x130b // blend(grouraud,texture,texturealpha)
#define X_TEXTUREENVA   0x130c // blend(envcolor,gouraud,texturealpha)
#define X_TEXTUREENVC   0x130d // blend(envcolor,gouraud,texturecolor)
#define X_SUB           0x130e // gouraud - texture
#define X_TEXTUREENVCR  0x130f // blend(gouraud,envcolor,texturecolor)
#define X_LASTCOMBINE   0x130f

EXPORT int     x_envcolor(float r,float g,float b,float a);

EXPORT int     x_procombine(int rgb,int alpha);
EXPORT int     x_procombine2(int rgb,int alpha,int text1text2,int sametex);

// N64 color combiner: (A-B)*C+D for color and alpha, 1 or 2 cycles, until
// the next x_combine/x_procombine. cc/ac: [cycle][A,B,C,D] input codes:
// 0 combined, 1 texel0 (text1), 2 texel1 (text2), 3 prim, 4 shade (vertex
// color), 5 env, 6 one, 7 zero, 8-13 the alpha of 0-5 as a color, then:
#define X_N64_LODF      14
#define X_N64_PRIMLODF  15
#define X_N64_NOISE     16
#define X_N64_K4        17 // SETCONVERT's K4, K5 (x_n64convert)
#define X_N64_K5        18
#define X_N64_HALF      19
#define X_N64_KEYCENTER 20 // SETKEYR/SETKEYGB's center and scale (x_n64key)
#define X_N64_KEYSCALE  21
#define X_N64_NEXTTEXEL  22 // texel0 of the pixel to the right (x_n64pixel),
#define X_N64_NEXTTEXELA 23 // and its alpha as a color
EXPORT void    x_n64combine(int cycles,const int *cc,const int *ac,
                            const float *prim,const float *env,float primlod);
EXPORT void    x_n64pixel(float width); // an N64 pixel's width in target pixels
EXPORT void    x_n64convert(float k4,float k5);
EXPORT void    x_n64key(const float *center,const float *scale); // r,g,b each

EXPORT int     x_texture(int text1handle); // single texture
EXPORT int     x_texture2(int text1handle,int text2handle); // dual texture
EXPORT int     x_rectclamp(int s,int t);  // clamp the bound textures on these axes (sampler override)

/****************************************************************************
** xtext.c
**
** Texture routines. Textures are selected with the x_mode routines
** when selecting rendering mode, but they are loaded with these
** routines.
**
** Currently all texture data is given as 32 bit RGBA and is
** internally converted to a better format. You can specify the
** suggested format (to control what components get most accuracy)
**
** Texture filtering is specified by the mode.
**
** - routines for doing fast texture anim/recalc will be added later.
** - freetexture doesn't work yet.
*/

// suggested formats for data once loaded to 3dfx
#define X_RGBA5551 0
#define X_RGBA4444 1
#define X_RGBA8888 2 // not supported in 3dfx
#define X_RGB565   3
#define X_I8       4
#define X_IA88     5
#define X_Z16      6
#define X_Z32      7
#define X_FORMATMASK 0xff
// these can be orred to format
#define X_CLAMP    0x100 // clamp this texture (default is loop)
#define X_MIPMAP   0x200 // mipmap texture (you have to provide ALL levels)
#define X_DYNAMIC  0x400 // allows opentexturedata & closetexturedata
#define X_NOBILIN  0x800 // pointsample
#define X_CLAMPNOX 0x1000
#define X_CLAMPNOY 0x2000

EXPORT int     x_createtexture(int format,int width,int height);
EXPORT int     x_loadtexturelevel(int handle,int level,char *data);
EXPORT void    x_freetexture(int handle);
EXPORT void    x_cleartexmem(void); // unloads all textures from accelerator
EXPORT int     x_gettextureinfo(int handle,int *format,int *memformat,int *width,int *height);
EXPORT uchar  *x_opentexturedata(int handle); // check memformat!
EXPORT void    x_closetexturedata(int handle);

/****************************************************************************
** xgeom.c
**
** NOTE: x_matrix is not a high speed thing right now (a few brute force
** 4x4 matrix multiplies each time called). For a large number of
** independent objects a better system has to be designed.
*/

// projection setup
EXPORT void    x_viewport(float x0,float y0,float x1,float y1);
EXPORT void    x_scissor(int on,float x0,float y0,float x1,float y1); // RDP scissor, x1/y1 exclusive
// render to texture: draw into a w x h offscreen target (rgba: its starting
// picture, top row first, or NULL), read rows back as RGBA8, return
EXPORT int     x_rtt_begin(int w,int h,const unsigned char *rgba);
EXPORT void    x_rtt_read(int h,unsigned char *rgba,unsigned char *cover);
// the same for rows y0..y0+h-1, and new pictures for such rows; resume: go
// on with the target as x_rtt_end left it (0 = not w x h any more)
EXPORT int     x_rtt_resume(int w,int h);
EXPORT void    x_rtt_readrows(int y0,int h,unsigned char *rgba,unsigned char *cover);
EXPORT void    x_rtt_writerows(int y0,int h,const unsigned char *rgba);
EXPORT void    x_rtt_end(void);
// cover: what later draws mark their pixels with (1..255), read back per
// pixel by x_rtt_read (0 = not drawn since x_rtt_begin)
EXPORT void    x_rtt_cover(int cover);
EXPORT void    x_frustum(float xmin,float xmax,float ymin,float ymax,float znear,float zfar);
EXPORT void    x_ortho(float xmin,float ymin,float xmax,float ymax,float znear,float zfar);
EXPORT void    x_projmatrix(xt_matrix *matrix); // NULL=ident
// simple version of x_frustum/x_ortho. fov<1 means ortho, fov>1 means perspective
EXPORT void    x_projection(float fov,float znear,float zfar);

// transform setup
EXPORT void    x_cameramatrix(xt_matrix *matrix); // NULL=ident
EXPORT void    x_matrix(xt_matrix *matrix); // NULL=ident
EXPORT void    x_getmatrix(xt_matrix *matrix);

// vertex uploading
#define X_POINTS     1
#define X_TRIANGLES  2
#define X_TRISTRIP   3
#define X_TRIFAN     4
#define X_QUADS      5
#define X_POLYLINE   6
#define X_LINES      7
#define X_QUADSTRIP  8  // for now converted to tristrip
#define X_POLYGON    9  // [MAX 64 vertices!!]
EXPORT void    x_begin(int type);
EXPORT void    x_end(void);
EXPORT void    x_flush(void);
EXPORT void    x_vx(xt_pos *pos,xt_data *data);
EXPORT void    x_vxa(int arrayindex,xt_data *data);
EXPORT void    x_vxarray(xt_pos *pos,int size,char *mask);
// these set fields in the global variables and call x_vx*
#define x_vxcolor(zr,zg,zb)      g_data.r=(zr), g_data.g=(zg), g_data.b=(zb), g_data.a=1.0f
#define x_vxcolor4(zr,zg,zb,za)  g_data.r=(zr), g_data.g=(zg), g_data.b=(zb), g_data.a=(za)
#define x_vxalpha(za)            g_data.a=(za)
#define x_vxtex(zs,zt)           g_data.t1s=(zs), g_data.t1t=(zt)
#define x_vxtexv(st)             g_data.t1s=((float *)(st))[0], g_data.t1t=((float *)(st))[1]
#define x_vxtexp(zs,zt,zw)       g_data.t1s=(zs), g_data.t1t=(zt), g_data.t1w=(zw)
#define x_vxtex2(zs,zt)          g_data.t2s=(zs), g_data.t2t=(zt)
#define x_vxdepth(z)             g_data.zs=(z)
#define x_vxtexclamp(s0,t0,s1,t1) g_data.t1clamp=1, g_data.t1c[0]=(s0), g_data.t1c[1]=(t0), g_data.t1c[2]=(s1), g_data.t1c[3]=(t1)
#define x_vxtex2v(st)            g_data.t2s=((float *)(st))[0], g_data.t2t=((float *)(st))[1]
#define x_vxpos(zx,zy,zz)        g_pos.x=(zx), g_pos.y=(zy), g_pos.z=(zz), x_vx(&g_pos,&g_data)
#define x_vxposv(xyz)            x_vx((xyz),&g_data)
#define x_vxposa(ind)            x_vxa((ind),&g_data)

/****************************************************************************
** Public structures that should be in client data segment (for xv_ macros).
** Currently defined in xmain.c.
*/

extern xt_data  g_data;
extern xt_pos   g_pos;

#ifdef  __cplusplus
}
#endif

#endif//_X_H_

