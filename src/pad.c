#include <windows.h>
#include <xinput.h>
#include "ultra.h"
#include "console.h"

#define CONT_A      0x0080
#define CONT_B      0x0040
#define CONT_G	    0x0020
#define CONT_START  0x0010
#define CONT_UP     0x0008
#define CONT_DOWN   0x0004
#define CONT_LEFT   0x0002
#define CONT_RIGHT  0x0001
#define CONT_L      0x2000
#define CONT_R      0x1000
#define CONT_E      0x0800
#define CONT_D      0x0400
#define CONT_C      0x0200
#define CONT_F      0x0100

#define A_BUTTON	CONT_A
#define B_BUTTON	CONT_B
#define L_TRIG		CONT_L
#define R_TRIG		CONT_R
#define Z_TRIG		CONT_G
#define START       CONT_START
#define U_JPAD		CONT_UP
#define L_JPAD		CONT_LEFT
#define R_JPAD		CONT_RIGHT
#define D_JPAD		CONT_DOWN
#define U_CBUT	CONT_E
#define D_CBUT	CONT_D
#define L_CBUT	CONT_C
#define R_CBUT	CONT_F

typedef struct
{
    word button;
    char stickx;
    char sticky;
} PadStructData;

PadStructData mypad;
word          lastbutton;
int           xnarrow;
int           xcenter;
int           ycenter;
int           joyactive;
int           selectpad;

int           mouseactive;
int           mousedisablecnt;

int           lastjoybuttons;

int  wire=0;
int  info=0;

#define AVERAGE 2

#define MM_PADSTRUCTDATA 0x8033afa8

// GetAsyncKeyState, but only while one of our windows has focus (keys typed
// into other applications must not drive the game)
static SHORT padkey(int vk)
{
    DWORD pid=0;
    GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    if(pid!=GetCurrentProcessId()) return(0);
    return(GetAsyncKeyState(vk));
}

void readjoystick(int *xpos,int *ypos,int *buttons)
{
    JOYINFO joy;
    memset(&joy,0,sizeof(joy));
    joyGetPos(JOYSTICKID1,&joy);
    *xpos=joy.wXpos;
    *ypos=joy.wYpos;
    *buttons=joy.wButtons;
}

void pad_enablejoy(int enable)
{
    joyactive=0;
    mouseactive=0;
    if(enable) joyactive=1;
}

// debugger "autopad": buttons pressed for 3 retraces every autoperiod, for
// unattended test runs (keyboard input would reach the user's desktop)
int pad_automask,pad_autoperiod;
int pad_autostickx,pad_autosticky; // stick held while autopad runs (autopad <period> <mask> <x> <y>)
static int autocount;

void pad_buttons(void)
{
    int a,k;
    mypad.button=0;
    if(pad_autoperiod>0 && autocount%pad_autoperiod<3) mypad.button|=pad_automask;
    k='S'; a=START;    if(padkey(k)<0) mypad.button|=a;
    k='F'; a=L_JPAD;   if(padkey(k)<0) mypad.button|=a;
    k='H'; a=R_JPAD;   if(padkey(k)<0) mypad.button|=a;
    k='T'; a=U_JPAD;   if(padkey(k)<0) mypad.button|=a;
    k='G'; a=D_JPAD;   if(padkey(k)<0) mypad.button|=a;
    k='J'; a=L_JPAD;   if(padkey(k)<0) mypad.button|=a;
    k='L'; a=R_CBUT;   if(padkey(k)<0) mypad.button|=a;
    k='I'; a=U_CBUT;   if(padkey(k)<0) mypad.button|=a;
    k='K'; a=D_CBUT;   if(padkey(k)<0) mypad.button|=a;
    k='F'; a=L_CBUT;   if(padkey(k)<0) mypad.button|=a;
    k='A'; a=A_BUTTON; if(padkey(k)<0) mypad.button|=a;
    k='X'; a=B_BUTTON; if(padkey(k)<0) mypad.button|=a;
    k='Z'; a=Z_TRIG;   if(padkey(k)<0) mypad.button|=a;
    k='C'; a=L_TRIG;   if(padkey(k)<0) mypad.button|=a;
    k='V'; a=R_TRIG;   if(padkey(k)<0) mypad.button|=a;
}

