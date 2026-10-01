#include "stdsdk.h" // includes ultra.h
#include "listview.h" // includes ultra.h
#include "bootlog.h"
extern HWND hwndMain;

// startup settings
char  startcmd[1024];
char  startrom[MAXFILE];

static HANDLE emuthreadhandle;

extern char romfilename[];

int godisabled;

// used by SOUND.C for directsound init
void *main_gethwnd(void)
{
    return(hwndMain);
}

void printcopyright(void)
{
    print("\x1\xf""%s V%i.%i.%i - %s",APPNAME, MAJORREV,MINORREV,PATCHLVL, TITLE);
    print("\n\x1\xf"COPYRIGHT1);
    print("\n\x1\x7"COPYRIGHT2);
    print("\n");
}

#define MAXRET 1024
static char cmdin[MAXRET];
static char cmdret[MAXRET];
static int  cmdretcnt;
static int  cmderrs;

void debugmessage(char *txt)
{
    static char last[1024];
    if(!txt)
    {
        AddDebugLine(NULL,0);
        return;
    }
    if(strncmp(txt,last,sizeof(last)-1))
    {
        static char tmp[1024];
        int i;

        // bounded copies: messages can carry long file paths
        strncpy(last,txt,sizeof(last)-1);
        last[sizeof(last)-1]=0;

        i=(int)strlen(last);
        memcpy(tmp,last,i+1);
        if(i>0 && tmp[i-1]=='\n') tmp[i-1]=0; // remove enter

        AddDebugLine(tmp,0);
    }
}

void outputhook(char *txt,char *full)
{
    int l;

    // console output comes here for further processing.
    // Exceptions start with 'exception(address):' and
    // errors and warnings similarly to help finding them.

    // the txt parameter is prefiltered to remove common
    // color codes at start, the full parameter is the full
    // line including color codes (0x01,0x0# where # is 1-15).

    // track errors
    if(!memcmp(txt,"exception(",10))
    {
        debugmessage(txt);
        if(cmderrs<3) cmderrs=3;
    }
    if(!memcmp(txt,"error(",6))
    {
        debugmessage(txt);
        if(cmderrs<2) cmderrs=2;
    }
    if(!memcmp(txt,"warning(",8))
    {
        debugmessage(txt);
        if(cmderrs<1) cmderrs=1;
    }
    if(!memcmp(txt,"note:",5))
    {
        debugmessage(txt);
    }

    // just quit if buffer already full
    if(cmdretcnt>=MAXRET-1) return;

    l=strlen(txt);
    if(l+cmdretcnt>=MAXRET-1) l=MAXRET-1-cmdretcnt;

    memcpy(cmdret+cmdretcnt,txt,l);
    cmdret[cmdretcnt+l]=0;
    cmdretcnt+=l+1;
}

int main_executing(void)
{
    return(st.executing);
}

// Waits up to ms for the emulation thread, letting its SendMessage calls
// through. That thread writes the status bar counters and the log list with
// SendMessage, which only returns once this (the window) thread takes the
// message; a plain Sleep here left it stuck in the call, where it never saw
// the break. Top Gear Overdrive (no idle time, counters sent every frame):
// F6 stopped the game, timed out and saved nothing.
static void main_wait(int ms)
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

// starts execution (might take a while)
int main_start(void)
{
    int i;

    print("info: emulation started\n");

    st.started=1;

    dlist_ignoregraphics(0);

    // Temporary solution. go runs too fast on fast processors and Zelda hangs when the moon appears.
#if !RELEASE
    breakcommand("go");
#else
    breakcommand("sgo");
#endif

    // wait for execution to start (up to 0.5 sec)
    for(i=0;i<500;i+=10)
    {
        if(st.executing) return(0);
        main_wait(10);
    }

    // execution did not start (emu stuck?)
    return(1);
}

