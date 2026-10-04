// The Linux front end: main(), ui.h, the keys and the ROM picker's loop.
//
//   ultrahle [<rom> [command ; command ...]]
//
// Without a ROM the window shows the ROM picker (picker.cpp) for the folder
// $ULTRAHLE_ROMS (the Flatpak launcher sets ~/UltraHLE/roms), else roms/ in
// the home folder below. In a game started from the picker, Esc or
// Back+Start goes back to it (B there returns to the game); in the picker,
// and in a game given on the command line, they quit.
//
// The main thread owns the X11 window (AAE's LinuxWindow, via xgl_glx.cpp)
// and pumps its events; main_thread() runs on a second thread as on Windows
// and draws through the GL context it makes current there. A ROM given
// without commands starts at once. Keys do what the Windows accelerators do:
//
//   Esc quit       F5 start        F7 pause/continue
//   F6 quicksave   F9 quickload    F10/F12 screenshot
//   F11 or Alt+Enter fullscreen
//
// and, for SteamOS game mode where there is no keyboard, controller
// shortcuts with Back (View on the Steam Deck; no N64 button uses it), read
// while a game runs (pad_evdev.cpp):
//
//   Back+Start quit (or picker)  Back+RB quicksave  Back+LB quickload
//   Back+Y screenshot   Back+X fullscreen
//
// Ctrl+C or SIGTERM quits cleanly, SIGUSR1 takes a screenshot, SIGUSR2
// saves the picker's next frame (picker.ppm).
//
// The program's home folder (ultra.ini, ultra.log, saves/, states/, snap/)
// is the executable's folder, or $ULTRAHLE_HOME when that is set (the
// Flatpak's own folder is read only).

extern "C" {
#include "../ultra.h"
#include "../bootlog.h"
#include "../ui.h"
extern char startcmd[1024];
extern char startrom[MAXFILE];
}
#include "aae/linux_window.h"
#include "aae/joystick.h"   // AAE_JOYBTN_*
#include "ui_linux.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <time.h>

bool xgl_linux_createwindow(bool fullscreen);
bool xgl_linux_pumpevents();
void xgl_linux_togglefullscreen();
void xgl_linux_destroywindow();
void pad_linux_key(unsigned long keysym, bool down);
void pad_linux_releaseall(void);

static std::string       s_cmdline;
static volatile bool     s_emudone;
static volatile bool     s_emuready;     // main_thread reached its command loop
static volatile int      s_action;       // key action for the main loop
static volatile bool     s_sigquit;      // SIGINT/SIGTERM: quit whatever is showing

static bool              s_picker;       // the ROM picker is showing
static bool              s_frompicker;   // the game was started from the picker
static bool              s_gameloaded;   // a game was started (B returns to it)
static volatile int      s_pickkeys;     // PK_* from the keyboard

/****************************************************************************
** ui.h
*/

extern "C" {

const char *ui_commandline(void)
{
    return s_cmdline.c_str();
}

void ui_exepath(char *out,int size)
{
    const char *home=getenv("ULTRAHLE_HOME");
    if(home && *home)
    {
        // a name after the folder: fixpath strips the last part
        snprintf(out,size,"%s/ultrahle",home);
        return;
    }
    ssize_t n=readlink("/proc/self/exe",out,size-1);
    if(n<0) n=0;
    out[n]=0;
}

void ui_statustext(int field,const char *text)
{
    (void)field; (void)text;
}

void ui_debugline(const char *text)
{
    if(text) fprintf(stderr,"%s\n",text);
}

void ui_message(const char *title,const char *text)
{
    fprintf(stderr,"%s: %s\n",title,text);
}

void ui_setemuthread(void) {}
void ui_emupriority(int high) { (void)high; }

void ui_wait(int ms)
{
    usleep(ms*1000);
}

} // extern "C"

// bootlog_ready() is the last thing main_thread does before its command loop
extern "C" void ui_linux_emuready(void)
{
    s_emuready=true;
}

/****************************************************************************
** keys (called inside PumpEvents on the main thread)
*/

void ui_linux_action(int act)
{
    s_action=act;
}

