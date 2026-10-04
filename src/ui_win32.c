// The Windows GUI's side of ui.h: status bar, log list, message boxes,
// the emulation thread's priority and the message pump while waiting.

#include "stdsdk.h"
#include "listview.h"
#include "ui.h"

extern HWND hwndMain;
extern HWND hwndStatus;

static HANDLE emuthread;

// used by sound.c for the DirectSound init
void *main_gethwnd(void)
{
    return(hwndMain);
}

const char *ui_commandline(void)
{
    return(GetCommandLine());
}

void ui_exepath(char *out,int size)
{
    GetModuleFileName(NULL,out,size);
}

void ui_statustext(int field,const char *text)
{
    SendMessage(hwndStatus,SB_SETTEXT,field,(LPARAM)text);
}

void ui_debugline(const char *text)
{
    AddDebugLine((char *)text,0);
}

void ui_message(const char *title,const char *text)
{
    MessageBox(NULL,text,title,MB_OK);
}

// GetCurrentThread is the pseudo handle "the calling thread": ui_emupriority
// (called by main_stop on the window thread) sets the caller's priority,
// as main.c always did
void ui_setemuthread(void)
{
    emuthread=GetCurrentThread();
}

void ui_emupriority(int high)
{
    SetThreadPriority(emuthread,high?THREAD_PRIORITY_HIGHEST:THREAD_PRIORITY_NORMAL);
}

// Waits up to ms for the emulation thread, letting its SendMessage calls
// through. That thread writes the status bar counters and the log list with
// SendMessage, which only returns once this (the window) thread takes the
// message; a plain Sleep here left it stuck in the call, where it never saw
// the break. Top Gear Overdrive (no idle time, counters sent every frame):
// F6 stopped the game, timed out and saved nothing.
void ui_wait(int ms)
{
    MSG   msg;
    DWORD start=GetTickCount();
    for(;;)
    {
        int left=ms-(int)(GetTickCount()-start);
        // sent messages only: posted ones stay queued for the message loop
        PeekMessage(&msg,NULL,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE);
        if(left<=0) break;
        MsgWaitForMultipleObjects(0,NULL,FALSE,left>10?10:left,QS_SENDMESSAGE);
    }
}