// stops execution (might take a while)
int main_stop(void)
{
    int i;

    print("info: emulation stopped\n");

    st.started=0;

    SetThreadPriority(emuthreadhandle,THREAD_PRIORITY_HIGHEST);

    if(!main_executing())
    {
        SetThreadPriority(emuthreadhandle,THREAD_PRIORITY_NORMAL);
        return(0);
    }

    // first try a nice break (wait up to 0.5 sec)
    for(i=0;i<50;i++)
    {
        st.nicebreak=1;
        main_wait(10);
        if(!main_executing())
        {
            SetThreadPriority(emuthreadhandle,THREAD_PRIORITY_NORMAL);
            return(0);
        }
    }

    // emulation pretty stuck, do a more forceful break (wait 1 sec)
    for(i=0;i<100;i++)
    {
        cpu_break();
        main_wait(10);
        if(!main_executing())
        {
            SetThreadPriority(emuthreadhandle,THREAD_PRIORITY_NORMAL);
            return(0);
        }
    }

    // problems, emulation is really hung up :(
//    exception("main_stop failed (breakout=%i, bailout=%i, exec=%i)!",st.breakout,st.bailout,st.executing);
    flushdisplay();

    SetThreadPriority(emuthreadhandle,THREAD_PRIORITY_NORMAL);
    return(1);
}

char *main_command(char *cmd,...)
{
    va_list argp;
    int     wasexecuting=st.executing;

    // clear return
    cmdretcnt=0;
    cmdret[0]=0;
    cmderrs=0;

    if(wasexecuting)
    {
        if(main_stop())
        {
            // coulnd't stop, fatal error; said in the log (a quicksave
            // dropped here used to leave no trace at all)
            print("note: emulation did not stop, '%s' was not run\n",cmd);
            cmderrs=3;
            return("exception(00000000): Cannot stop execution");
        }
    }

    // expand printf-style
    va_start(argp,cmd);
    vsprintf(cmdin,cmd,argp);

    // this will first wait emulation is at a suitable point
    // (if executing) and then break it, and then execute the
    // command. If you want emulatio to continue, append ';go'
    // to the command.

    {
        godisabled=1;
        command(cmdin);
        godisabled=0;
    }

    cmdretcnt=99999; // no return additions anymore

    if(wasexecuting)
    {
        // was executing when called, restart
        main_start();
    }

    // return all printed text from last command
    return(cmdret);
}

char *main_commandreturn(void)
{
    return(cmdret);
}

int main_commanderrors(void)
{
    return(cmderrs);
}

void main_hide3dfx(void)
{
    if(main_executing() && st.started)
    {
        breakcommand("hide3dfx;go");
    }
    else
    {
        breakcommand("hide3dfx");
    }
}

// only a request (served on the emulator thread at the next frame or
// retrace): main_command would stop and restart emulation around it
void main_screenshot(void)
{
    rdp_screenshot(NULL);
}

void main_show3dfx(void)
{
    if(main_executing() && st.started)
    {
        breakcommand("show3dfx;go");
    }
    else
    {
        breakcommand("show3dfx");
    }
}

LRESULT CALLBACK thread_window_proc (HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
     switch (iMsg)
     {
          case WM_CREATE :
               return 0;

          case WM_SIZE :
               return 0;

          case WM_PAINT :
               {
                   PAINTSTRUCT ps;
                   BeginPaint(hwnd, &ps);
                   EndPaint(hwnd, &ps);
               }
               return 0;

          case WM_DESTROY :
               PostQuitMessage (0) ;
               return 0 ;
     }

     return DefWindowProc (hwnd, iMsg, wParam, lParam) ;
}

void help(void)
{
    if(!RELEASE)
    {
        MessageBox(NULL,
               "usage: ULTRA [options] <romfile> [command ; command ; ...]\n"
               "\n"
               "options:\n"
               "-r#  select 3dfx screen width (height is 0.75*width), use:\n"
               "       -r1024 for 1024x768 (Voodoo2 SLI / Banshee)\n"
               "       -r800  for  800x600 (Voodoo2 / Banshee)\n"
               "       -r640  for  640x480 (default)\n"
               "       -r512  for  512x384 (fastest on Voodoo1)\n"
               "-v   disable voodoo2 extensions\n"
               "-s   shutdown glide when returning to console (F12) instead\n"
               "     of just flipping screen (for Banshee)\n"
               "-n   no file mapping for preflipped roms (load whole rom)\n"
               "-c   show console\n"
               ,"UltraHLE",MB_OK);
    }
}

