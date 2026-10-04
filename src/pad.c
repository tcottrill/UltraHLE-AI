// N64 controllers from the host input in padin.h (pad_win32.c on Windows)

#include "ultra.h"
#include "console.h"
#include "padin.h"

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
    k='S'; a=START;    if(padin_key(k)) mypad.button|=a;
    k='F'; a=L_JPAD;   if(padin_key(k)) mypad.button|=a;
    k='H'; a=R_JPAD;   if(padin_key(k)) mypad.button|=a;
    k='T'; a=U_JPAD;   if(padin_key(k)) mypad.button|=a;
    k='G'; a=D_JPAD;   if(padin_key(k)) mypad.button|=a;
    k='J'; a=L_JPAD;   if(padin_key(k)) mypad.button|=a;
    k='L'; a=R_CBUT;   if(padin_key(k)) mypad.button|=a;
    k='I'; a=U_CBUT;   if(padin_key(k)) mypad.button|=a;
    k='K'; a=D_CBUT;   if(padin_key(k)) mypad.button|=a;
    k='F'; a=L_CBUT;   if(padin_key(k)) mypad.button|=a;
    k='A'; a=A_BUTTON; if(padin_key(k)) mypad.button|=a;
    k='X'; a=B_BUTTON; if(padin_key(k)) mypad.button|=a;
    k='Z'; a=Z_TRIG;   if(padin_key(k)) mypad.button|=a;
    k='C'; a=L_TRIG;   if(padin_key(k)) mypad.button|=a;
    k='V'; a=R_TRIG;   if(padin_key(k)) mypad.button|=a;
}

