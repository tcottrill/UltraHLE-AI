// OpenGL 3.3 core implementation of the x_* display API (src/x.h): context
// lifetime, frame present, clears, readback and utilities. Replaces XGLIDE
// (XGLIDE_Decompile/api.c, fx.c).

#include "xgl_internal.h"
#include <stdarg.h>

xgl_state xg;
xt_stats  xgl_stats;

static FILE          *logfile;
static LARGE_INTEGER  timebase,timefreq;

/****************************************************************************
** utilities
*/

void x_log(char *txt,...)
{
    va_list ap;
    if(!logfile) logfile=fopen("x.log","wt");
    if(!logfile) return;
    va_start(ap,txt);
    vfprintf(logfile,txt,ap);
    va_end(ap);
    fflush(logfile);
}

void x_fatal(char *txt,...)
{
    char    buf[1024];
    va_list ap;
    va_start(ap,txt);
    vsnprintf(buf,sizeof(buf),txt,ap);
    va_end(ap);
    x_log("FATAL: %s\n",buf);
    MessageBox(NULL,buf,"UltraHLE",MB_OK|MB_ICONERROR);
    exit(1);
}

void *x_alloc(int size)               { return calloc(1,size); }
void *x_allocfast(int size)           { return malloc(size); }
void *x_realloc(void *p,int newsize)  { return realloc(p,newsize); }
void  x_free(void *blk)               { free(blk); }
void  x_fastfpu(int fast)             { (void)fast; } // x87 precision switch not needed

void x_timereset(void)
{
    QueryPerformanceFrequency(&timefreq);
    QueryPerformanceCounter(&timebase);
}

int x_timeus(void)
{
    LARGE_INTEGER now;
    if(!timefreq.QuadPart) x_timereset();
    QueryPerformanceCounter(&now);
    return (int)((now.QuadPart-timebase.QuadPart)*1000000/timefreq.QuadPart);
}

int   x_timems(void)                  { return x_timeus()/1000; }
void  x_sleep(int ms)                 { Sleep(ms); }
char *x_version(void)                 { return "OpenGL 3.3 core"; }
int   x_query(void *hdc,void *hwnd)   { (void)hdc; (void)hwnd; return 0; }
void  x_select(int which)             { (void)which; }
void  x_resize(int width,int height)  { (void)width; (void)height; } // render size is fixed

void x_getstats(xt_stats *s,int ssize)
{
    static int lasttime;
    int now=x_timeus();
    if(s)
    {
        if(ssize==sizeof(xt_stats))
        {
            memcpy(s,&xgl_stats,sizeof(xt_stats));
            s->frametime=(now==lasttime)?1:now-lasttime;
        }
        else memset(s,0,ssize);
    }
    lasttime=now;
    xgl_stats.in_vx=xgl_stats.in_tri=xgl_stats.out_tri=0;
    xgl_stats.chg_mode=xgl_stats.chg_text=xgl_stats.text_uploaded=0;
}

/****************************************************************************
** context
*/

