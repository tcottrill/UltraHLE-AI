// Windows part of the OpenGL x_* backend: the wgl 3.3 core context on the
// display window (xgl_window.c), buffer swaps, the clock and error boxes.
// The Linux build has xgl_glx.c in its place.

#include "xgl_internal.h"
#include "../bootlog.h"

static HWND          hwnd;
static HDC           hdc;
static HGLRC         hrc;
static LARGE_INTEGER timebase,timefreq;

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

void x_sleep(int ms) { Sleep(ms); }

void xgl_ctx_error(const char *msg)
{
    MessageBox(NULL,msg,"UltraHLE",MB_OK|MB_ICONERROR);
}

int xgl_ctx_create(void)
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

    hwnd=(HWND)xgl_window();
    bootlog("gl: GetDC");
    hdc=GetDC(hwnd);
    if(!hdc) { bootlog("gl: GetDC failed, error %lu",GetLastError()); return 0; }

    // A window's pixel format can be set only once; the display is closed
    // and reopened (hide3dfx), so keep the format already chosen.
    if(!GetPixelFormat(hdc))
    {
        memset(&pfd,0,sizeof(pfd));
        pfd.nSize     =sizeof(pfd);
        pfd.nVersion  =1;
        pfd.dwFlags   =PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
        pfd.iPixelType=PFD_TYPE_RGBA;
        pfd.cColorBits=32;
        pfd.iLayerType=PFD_MAIN_PLANE;
        bootlog("gl: ChoosePixelFormat");
        fmt=ChoosePixelFormat(hdc,&pfd);
        if(!fmt) { bootlog("gl: ChoosePixelFormat failed, error %lu",GetLastError()); return 0; }
        bootlog("gl: SetPixelFormat %d",fmt);
        if(!SetPixelFormat(hdc,fmt,&pfd))
        {
            bootlog("gl: SetPixelFormat failed, error %lu",GetLastError());
            return 0;
        }
    }

    // Bootstrap context, only to load wglCreateContextAttribsARB
    // (same sequence as Tempest platform/windows/sys_gl.c CreateGLContext).
    bootlog("gl: wglCreateContext");
    temp=wglCreateContext(hdc);
    if(!temp) { bootlog("gl: wglCreateContext failed, error %lu",GetLastError()); return 0; }
    if(!wglMakeCurrent(hdc,temp))
    {
        bootlog("gl: wglMakeCurrent (bootstrap) failed, error %lu",GetLastError());
        wglDeleteContext(temp);
        return 0;
    }
    bootlog("gl: bootstrap context %s / %s / %s",glGetString(GL_VENDOR),
            glGetString(GL_RENDERER),glGetString(GL_VERSION));
    glewExperimental=GL_TRUE;
    if(glewInit()!=GLEW_OK || !wglCreateContextAttribsARB)
    {
        bootlog("gl: glewInit or wglCreateContextAttribsARB failed");
        wglMakeCurrent(NULL,NULL);
        wglDeleteContext(temp);
        return 0;
    }

    bootlog("gl: wglCreateContextAttribsARB 3.3 core");
    hrc=wglCreateContextAttribsARB(hdc,0,attribs);
    wglMakeCurrent(NULL,NULL);
    wglDeleteContext(temp);
    if(!hrc) { bootlog("gl: 3.3 core context failed, error %lu",GetLastError()); return 0; }
    if(!wglMakeCurrent(hdc,hrc))
    {
        bootlog("gl: wglMakeCurrent failed, error %lu",GetLastError());
        return 0;
    }

    // Re-resolve entry points for the core context; glewInit leaves a
    // harmless GL_INVALID_ENUM behind on core profiles.
    glewExperimental=GL_TRUE;
    if(glewInit()!=GLEW_OK) { bootlog("gl: glewInit (core) failed"); return 0; }
    while(glGetError()!=GL_NO_ERROR) {}
    return 1;
}

void xgl_ctx_destroy(void)
{
    if(hrc)
    {
        wglMakeCurrent(NULL,NULL);
        wglDeleteContext(hrc);
        hrc=NULL;
    }
    if(hdc)
    {
        ReleaseDC(hwnd,hdc);
        hdc=NULL;
    }
}

void xgl_ctx_vsync(int on)
{
    if(wglSwapIntervalEXT) wglSwapIntervalEXT(on?1:0);
}

void xgl_ctx_swap(void)
{
    SwapBuffers(hdc);
}

int xgl_ctx_current(void)
{
    return(hrc && wglGetCurrentContext()==hrc);
}

void xgl_ctx_size(int *w,int *h)
{
    RECT rc;
    GetClientRect(hwnd,&rc);
    *w=rc.right;
    *h=rc.bottom;
}
