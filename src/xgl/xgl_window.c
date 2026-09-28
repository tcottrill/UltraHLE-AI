// Game display window for the OpenGL x_* backend. See xgl.h.

#include "xgl_internal.h"
#include "../resources/resource.h"

#define RENDERSTYLE WS_OVERLAPPEDWINDOW

static HWND hwndrender;
static int  lastw,lasth;
static int  fullscreen;        // borderless fullscreen (Alt+Enter)
static RECT savedrect;         // window rect to restore when leaving fullscreen

// Alt+Enter: borderless window covering the monitor, and back.
// Runs on the GUI thread, which owns the window.
static void togglefullscreen(HWND hwnd)
{
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

    hwndrender=CreateWindow("UltraHLEDisplay","UltraHLE",RENDERSTYLE,
                            CW_USEDEFAULT,CW_USEDEFAULT,640,480,
                            (HWND)owner,NULL,(HINSTANCE)hinstance,NULL);
}

void *xgl_window(void)
{
    return hwndrender;
}

// Called from the emulator thread. Sizes the client area to width x height
// the first time a size is seen (later reopens keep the user's size, and
// fullscreen is left alone), and shows or hides the window without
// activating it.
void xgl_showwindow(int width,int height,int show)
{
    if(!hwndrender) return;
    if(show && !fullscreen && width>0 && height>0 && (width!=lastw || height!=lasth))
    {
        RECT r={0,0,width,height};
        AdjustWindowRect(&r,RENDERSTYLE,FALSE);
        SetWindowPos(hwndrender,NULL,0,0,r.right-r.left,r.bottom-r.top,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS);
        lastw=width;
        lasth=height;
    }
    ShowWindowAsync(hwndrender,show?SW_SHOWNA:SW_HIDE);
}