static int createcontext(void)
{
    static const int attribs[]=
    {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC temp;
    int   fmt;

    xg.hdc=GetDC(xg.hwnd);
    if(!xg.hdc) return 0;

    // A window's pixel format can be set only once; the display is closed
    // and reopened (hide3dfx), so keep the format already chosen.
    if(!GetPixelFormat(xg.hdc))
    {
        memset(&pfd,0,sizeof(pfd));
        pfd.nSize     =sizeof(pfd);
        pfd.nVersion  =1;
        pfd.dwFlags   =PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
        pfd.iPixelType=PFD_TYPE_RGBA;
        pfd.cColorBits=32;
        pfd.iLayerType=PFD_MAIN_PLANE;
        fmt=ChoosePixelFormat(xg.hdc,&pfd);
        if(!fmt || !SetPixelFormat(xg.hdc,fmt,&pfd)) return 0;
    }

    // Bootstrap context, only to load wglCreateContextAttribsARB
    // (same sequence as Tempest platform/windows/sys_gl.c CreateGLContext).
    temp=wglCreateContext(xg.hdc);
    if(!temp) return 0;
    if(!wglMakeCurrent(xg.hdc,temp)) { wglDeleteContext(temp); return 0; }
    glewExperimental=GL_TRUE;
    if(glewInit()!=GLEW_OK || !wglCreateContextAttribsARB)
    {
        wglMakeCurrent(NULL,NULL);
        wglDeleteContext(temp);
        return 0;
    }

    xg.hrc=wglCreateContextAttribsARB(xg.hdc,0,attribs);
    wglMakeCurrent(NULL,NULL);
    wglDeleteContext(temp);
    if(!xg.hrc || !wglMakeCurrent(xg.hdc,xg.hrc)) return 0;

    // Re-resolve entry points for the core context; glewInit leaves a
    // harmless GL_INVALID_ENUM behind on core profiles.
    glewExperimental=GL_TRUE;
    if(glewInit()!=GLEW_OK) return 0;
    while(glGetError()!=GL_NO_ERROR) {}

    x_log("GL_VERSION: %s\nGL_RENDERER: %s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
    return 1;
}

static void destroycontext(void)
{
    if(xg.hrc)
    {
        wglMakeCurrent(NULL,NULL);
        wglDeleteContext(xg.hrc);
        xg.hrc=NULL;
    }
    if(xg.hdc)
    {
        ReleaseDC(xg.hwnd,xg.hdc);
        xg.hdc=NULL;
    }
}

/****************************************************************************
** open / close
*/

void x_init(void)
{
    x_log("Init: %s\n",x_version());
}

// hdc/hwnd are ignored: we render into our own window (xgl_window.c).
// GL objects (context, shader, geometry, FBO, textures) survive close/reopen
// (hide3dfx): rdp.c's rdp_copybackground keeps texture handles for the whole
// program life, as XGLIDE did, so nothing here may invalidate them.
int x_open(void *hdc,void *hwnd,int width,int height,int buffers,int vsync)
{
    (void)hdc; (void)hwnd; (void)buffers;

    xg.hwnd=(HWND)xgl_window();
    if(!xg.hwnd) x_fatal("x_open: display window missing (xgl_createwindow not called)");

    if(!xg.glready)
    {
        xg.xs=width;
        xg.ys=height;
        if(!createcontext())
        {
            destroycontext();
            x_fatal("OpenGL 3.3 core is required but could not be initialized.");
        }
        if(wglSwapIntervalEXT) wglSwapIntervalEXT(vsync?1:0);

        if(!xgl_fbo_create(width,height) || !xgl_shader_create() || !xgl_geom_create())
            x_fatal("x_open: OpenGL setup failed, see x.log");
        xg.glready=1;
    }
    else
    {
        if(width!=xg.xs || height!=xg.ys)
        {
            xgl_fbo_destroy();
            if(!xgl_fbo_create(width,height))
                x_fatal("x_open: OpenGL setup failed, see x.log");
        }
        if(wglSwapIntervalEXT) wglSwapIntervalEXT(vsync?1:0);
    }

    xg.xs=width;
    xg.ys=height;
    xg.open=1;
    xgl_state_default();
    xgl_geom_default();
    x_reset();
    xgl_showwindow(width,height,1);
    x_log("x_open: %ix%i vsync=%i\n",width,height,vsync);
    return 1;
}

void x_close(int which)
{
    (void)which;
    if(!xg.open) return;
    x_flush();
    xg.open=0;
    xgl_showwindow(0,0,0);
    x_log("x_close\n");
}

void x_deinit(void)
{
    x_close(1);
    x_log("Deinit: %s\n",x_version());
}

// Glide switched the Voodoo output on/off here; we show/hide our window.
void x_fullscreen(int fullscreen)
{
    xgl_showwindow(0,0,fullscreen);
}

/****************************************************************************
** frame
*/

void x_clear(int writecolor,int writedepth,float cr,float cg,float cb)
{
    GLbitfield bits=0;

    if(!xg.open) return;
    x_flush();
    xgl_fbo_bind();
    xgl_applyviewport();   // clears are clipped like Glide's grClipWindow
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(cr,cg,cb,1.0f);   // no alpha buffer on the Voodoo; reads as 1.0
    glClearDepth(1.0);
    if(writecolor>=1) bits|=GL_COLOR_BUFFER_BIT;
    if(writedepth>=1) bits|=GL_DEPTH_BUFFER_BIT;
    if(bits) glClear(bits);
    // the render-mode masks are applied again at the next draw
}

void x_finish(void)
{
    if(!xg.open) return;
    x_flush();
    xgl_fbo_savefront();
    xgl_fbo_present();
    SwapBuffers(xg.hdc);
    xgl_fbo_bind();
    x_reset();
    xg.frame++;
}

// 1 if the calling thread can use the GL context (the thread that renders)
int x_hascontext(void)
{
    return(xg.open && wglGetCurrentContext()==xg.hrc);
}

// Only X_FB_RGBA8888 is supported (rdp.c screenshots). Row 0 is the top row;
// bytes are R,G,B,255 as XGLIDE's init_readfb produced. Fails on a thread
// without the GL context (the UI thread), where glReadPixels reads nothing.
int x_readfb(int fb,int x,int y,int xs,int ys,char *buffer,int bufrowlen)
{
    unsigned char *tmp;
    int row,col;

    if(!x_hascontext() || (fb&0xff)!=X_FB_RGBA8888 || 4*xs>bufrowlen) return 1;
    tmp=calloc(xs*ys,4);
    if(!tmp) return 1;

    x_flush();
    // X_FB_FRONT: the last finished frame (the render FBO is already being
    // redrawn by then); otherwise the frame in progress.
    glBindFramebuffer(GL_READ_FRAMEBUFFER,(fb&X_FB_FRONT)?xg.frontfbo:xg.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(x,xg.ys-y-ys,xs,ys,GL_RGBA,GL_UNSIGNED_BYTE,tmp); // GL rows start at the bottom
    xgl_fbo_bind();

    for(row=0;row<ys;row++)
    {
        unsigned char *s=tmp+(ys-1-row)*xs*4;
        unsigned char *d=(unsigned char *)buffer+row*bufrowlen;
        for(col=0;col<xs;col++)
        {
            d[col*4+0]=s[col*4+0];
            d[col*4+1]=s[col*4+1];
            d[col*4+2]=s[col*4+2];
            d[col*4+3]=255;
        }
    }
    free(tmp);
    return 0;
}

int x_writefb(int fb,int x,int y,int xs,int ys,char *buffer,int bufrowlen)
{
    (void)fb; (void)x; (void)y; (void)xs; (void)ys; (void)buffer; (void)bufrowlen;
    return 1; // not implemented in XGLIDE either
}