void pad_keyboard(int readasync)
{
    static int lastx,lasty;
    float x,y,l,maxspd;
    int   down;

    if(readasync)
    {
        xcenter=ycenter=xnarrow=0;
        down=padkey(VK_HOME)<0;  if(down) { xcenter=-6*down;  ycenter=+12*down; xnarrow=1; }
        down=padkey(VK_PRIOR)<0; if(down) { xcenter=+6*down;  ycenter=+12*down; xnarrow=1; }
        down=padkey(VK_NEXT)<0;  if(down) { xcenter=+6*down;  ycenter=-12*down; xnarrow=1; }
        down=padkey(VK_END)<0;   if(down) { xcenter=-6*down;  ycenter=-12*down; xnarrow=1; }
        down=padkey(VK_LEFT)<0;  if(down) { xcenter=-16*down;                              }
        down=padkey(VK_RIGHT)<0; if(down) { xcenter=+16*down;                              }
        down=padkey(VK_UP)<0;    if(down) {                   ycenter=+16*down;            }
        down=padkey(VK_DOWN)<0;  if(down) {                   ycenter=-16*down;            }
    }

    if(!xcenter) mypad.stickx/=2;
    else if(xcenter<0 && mypad.stickx>0) mypad.stickx=xcenter*3;
    else if(xcenter>0 && mypad.stickx<0) mypad.stickx=xcenter*3;
    else mypad.stickx+=xcenter;

    if(!ycenter) mypad.sticky/=2;
    else if(ycenter<0 && mypad.sticky>0) mypad.sticky=ycenter*3;
    else if(ycenter>0 && mypad.sticky<0) mypad.sticky=ycenter*3;
    else mypad.sticky+=ycenter;

    maxspd=115;
    if(padkey(VK_SHIFT)<0) maxspd=50;
    if(padkey(VK_CONTROL)<0) maxspd=30;

    x=mypad.stickx;
    y=mypad.sticky;
    l=sqrt(x*x+y*y);
    if(l>maxspd)
    {
        x*=maxspd/l;
        y*=maxspd/l;
    }
    if(xnarrow)
    {
        if(x> 40) x= 40;
        if(x<-40) x=-40;
    }

    mypad.stickx=(x+lastx)/2;
    mypad.sticky=(y+lasty)/2;
    lastx=x;
    lasty=y;
}

void pad_joy(void)
{
    int x,y,b,bc;

    readjoystick(&x,&y,&b);
    if(b && !joyactive)
    {
        joyactive=1;
    }

    bc=lastjoybuttons^b;
    if(joyactive)
    {
        int a;
        if(bc&1)
        {
            a=Z_TRIG;   mypad.button|=a; if(!(b&1)) mypad.button^=a;
        }
        if(bc&2)
        {
            a=A_BUTTON; mypad.button|=a; if(!(b&2)) mypad.button^=a;
        }
        if(bc&4)
        {
            a=R_TRIG;   mypad.button|=a; if(!(b&4)) mypad.button^=a;
        }
        if(bc&8)
        {
            a=B_BUTTON; mypad.button|=a; if(!(b&8)) mypad.button^=a;
        }

        if(x<16384) x=8192;
        if(x>28672 && x<36864) x=32768;
        if(x>49152) x=49152;

        if(y<16384) y=8192;
        if(y>28672 && y<36864) y=32768;
        if(y>49152) y=49152;

        x=(x-32768)*80/16384;
        y=(y-32768)*80/16384;

        mypad.stickx=x;
        mypad.sticky=-y;
    }
    lastjoybuttons=b;
}