void linux_window_key(unsigned long keysym, bool down, bool alt, bool shift)
{
    (void)shift;
    pad_linux_key(keysym,down);
    if(!down) return;
    if(s_picker)
    {
        int k=0;
        switch(keysym)
        {
        case XK_Up:        k=PK_UP;     break;
        case XK_Down:      k=PK_DOWN;   break;
        case XK_Prior:     k=PK_PGUP;   break;
        case XK_Next:      k=PK_PGDN;   break;
        case XK_Home:      k=PK_HOME;   break;
        case XK_End:       k=PK_END;    break;
        case XK_KP_Enter:  k=PK_OK;     break;
        case XK_Return:    if(alt) s_action=ACT_FULL; else k=PK_OK; break;
        case XK_BackSpace: k=PK_BACK;   break;
        case XK_Escape:    k=PK_QUIT;   break;
        case XK_F5:        k=PK_RESCAN; break;
        case XK_F11:       s_action=ACT_FULL; break;
        }
        s_pickkeys|=k;
        return;
    }
    switch(keysym)
    {
    case XK_Escape: s_action=ACT_QUIT;  break;
    case XK_F5:     s_action=ACT_START; break;
    case XK_F7:     s_action=ACT_PAUSE; break;
    case XK_F6:     s_action=ACT_SAVE;  break;
    case XK_F9:     s_action=ACT_LOAD;  break;
    case XK_F10:
    case XK_F12:    s_action=ACT_SHOT;  break;
    case XK_F11:    s_action=ACT_FULL;  break;
    case XK_Return: if(alt) s_action=ACT_FULL; break;
    }
}

void linux_window_focus(bool focused)
{
    if(!focused) pad_linux_releaseall();
}

static void do_action(int a)
{
    switch(a)
    {
    case ACT_START: if(!main_executing()) main_start(); break;
    case ACT_PAUSE: if(main_executing()) main_stop(); else main_start(); break;
    case ACT_SAVE:  main_command("save"); break;
    case ACT_LOAD:  main_command("load"); break;
    case ACT_SHOT:  main_screenshot(); break;
    case ACT_FULL:  xgl_linux_togglefullscreen(); break;
    }
}

/****************************************************************************
** main
*/

// Ctrl+C / kill: quit cleanly; kill -USR1: screenshot (as F10)
static void onsignal(int sig)
{
    if(sig==SIGUSR1) s_action=ACT_SHOT;
    else if(sig==SIGUSR2) picker_capture=1;
    else s_sigquit=true;
}

/****************************************************************************
** ROM picker
*/

static int ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (int)(t.tv_sec*1000+t.tv_nsec/1000000);
}

static std::string romfolder(void)
{
    const char *e=getenv("ULTRAHLE_ROMS");
    if(e && *e) return e;
    return std::string(init.rootpath)+"roms";
}

static uint16_t s_padprev;     // pad buttons at the last picker frame
static int      s_dirprev;     // PK_UP/DOWN/PGUP/PGDN held then
static int      s_repeat;      // when a held direction repeats (ms_now)

static void picker_enter(void)
{
    int lx,ly;
    s_picker=true;
    s_pickkeys=0;
    // what is held now (Back+Start that left the game) is not a new press
    s_padprev=pad_linux_menupoll(&lx,&ly);
    s_dirprev=PK_UP|PK_DOWN|PK_PGUP|PK_PGDN;
}

// controller presses for the picker: edges, directions repeat while held
static int picker_padinput(void)
{
    int lx,ly,in=0,dir=0,now=ms_now();
    uint16_t b=pad_linux_menupoll(&lx,&ly);
    uint16_t press=b&~s_padprev;

    if((b&AAE_JOYBTN_BACK) && (press&AAE_JOYBTN_START)) in|=PK_QUIT;
    else if(press&(AAE_JOYBTN_A|AAE_JOYBTN_START))    in|=PK_OK;
    if(press&AAE_JOYBTN_B) in|=PK_BACK;
    if(press&AAE_JOYBTN_Y) in|=PK_RESCAN;

    if((b&AAE_JOYBTN_DPAD_UP)   || ly<-64) dir|=PK_UP;
    if((b&AAE_JOYBTN_DPAD_DOWN) || ly> 64) dir|=PK_DOWN;
    if((b&(AAE_JOYBTN_DPAD_LEFT|AAE_JOYBTN_LEFT_SHOULDER))   || lx<-64) dir|=PK_PGUP;
    if((b&(AAE_JOYBTN_DPAD_RIGHT|AAE_JOYBTN_RIGHT_SHOULDER)) || lx> 64) dir|=PK_PGDN;
    if(dir&~s_dirprev) { in|=dir&~s_dirprev; s_repeat=now+350; }
    else if(dir && now>=s_repeat) { in|=dir; s_repeat=now+70; }

    s_padprev=b;
    s_dirprev=dir;
    return in;
}