// appends '\' to path if it doesn't contain it
void fixpath(char *path,int striplastname)
{
    char *p;
    if(!*path)
    {
        return;
    }
    if(striplastname)
    {
        // strip exe name from above
        p=path+strlen(path);
        while(p>path+1 && *p!=':' && *p!='\\')
        {
            p--;
        }
    }
    else p=path+strlen(path)-1;
    // append '\\' if not at end
    if(p[0]!='\\') *++p='\\';
    // end string
    p[1]=0;
}

// Next command line argument into out (size bytes), Windows style: spaces
// inside "..." belong to the argument and the quotes are removed, so
// "C:\Program Files\UltraHLE\UltraHLE64.exe" is one argument. Returns 0 at
// the end of the line.
static int nextarg(char **cmd,char *out,int size)
{
    char *s=*cmd;
    int   n=0,quoted=0;

    while(*s && *s<=32) s++;
    if(!*s) { *cmd=s; return(0); }
    while(*s && (quoted || *s>32))
    {
        if(*s=='"') quoted=!quoted;
        else if(n<size-1) out[n++]=*s;
        s++;
    }
    out[n]=0;
    *cmd=s;
    return(1);
}

void main_startup(void)
{
    char *cmd;
    char  arg[MAXFILE];
    int   showhelp=0;
    int   romname=0;
    int   a;

    cmd=GetCommandLine();

    GetModuleFileName(NULL,init.rootpath,sizeof(init.rootpath));
    fixpath(init.rootpath,1);

    // defaults
    init.gfxwid=640;
    init.gfxhig=480;

    // skip name of executable (quoted when its folder has spaces)
    nextarg(&cmd,arg,sizeof(arg));

    // command line options
    while(nextarg(&cmd,arg,sizeof(arg)))
    {
        // check for '-' or '/'
        if(*arg=='-' || *arg=='/')
        {
            switch(arg[1])
            {
            case 'c':
                init.showconsole=1;
                break;
            case 'n':
                init.nomemmap=1;
                break;
            case 'v':
                init.novoodoo2=1;
                break;
            case 's':
                init.shutdownglide=1;
                break;
            case 'r':
                a=atoi(arg+2);
                if(a<320 || a>2048) a=640;
                init.gfxwid=a;
                init.gfxhig=480*a/640;
                break;
            default:
                showhelp=1;
                break;
            }
        }
        else if(!romname)
        {
            // override romfile name, skip '!' if in filename
            strncpy(startrom,*arg=='!'?arg+1:arg,sizeof(startrom)-1);
            romname=1; // name read
        }
        else if(strlen(startcmd)+strlen(arg)+4<sizeof(startcmd))
        {
            // add to startup commands (space separates args); an argument
            // with spaces (a quoted path) gets its quotes back for command()
            strcat(startcmd," ");
            if(strchr(arg,' '))
            {
                strcat(startcmd,"\"");
                strcat(startcmd,arg);
                strcat(startcmd,"\"");
            }
            else strcat(startcmd,arg);
        }
    }

    if(showhelp)
    {
        help();
        *startcmd=0;
        *startrom=0;
    }

    // load default settings from inifile (these could set paths
    // and other stuff)
    inifile_read("default");
}

void main_thread(void)
{
    static char romname[256];

    emuthreadhandle=GetCurrentThread();

    bootlog("cmd_init");
    cmd_init();

    // open console
    bootlog("view_open");
    view_open();
    view_changed(VIEW_RESIZE);
    flushdisplay();

    // copyrights
    printcopyright();

print(CYAN"startrom: %s\n",startrom);
print(CYAN"startcmd: %s\n",startcmd);
print(CYAN"rootpath: %s ",init.rootpath);
print(CYAN"savepath: %s ",init.savepath);
print(CYAN"rompath: %s\n",init.rompath);

    view_status("startup commands");
    // process optional commands on command line
    bootlog("boot %s",*startrom?startrom:"(empty cart)");
    if(*startrom)
    {
        boot(startrom,init.nomemmap);
    }
    else
    {
        // no rom specified, this will create a dummy empty cart
        boot(NULL,0);
    }
    if(*startcmd)
    {
        command(startcmd);
        *startcmd=0;
    }

    // help texts
    print("\x1\x3""Type 'help' for help or press F5 to start.\n");
    view_status("ready");
    bootlog_ready();

    // main debugui loop
    debugui();

    // reset 3dfx
    rdp_closedisplay();
    // close console
    view_close();
}