/****************************************************************************
** Xbox controller (XInput). Used automatically when one is connected; the
** keyboard keeps working alongside it.
**
**   left stick  -> analog stick     A      -> A      X or B -> B
**   right stick -> C buttons        Start  -> Start  D-pad  -> D-pad
**   left trigger-> Z                LB     -> L      RB or right trigger -> R
*/

#define XPAD_RETRY   60    // frames between scans while no controller is connected
#define XPAD_CSTICK  16000 // right stick deflection that presses a C button

static int xpadindex=-1;   // connected controller (0..3), -1 = none
static int xpadretry;

// Polling an empty XInput slot is slow, so scan for a controller only every
// XPAD_RETRY frames while none is connected.
static int xpad_read(XINPUT_STATE *s)
{
    int i;
    if(xpadindex>=0)
    {
        memset(s,0,sizeof(*s));
        if(XInputGetState(xpadindex,s)==ERROR_SUCCESS) return(1);
        xpadindex=-1;
        xpadretry=0;
    }
    if(xpadretry>0)
    {
        xpadretry--;
        return(0);
    }
    xpadretry=XPAD_RETRY;
    for(i=0;i<XUSER_MAX_COUNT;i++)
    {
        memset(s,0,sizeof(*s));
        if(XInputGetState(i,s)==ERROR_SUCCESS)
        {
            xpadindex=i;
            return(1);
        }
    }
    return(0);
}

// Returns 1 and updates mypad when a controller is connected.
static int pad_xinput(void)
{
    XINPUT_STATE s;
    XINPUT_GAMEPAD *g=&s.Gamepad;
    WORD  b;
    float x,y,l,range;

    if(!xpad_read(&s)) return(0);

    pad_buttons();      // keyboard buttons still count
    pad_keyboard(1);    // keyboard stick, overridden below when the stick moves

    b=g->wButtons;
    if(b&XINPUT_GAMEPAD_A)              mypad.button|=A_BUTTON;
    if(b&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_X)) mypad.button|=B_BUTTON;
    if(b&XINPUT_GAMEPAD_START)          mypad.button|=START;
    if(b&XINPUT_GAMEPAD_DPAD_UP)        mypad.button|=U_JPAD;
    if(b&XINPUT_GAMEPAD_DPAD_DOWN)      mypad.button|=D_JPAD;
    if(b&XINPUT_GAMEPAD_DPAD_LEFT)      mypad.button|=L_JPAD;
    if(b&XINPUT_GAMEPAD_DPAD_RIGHT)     mypad.button|=R_JPAD;
    if(b&XINPUT_GAMEPAD_LEFT_SHOULDER)  mypad.button|=L_TRIG;
    if(b&XINPUT_GAMEPAD_RIGHT_SHOULDER) mypad.button|=R_TRIG;
    if(g->bRightTrigger>XINPUT_GAMEPAD_TRIGGER_THRESHOLD) mypad.button|=R_TRIG;
    if(g->bLeftTrigger >XINPUT_GAMEPAD_TRIGGER_THRESHOLD) mypad.button|=Z_TRIG;

    if(g->sThumbRX> XPAD_CSTICK) mypad.button|=R_CBUT;
    if(g->sThumbRX<-XPAD_CSTICK) mypad.button|=L_CBUT;
    if(g->sThumbRY> XPAD_CSTICK) mypad.button|=U_CBUT;
    if(g->sThumbRY<-XPAD_CSTICK) mypad.button|=D_CBUT;

    // left stick: round dead zone, remaining travel scaled to the N64's +-80
    x=g->sThumbLX;
    y=g->sThumbLY;
    l=sqrt(x*x+y*y);
    if(l>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
    {
        range=32767.0f-XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
        if(l>32767.0f) l=32767.0f;
        l=(l-XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)/range*80.0f/l;
        mypad.stickx=(char)(x*l);
        mypad.sticky=(char)(y*l);
    }
    return(1);
}