// one pass of the main loop while the picker shows; false to quit
static bool picker_pass(void)
{
    static int next;
    char path[MAXFILE];
    int  in=s_pickkeys|picker_padinput();
    s_pickkeys=0;
    if(!in && ms_now()<next) return true;   // about 60 frames a second
    next=ms_now()+16;

    switch(picker_frame(in,s_gameloaded,path,sizeof(path)))
    {
    case PICK_QUIT:
        return false;
    case PICK_RESUME:
        s_picker=false;
        main_start();
        break;
    case PICK_LAUNCH:
        if(!s_emuready) break;              // main_thread still starting
        bootlog("picker: %s",path);
        main_command("rom \"%s\"",path);
        if(main_commanderrors()>1)
        {
            std::string m="Could not load "+std::string(strrchr(path,'/')?strrchr(path,'/')+1:path);
            picker_message(m.c_str());
            break;
        }
        picker_message("");
        s_picker=false;
        s_frompicker=true;
        s_gameloaded=true;
        main_start();
        break;
    }
    return true;
}

static void *emuthread(void *arg)
{
    (void)arg;
    bootlog("emulator thread started");
    main_thread();
    bootlog("emulator thread finished");
    s_emudone=true;
    return NULL;
}

int main(int argc,char **argv)
{
    pthread_t emu;
    bool      quit=false;

    // the emulation thread renders and swaps on the window this thread owns
    XInitThreads();

    signal(SIGINT,onsignal);
    signal(SIGTERM,onsignal);
    signal(SIGUSR1,onsignal);
    signal(SIGUSR2,onsignal);

    for(int i=0;i<argc;i++)
    {
        if(i) s_cmdline+=' ';
        if(strchr(argv[i],' ')) s_cmdline+='"'+std::string(argv[i])+'"';
        else s_cmdline+=argv[i];
    }

    bootlog_begin();
    bootlog("main_startup");
    main_startup();

    if(!xgl_linux_createwindow(false))
    {
        fprintf(stderr,"UltraHLE: no X11 display (DISPLAY=%s)\n",getenv("DISPLAY")?getenv("DISPLAY"):"");
        return 1;
    }

    bool autostart=*startrom && !*startcmd;
    if(!*startrom)
    {
        picker_scan(romfolder().c_str());
        picker_enter();
    }
    if(pthread_create(&emu,NULL,emuthread,NULL))
    {
        fprintf(stderr,"UltraHLE: cannot start the emulation thread\n");
        return 1;
    }

    while(!quit && !s_emudone && !s_sigquit)
    {
        if(!xgl_linux_pumpevents()) quit=true;   // window closed
        if(autostart && s_emuready)
        {
            autostart=false;
            main_start();
        }
        int a=s_action;
        s_action=ACT_NONE;
        if(s_picker)
        {
            if(a==ACT_FULL || a==ACT_SHOT) do_action(a);
            if(!picker_pass()) quit=true;
        }
        else if(a==ACT_QUIT)
        {
            if(s_frompicker)
            {
                main_stop();      // back to the list; B continues this game
                picker_enter();
            }
            else quit=true;
        }
        else if(a) do_action(a);
        usleep(5000);
    }

    // end main_thread through its own exit command, as the Windows GUI does
    if(!s_emudone) main_command("x");
    pthread_join(emu,NULL);
    xgl_linux_destroywindow();
    bootlog("exit");
    return 0;
}