void pad_keyboard(int readasync)
{
    static int lastx,lasty;
    float x,y,l,maxspd;
    int   down;

    if(readasync)
    {
        xcenter=ycenter=xnarrow=0;
        down=padin_key(PADIN_VK_HOME);  if(down) { xcenter=-6*down;  ycenter=+12*down; xnarrow=1; }
        down=padin_key(PADIN_VK_PRIOR); if(down) { xcenter=+6*down;  ycenter=+12*down; xnarrow=1; }
        down=padin_key(PADIN_VK_NEXT);  if(down) { xcenter=+6*down;  ycenter=-12*down; xnarrow=1; }
        down=padin_key(PADIN_VK_END);   if(down) { xcenter=-6*down;  ycenter=-12*down; xnarrow=1; }
        down=padin_key(PADIN_VK_LEFT);  if(down) { xcenter=-16*down;                              }
        down=padin_key(PADIN_VK_RIGHT); if(down) { xcenter=+16*down;                              }
        down=padin_key(PADIN_VK_UP);    if(down) {                   ycenter=+16*down;            }
        down=padin_key(PADIN_VK_DOWN);  if(down) {                   ycenter=-16*down;            }
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
    if(padin_key(PADIN_VK_SHIFT)) maxspd=50;
    if(padin_key(PADIN_VK_CONTROL)) maxspd=30;

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

    padin_joy(&x,&y,&b);
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
** Xbox controllers (XInput), up to four: one per N64 controller port. The
** keyboard keeps working alongside the pad on port 1.
**
**   left stick  -> analog stick     A      -> A      X or B -> B
**   right stick -> C buttons        Start  -> Start  D-pad  -> D-pad
**   left trigger-> Z                LB     -> L      RB or right trigger -> R
**
** Hotplug: a pad that connects takes the lowest free port and keeps it until
** it is unplugged, so a single pad is always controller 1 and the other
** players don't move when one pad drops out.
*/

#define XPAD_CSTICK   16000 // right stick deflection that presses a C button
#define XPAD_DEADZONE 7849  // left stick (XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
#define XPAD_TRIGGER  30    // trigger pressed (XINPUT_GAMEPAD_TRIGGER_THRESHOLD)

static int        xpadport[PADIN_SLOTS]={-1,-1,-1,-1}; // gamepad slot -> N64 port, -1 = not connected
static PadInState xpadstate[PADIN_SLOTS];
static int        xpadmotor[PADIN_SLOTS]; // motor state sent to each slot

static PadStructData otherpad[PAD_PORTS-1];          // ports 2-4 (port 1 is mypad)

static PadStructData *pad_port(int port)
{
    return(port==0?&mypad:&otherpad[port-1]);
}

// XInput slot on N64 port 'port', -1 = none
static int xpad_onport(int port)
{
    int i;
    for(i=0;i<PADIN_SLOTS;i++) if(xpadport[i]==port) return(i);
    return(-1);
}

static int xpadscanned; // xpad_scan has run

// Poll every slot and assign ports to pads that come and go.
static void xpad_scan(void)
{
    int i,port;
    xpadscanned=1;
    for(i=0;i<PADIN_SLOTS;i++)
    {
        if(padin_pad(i,&xpadstate[i]))
        {
            if(xpadport[i]>=0) continue;
            for(port=0;port<PAD_PORTS && xpad_onport(port)>=0;port++) ;
            if(port<PAD_PORTS)
            {
                xpadport[i]=port;
                print("pad: Xbox controller %i connected as controller %i\n",i+1,port+1);
            }
        }
        else
        {
            if(xpadport[i]>=0)
            {
                print("pad: Xbox controller %i (controller %i) disconnected\n",i+1,xpadport[i]+1);
                memset(pad_port(xpadport[i]),0,sizeof(PadStructData));
                xpadport[i]=-1;
                xpadmotor[i]=0; // a pad plugged in later starts still
            }
        }
    }
}

// Adds the pad's buttons to p; the left stick replaces p's stick when it is
// out of its dead zone.
static void xpad_fill(PadStructData *p,const PadInState *g)
{
    int   b;
    float x,y,l,range;

    b=g->buttons;
    if(b&PADIN_A)                 p->button|=A_BUTTON;
    if(b&(PADIN_B|PADIN_X))       p->button|=B_BUTTON;
    if(b&PADIN_START)             p->button|=START;
    if(b&PADIN_DPAD_UP)           p->button|=U_JPAD;
    if(b&PADIN_DPAD_DOWN)         p->button|=D_JPAD;
    if(b&PADIN_DPAD_LEFT)         p->button|=L_JPAD;
    if(b&PADIN_DPAD_RIGHT)        p->button|=R_JPAD;
    if(b&PADIN_LSHOULDER)         p->button|=L_TRIG;
    if(b&PADIN_RSHOULDER)         p->button|=R_TRIG;
    if(g->rt>XPAD_TRIGGER)        p->button|=R_TRIG;
    if(g->lt>XPAD_TRIGGER)        p->button|=Z_TRIG;

    if(g->rx> XPAD_CSTICK) p->button|=R_CBUT;
    if(g->rx<-XPAD_CSTICK) p->button|=L_CBUT;
    if(g->ry> XPAD_CSTICK) p->button|=U_CBUT;
    if(g->ry<-XPAD_CSTICK) p->button|=D_CBUT;

    // left stick: round dead zone, remaining travel scaled to the N64's +-80
    x=g->lx;
    y=g->ly;
    l=sqrt(x*x+y*y);
    if(l>XPAD_DEADZONE)
    {
        range=32767.0f-XPAD_DEADZONE;
        if(l>32767.0f) l=32767.0f;
        l=(l-XPAD_DEADZONE)/range*80.0f/l;
        p->stickx=(char)(x*l);
        p->sticky=(char)(y*l);
    }
}

// Returns 1 and updates mypad when a controller is on port 1.
static int pad_xinput(void)
{
    int i=xpad_onport(0);
    if(i<0) return(0);
    pad_buttons();      // keyboard buttons still count
    pad_keyboard(1);    // keyboard stick, overridden below when the stick moves
    xpad_fill(&mypad,&xpadstate[i]);
    return(1);
}

// ports 2-4: only Xbox controllers
static void pad_xinputothers(void)
{
    int port,i;
    for(port=1;port<PAD_PORTS;port++)
    {
        PadStructData *p=pad_port(port);
        memset(p,0,sizeof(*p));
        i=xpad_onport(port);
        if(i>=0) xpad_fill(p,&xpadstate[i]);
    }
}

// Rumble Pak motor on port 'port' (pif.c), driven on the Xbox controller
// there. One motor on the N64; both Xbox motors, the heavy one stronger.

static void xpad_motor(int slot,int on)
{
    if(xpadmotor[slot]==on) return;
    xpadmotor[slot]=on;
    padin_rumble(slot,on?0xC000:0,on?0x6000:0);
}

void pad_rumble(int port,int on)
{
    int i=xpad_onport(port);
    if(i>=0) xpad_motor(i,on!=0);
}

// all motors off (emulation stopped: nothing would turn them off)
void pad_rumbleoff(void)
{
    int i;
    for(i=0;i<PADIN_SLOTS;i++) if(xpadport[i]>=0) xpad_motor(i,0);
}

// 1 when N64 controller port 'port' (0-3) has a controller in it. Port 1 is
// always there (keyboard); ports 2-4 while an Xbox controller is on them.
int pad_present(int port)
{
    if(port==0) return(1);
    if(port<0 || port>=PAD_PORTS) return(0);
    // the game asks at boot (osContInit) before any frame has sampled the
    // pads: look now, so ports 2-4 are there from the start as on the
    // hardware (games seen so far ask again later, so this fixed none)
    if(!xpadscanned) xpad_scan();
    return(xpad_onport(port)>=0);
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
    if(padin_key(PADIN_VK_CONTROL)) b|=8;
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

    if(mousedisablecnt==1 && padin_key(PADIN_VK_F6)) mouseactive=0;
    if(!padin_key(PADIN_VK_F6)) mousedisablecnt=1;

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

    xpad_scan();
    pad_xinputothers();

    if(mouseactive)
    {
        pad_buttons(); // buttons may be overridden by mouse/joy
        pad_mouse();
    }
    else if(pad_xinput())
    {
        // Xbox controller on port 1: pad_xinput() filled in mypad
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
    if(pad>=0 && pad<PAD_PORTS) state=*(dword *)pad_port(pad);
    else state=0;
    if(!pad) st.padstate=state;
    state=FLIP32(state);
    return(state);
}

// osContGetReadData: OSContPad[4] = button (2), stick_x, stick_y, errno;
// 6 bytes each. Ports without a controller get CONT_NO_RESPONSE_ERROR.
void pad_writedata(dword addr)
{
    int   i;
    dword state;
    for(i=0;i<PAD_PORTS;i++,addr+=6)
    {
        if(!pad_present(i))
        {
            mem_write8(addr+4,0x08);
            continue;
        }
        state=pad_getdata(i);
        mem_write16(addr,state>>16);
        mem_write8(addr+2,(state>>8)&0xff);
        mem_write8(addr+3,state&0xff);
        mem_write8(addr+4,0);
    }
}