void pad_mouse(void)
{
    static float xhistory[AVERAGE];
    static float yhistory[AVERAGE];
    static int   lastb;
    static int   stopcnt=0;
    static int   hi=0;
    float speed=10.0;
    int xi,yi,b,i,sx,sy;

    sx=mypad.stickx;
    sy=mypad.sticky;

    con_readmouserelative(&xi,&yi,&b);

    yi=-yi;
    xhistory[hi]=xi;
    yhistory[hi]=yi;
    hi=(hi+1)%AVERAGE;

    if(b&1)
    {
        sx+=xi/2;
        sy+=yi/2;
        stopcnt=99;
    }
    else
    {
        if(stopcnt>0)
        {
            sx/=2;
            sy/=2;
            stopcnt=0;
        }
        else
        {
            xi=yi=0;
            for(i=0;i<AVERAGE;i++)
            {
                xi+=xhistory[i];
                yi+=yhistory[i];
            }
            sx=xi*4/AVERAGE;
            sy=yi*4/AVERAGE;
        }
    }

    /*
    if(padkey(VK_CONTROL)<0) b|=8;
    else b&=~8;
    */

    if((b^lastb)&2)
    {
        if(b&2) mypad.button|=A_BUTTON; else mypad.button&=~A_BUTTON;
    }
    if((b^lastb)&4)
    {
        if(b&4) mypad.button|=B_BUTTON; else mypad.button&=~B_BUTTON;
    }
    if((b^lastb)&8)
    {
        if(b&8) mypad.button|=Z_TRIG; else mypad.button&=~Z_TRIG;
    }
    lastb=b;

    if(mousedisablecnt==1 && padkey(VK_F6)<0) mouseactive=0;
    if(padkey(VK_F6)==0) mousedisablecnt=1;

    if(sx> 80) sx= 80;
    if(sx<-80) sx=-80;
    if(sy> 80) sy= 80;
    if(sy<-80) sy=-80;

    mypad.stickx=sx;
    mypad.sticky=sy;
}

// The controller is read once for every frame the game finishes
// (pad_drawframe). A game that draws with its CPU finishes none: Namco Museum
// 64 sends no graphics task at all, its buttons stayed zero, and nothing got
// past its Controller Pak prompt. After PAD_QUIET retraces without a frame
// the controller is read at every retrace instead (pad_frame).
#define PAD_QUIET 6
static int padquiet; // retraces since the last finished frame

static void pad_sample(void)
{
    if(!rdp_gfxactive() || st.keyboarddisable)
    {
        // autopad still presses its buttons: unattended runs under a
        // debugger breakpoint command (mc, t) have no active display
        if(pad_autoperiod>0)
        {
            mypad.button=(autocount%pad_autoperiod<3)?pad_automask:0;
            mypad.stickx=pad_autostickx;
            mypad.sticky=pad_autosticky;
        }
        return;
    }

    lastbutton=mypad.button;

    if(mouseactive)
    {
        pad_buttons(); // buttons may be overridden by mouse/joy
        pad_mouse();
    }
    else if(pad_xinput())
    {
        // Xbox controller connected: pad_xinput() filled in mypad
    }
    else if(joyactive)
    {
        pad_buttons(); // buttons may be overridden by mouse/joy
        pad_joy();
    }
    else // keyboard
    {
        pad_buttons();
        pad_keyboard(1);
    }

    if(mypad.stickx> 80) mypad.stickx= 80;
    if(mypad.stickx<-80) mypad.stickx=-80;
    if(mypad.sticky> 80) mypad.sticky= 80;
    if(mypad.sticky<-80) mypad.sticky=-80;
}

void pad_drawframe(void)
{
    padquiet=0;
    pad_sample();
}

void pad_frame(void)
{
    autocount++;
    if(padquiet<PAD_QUIET) padquiet++;
    else pad_sample();
}

dword pad_getdata(int pad)
{
    dword state;
    if(pad==0) state=*(dword *)&mypad;
    else state=0;
    if(!pad) st.padstate=state;
    state=FLIP32(state);
    return(state);
}

void pad_writedata(dword addr)
{
    dword state=pad_getdata(0);
    mem_write32(addr,state);
}

