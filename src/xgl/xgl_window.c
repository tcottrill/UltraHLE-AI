// Game display window for the OpenGL x_* backend. See xgl.h.

#include "xgl_internal.h"
#include "../resources/resource.h"

#define RENDERSTYLE WS_OVERLAPPEDWINDOW

static HWND hwndrender;
static int  lastw,lasth;
static int  fullscreen;        // borderless fullscreen (Alt+Enter)
static RECT savedrect;         // window rect to restore when leaving fullscreen

// Mouse capture, as in C64Emu's WindowCode.cpp: a click in the window hides
// the cursor and confines it to the client area; F11, losing the focus or
// minimizing releases it. Going fullscreen captures, going back to a window
// releases. No SetCapture: only ClipCursor, and only while focused.
#define TITLE         "UltraHLE"
#define TITLECAPTURED "UltraHLE - Mouse Captured (F11 to release)"
static int  mousecaptured;
static int  hasfocus,minimized;

// applies the state: clipped and hidden only when captured, focused and not
// minimized
static void updatecursor(HWND hwnd)
{
    if(mousecaptured && hasfocus && !minimized)
    {
        RECT  rc;
        POINT tl,br;
        GetClientRect(hwnd,&rc);
        tl.x=rc.left;  tl.y=rc.top;
        br.x=rc.right; br.y=rc.bottom;
        ClientToScreen(hwnd,&tl);
        ClientToScreen(hwnd,&br);
        SetRect(&rc,tl.x,tl.y,br.x,br.y);
        ClipCursor(&rc);
        SetCursor(NULL); // WM_SETCURSOR keeps it hidden from here on
    }
    else ClipCursor(NULL);
}

static void capturemouse(HWND hwnd)
{
    if(mousecaptured) return;
    mousecaptured=1;
    updatecursor(hwnd);
    SetWindowText(hwnd,TITLECAPTURED);
}

static void releasemouse(HWND hwnd)
{
    int was=mousecaptured;
    mousecaptured=0;
    ClipCursor(NULL); // always, even if the flag was out of step
    if(was)
    {
        SetWindowText(hwnd,TITLE);
        SetCursor(LoadCursor(NULL,IDC_ARROW));
    }
}

// Alt+Enter: borderless window covering the monitor, and back.
// Runs on the GUI thread, which owns the window.
static void togglefullscreen(HWND hwnd)
{
    releasemouse(hwnd);
    if(!fullscreen)
    {
        MONITORINFO mi;
        mi.cbSize=sizeof(mi);
        GetWindowRect(hwnd,&savedrect);
        GetMonitorInfo(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi);
        SetWindowLong(hwnd,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        SetWindowPos(hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,
                     mi.rcMonitor.right-mi.rcMonitor.left,
                     mi.rcMonitor.bottom-mi.rcMonitor.top,
                     SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
        fullscreen=1;
        capturemouse(hwnd); // fullscreen captures without a click
    }
    else
    {
        SetWindowLong(hwnd,GWL_STYLE,RENDERSTYLE|WS_VISIBLE);
        SetWindowPos(hwnd,NULL,savedrect.left,savedrect.top,
                     savedrect.right-savedrect.left,savedrect.bottom-savedrect.top,
                     SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOOWNERZORDER);
        fullscreen=0;
    }
}

static LRESULT CALLBACK renderproc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SETFOCUS:
        hasfocus=1;
        if(fullscreen) capturemouse(hwnd); // coming back to a fullscreen game
        updatecursor(hwnd);
        return 0;
    case WM_KILLFOCUS:
        hasfocus=0;
        releasemouse(hwnd);
        return 0;
    case WM_SIZE:
        minimized=(wp==SIZE_MINIMIZED);
        if(minimized) releasemouse(hwnd);
        else updatecursor(hwnd);
        break;
    case WM_MOVE:
        updatecursor(hwnd); // the clip rectangle is in screen coordinates
        break;
    case WM_SETCURSOR:
        if(hasfocus && mousecaptured && LOWORD(lp)==HTCLIENT)
        {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if(GetFocus()!=hwnd) SetFocus(hwnd);
        capturemouse(hwnd);
        return 0;
    case WM_KEYDOWN:
        if(wp==VK_F11)
        {
            releasemouse(hwnd);
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if(wp==VK_RETURN && (lp&(1<<29)) && !(lp&(1<<30))) // Alt+Enter, not auto-repeat
        {
            togglefullscreen(hwnd);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if(wp==VK_RETURN) return 0;  // no error beep for Alt+Enter
        break;
    case WM_ERASEBKGND:
        return 1;              // OpenGL paints the whole client area
    case WM_CLOSE:
        // same as choosing Emulation/Stop: stops emulation and closes the display
        PostMessage(GetWindow(hwnd,GW_OWNER),WM_COMMAND,IDM_EMULATION_STOP,0);
        return 0;
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}

void xgl_createwindow(void *hinstance,void *owner)
{
    WNDCLASS wc;

    memset(&wc,0,sizeof(wc));
    wc.style        =CS_OWNDC;
    wc.lpfnWndProc  =renderproc;
    wc.hInstance    =(HINSTANCE)hinstance;
    wc.hCursor      =LoadCursor(NULL,IDC_ARROW);
    wc.lpszClassName="UltraHLEDisplay";
    RegisterClass(&wc);

    hwndrender=CreateWindow("UltraHLEDisplay",TITLE,RENDERSTYLE,
                            CW_USEDEFAULT,CW_USEDEFAULT,640,480,
                            (HWND)owner,NULL,(HINSTANCE)hinstance,NULL);
}

void *xgl_window(void)
{
    return hwndrender;
}

// Called from the emulator thread. Sizes the client area to width x height
// the first time a size is seen (later reopens keep the user's size, and
// fullscreen is left alone), and shows or hides the window. A window that
// opens goes to the top of the z order and is activated; one that is already
// open is left where the user put it.
void xgl_showwindow(int width,int height,int show)
{
    int opens;
    if(!hwndrender) return;
    opens=show && !IsWindowVisible(hwndrender);
    if(show && !fullscreen && width>0 && height>0 && (width!=lastw || height!=lasth))
    {
        RECT r={0,0,width,height};
        AdjustWindowRect(&r,RENDERSTYLE,FALSE);
        SetWindowPos(hwndrender,NULL,0,0,r.right-r.left,r.bottom-r.top,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);
        lastw=width;
        lasth=height;
    }
    // it was created hidden at startup and kept that place in the z order:
    // shown with SW_SHOWNA it came up behind whatever had been used since
    if(opens) SetWindowPos(hwndrender,HWND_TOP,0,0,0,0,
                           SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW|SWP_ASYNCWINDOWPOS);
    else ShowWindowAsync(hwndrender,show?SW_SHOWNA:SW_HIDE);
}
