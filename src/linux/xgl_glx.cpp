// The window system part of the OpenGL x_* backend on Linux (xgl_wgl.c and
// xgl_window.c on Windows): one X11 window from AAE's LinuxWindow, a GLX
// context from AAE's sys_gl code. ui_linux.cpp creates the window on the main
// thread and pumps its events there; the renderer creates the context on the
// emulation thread (x_open), makes it current there and swaps there. Xlib is
// made thread safe by XInitThreads at startup.

#include "aae/linux_window.h"
#include "aae/sys_gl.h"

extern "C" {
#include "../xgl/xgl_internal.h"
}
#include <time.h>
#include <unistd.h>

static LinuxWindow s_window;
static WindowSetup s_setup;
static bool        s_created;

WindowSetup& GetWindowSetup() { return s_setup; }
ISystemWindow& GetSystemWindow() { return s_window; }

// main thread: the game window, 640x480 at first
bool xgl_linux_createwindow(bool fullscreen)
{
    s_setup.windowWidth       =640;
    s_setup.windowHeight      =480;
    s_setup.centerWindow      =true;
    s_setup.useFullscreen     =fullscreen;
    s_setup.cursorClipEnabled =false;   // UltraHLE has no use for the mouse
    s_created=s_window.Create(s_setup);
    return s_created;
}

// main thread: X events; false when the window was closed
bool xgl_linux_pumpevents()
{
    return s_created && s_window.PumpEvents();
}

void xgl_linux_togglefullscreen()
{
    if(!s_created) return;
    s_window.ToggleBorderlessFullscreen();
    // no pointer over a fullscreen game; back for a window
    s_window.SetCursorVisible(!GetWindowSetup().borderlessFullscreen);
}

void xgl_linux_destroywindow()
{
    if(s_created) s_window.Destroy();
    s_created=false;
}

/****************************************************************************
** xgl.h / xgl_internal.h
*/

extern "C" {

void xgl_createwindow(void *hinstance,void *owner)
{
    (void)hinstance; (void)owner; // ui_linux.cpp made it at startup
}

void *xgl_window(void)
{
    return s_created ? s_window.XDisplay() : NULL;
}

// The Linux window is the only one and stays open
void xgl_showwindow(int width,int height,int show)
{
    (void)width; (void)height; (void)show;
}

static struct timespec timebase;
static int             timeset;

void x_timereset(void)
{
    clock_gettime(CLOCK_MONOTONIC,&timebase);
    timeset=1;
}

int x_timeus(void)
{
    struct timespec now;
    if(!timeset) x_timereset();
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (int)((now.tv_sec-timebase.tv_sec)*1000000+(now.tv_nsec-timebase.tv_nsec)/1000);
}

void x_sleep(int ms)
{
    if(ms>0) usleep(ms*1000);
    else sched_yield();
}

void xgl_ctx_error(const char *msg)
{
    bootlog("FATAL: %s",msg);
}

int xgl_ctx_create(void)
{
    // OpenGL 3.3 core at least (AAE asks 4.2 first), current on this thread
    return InitOpenGLContext(false,false,true) ? 1 : 0;
}

void xgl_ctx_destroy(void) { DeleteGLContext(); }
void xgl_ctx_vsync(int on) { glchain_set_vsync(on!=0); }
void xgl_ctx_swap(void)    { glchain_swap_buffers(); }
int  xgl_ctx_current(void) { return sys_gl_context_current() ? 1 : 0; }

void xgl_ctx_size(int *w,int *h)
{
    *w=s_window.ClientWidth();
    *h=s_window.ClientHeight();
}

} // extern "C"
