// State shared between the files of the OpenGL x_* backend.
#ifndef XGL_INTERNAL_H
#define XGL_INTERNAL_H

#include "xgl_gl.h"
#include "../x.h"
#include "xgl.h"

typedef struct
{
    float x,y,w;        // clip-space position; z is derived in the vertex shader
    float r,g,b,a;
    float s1,t1;
    float s2,t2;
    float zs;           // screen depth 0..1, or <0: depth from w (see the shader)
} xgl_vertex;

#define XGL_BATCHVX (3*4096)  // vertices per draw call

// Turns an x_begin/x_vx stream into a triangle list (see xgl_geom.c).
typedef struct
{
    int        type;       // X_TRIANGLES etc, 0 outside x_begin/x_end
    int        count;      // vertices since x_begin
    int        flip;       // tristrip winding toggle
    xgl_vertex first;      // first vertex (fans, polygons)
    xgl_vertex prev[4];    // last four vertices, prev[3] newest
} xgl_assembler;

typedef struct
{
    // display
    HWND   hwnd;
    HDC    hdc;
    HGLRC  hrc;
    int    open;
    int    glready;        // context+shader+geometry exist (survives close/reopen)
    int    xs,ys;          // render size (the FBO)
    int    frame;

    // render target
    GLuint fbo,fbocolor,fbodepth;
    GLuint frontfbo,frontcolor;   // copy of the last finished frame (X_FB_FRONT)
    // render to texture (x_rtt_*): an offscreen target drawn 1:1 in N64
    // pixels, read back into RDRAM by rdp.c; xs/ys are its size meanwhile
    GLuint rttfbo,rttcolor,rttdepth;
    int    rttw,rtth,rtton,mainxs,mainys;
    int    rttcover;       // stencil value draws leave in it (x_rtt_cover)

    // shader
    GLuint prog;
    GLint  u_tex1,u_tex2,u_rgbmode,u_alphamode,u_texmode,u_env,u_alpharef;
    GLint  u_fogmode,u_fogmin,u_fogmax,u_fogcolor,u_deptha,u_depthb;

    // geometry
    GLuint        vao,vbo;
    xgl_vertex    batch[XGL_BATCHVX];
    int           batchn;
    xgl_assembler as;

    // viewport, GL-origin (y runs up from the bottom, inclusive), and depth
    // range; converted from x_viewport's x_* coordinates (y runs down) the
    // same way XGLIDE api.c x_viewport did
    int    view_x0,view_y0,view_x1,view_y1;
    int    sc_on,sc_x0,sc_y0,sc_x1,sc_y1; // RDP scissor, GL-origin, x1/y1 exclusive
    float  znear,zfar,zdecal;
    float  matrix[16];

    // render mode, pushed to GL by xgl_state_apply() at each draw
    int    geometry,geometryon,geometryoff;
    int    colormask,depthmask,depthtest;
    int    blendsrc,blenddst;
    float  blendalpha;  // X_CONSTALPHA
    float  alphatest;
    int    colortext1,text1text2;
    float  env[4];
    int    text1,text2;
    int    rectclamp;   // x_rectclamp: bit 0 clamp s, bit 1 clamp t
    int    fogtype;
    float  fogmin,fogmax,fogcolor[3];
    // N64 combiner (x_n64combine); n64cycles 0 = the x_combine modes
    int    n64cycles,n64cc[8],n64ac[8];
    float  n64prim[4],n64primlod,n64k[2];
    GLint  u_n64cyc,u_cc0,u_cc1,u_ac0,u_ac1,u_prim,u_primlod,u_n64k;
} xgl_state;

extern xgl_state xg;
extern xt_stats  xgl_stats;

// xgl_window.c (safe to call from the emulator thread)
void   xgl_showwindow(int width,int height,int show);

// xgl_fbo.c
int    xgl_fbo_create(int w,int h);
void   xgl_fbo_destroy(void);
void   xgl_fbo_bind(void);
void   xgl_fbo_present(void);
void   xgl_fbo_savefront(void);

// xgl_shader.c
int    xgl_shader_create(void);
void   xgl_shader_destroy(void);

// xgl_tex.c
GLuint xgl_tex_glname(int handle);
int    xgl_tex_nearest(int handle);

// xgl_state.c
void   xgl_state_default(void);
void   xgl_state_apply(void);

// xgl_geom.c
int    xgl_geom_create(void);
void   xgl_geom_destroy(void);
void   xgl_geom_default(void);
void   xgl_applyviewport(void);
void   xgl_applydepth(void);

#endif
