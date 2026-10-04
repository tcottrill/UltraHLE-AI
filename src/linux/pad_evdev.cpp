// padin.h on Linux: gamepads through AAE's evdev joystick code (aae/
// evdev_joystick.cpp: hotplug rescan, rumble), keys from the window's X11
// key events (ui_linux.cpp calls pad_linux_key). evdev is read on the
// emulation thread while a game runs (pad.c samples the controllers) and on
// the main thread by the ROM picker (pad_linux_menupoll); a lock keeps the
// two apart.

extern "C" {
#include "../ultra.h"
#include "../padin.h"
}
#include "aae/joystick.h"
#include "ui_linux.h"
#include <X11/keysym.h>
#include <mutex>

bool evdev_pad_raw(int slot, uint16_t* buttons, int* lx, int* ly, int* rx, int* ry,
                   int* lt, int* rt);

/****************************************************************************
** keyboard: padin.h key numbers (Windows virtual keys) set from X11 keysyms
*/

static volatile unsigned char keydown[256];

static int keysym_to_vk(unsigned long ks)
{
    if(ks>='a' && ks<='z') return (int)(ks-'a'+'A');
    if(ks>='A' && ks<='Z') return (int)ks;
    switch(ks)
    {
    case XK_Shift_L: case XK_Shift_R:     return PADIN_VK_SHIFT;
    case XK_Control_L: case XK_Control_R: return PADIN_VK_CONTROL;
    case XK_Prior: return PADIN_VK_PRIOR;
    case XK_Next:  return PADIN_VK_NEXT;
    case XK_End:   return PADIN_VK_END;
    case XK_Home:  return PADIN_VK_HOME;
    case XK_Left:  return PADIN_VK_LEFT;
    case XK_Up:    return PADIN_VK_UP;
    case XK_Right: return PADIN_VK_RIGHT;
    case XK_Down:  return PADIN_VK_DOWN;
    case XK_F6:    return PADIN_VK_F6;
    }
    return 0;
}

// window thread: a key went down or up
void pad_linux_key(unsigned long keysym, bool down)
{
    int vk=keysym_to_vk(keysym);
    if(vk) keydown[vk]=down?1:0;
}

// window thread: focus lost, no key may stay down
void pad_linux_releaseall(void)
{
    for(int i=0;i<256;i++) keydown[i]=0;
}

int padin_key(int vk)
{
    return (vk>0 && vk<256) ? keydown[vk] : 0;
}

void padin_joy(int *x,int *y,int *buttons)
{
    *x=*y=32768; // no WinMM joystick on Linux; gamepads come through padin_pad
    *buttons=0;
}

/****************************************************************************
** gamepads: evdev slots 0-3 (stable per device, see evdev_joystick.cpp)
*/

static volatile int changed;
static std::mutex  evdevlock;

// read events, rescan for new pads now and then (evdevlock held)
static void pollpads(void)
{
    if(!_joystick_installed) install_joystick();
    if(changed) { changed=0; joystick_device_change(); }
    poll_joystick();
}

// SteamOS game mode has no keyboard: Back plus a button does what the
// function keys do (ui_linux.cpp). Edge triggered, any pad, held for two
// samples (AAE's joystick_check_combo).
static void shortcuts(void)
{
    static const struct { uint16_t mask; int act; } combo[]=
    {
        { AAE_JOYBTN_BACK|AAE_JOYBTN_START,          ACT_QUIT },
        { AAE_JOYBTN_BACK|AAE_JOYBTN_RIGHT_SHOULDER, ACT_SAVE },
        { AAE_JOYBTN_BACK|AAE_JOYBTN_LEFT_SHOULDER,  ACT_LOAD },
        { AAE_JOYBTN_BACK|AAE_JOYBTN_Y,              ACT_SHOT },
        { AAE_JOYBTN_BACK|AAE_JOYBTN_X,              ACT_FULL },
    };
    for(const auto &c:combo)
    {
        if(joystick_check_combo(0,c.mask)) ui_linux_action(c.act);
    }
}

void pad_devicechange(void)
{
    changed=1;
}

int padin_pad(int slot,PadInState *s)
{
    uint16_t b;
    int lx,ly,rx,ry,lt,rt;

    std::lock_guard<std::mutex> lock(evdevlock);
    memset(s,0,sizeof(*s));
    if(slot==0)
    {
        pollpads(); // once per sample
        shortcuts();
    }
    static bool pressed[MAX_JOYSTICKS]; // first button logged since connecting
    if(!evdev_pad_raw(slot,&b,&lx,&ly,&rx,&ry,&lt,&rt))
    {
        if(slot>=0 && slot<MAX_JOYSTICKS) pressed[slot]=false;
        return 0;
    }
    if(b && slot>=0 && slot<MAX_JOYSTICKS && !pressed[slot])
    {
        // shows which pad reaches the game (a silent slot = a pad that
        // sends nothing, ultra.log)
        pressed[slot]=true;
        print("pad: Xbox controller %i (%s) first button press %04X\n",slot+1,
              joystick_get_display_name(slot),b);
    }

    // AAE_JOYBTN_* are XInput's bits, as are PADIN_*
    s->buttons=b;
    // -128..127 with up negative -> XInput's -32768..32767 with up positive
    s->lx=(short)(lx*258);
    s->ly=(short)(-ly*258);
    s->rx=(short)(rx*258);
    s->ry=(short)(-ry*258);
    s->lt=(unsigned char)lt;
    s->rt=(unsigned char)rt;
    return 1;
}

void padin_rumble(int slot,int heavy,int light)
{
    std::lock_guard<std::mutex> lock(evdevlock);
    joystick_set_rumble(slot,heavy/65535.0f,light/65535.0f);
}

// ROM picker (main thread): the buttons of every pad together (AAE_JOYBTN_*)
// and the furthest left stick deflection, -128..127 (up negative)
uint16_t pad_linux_menupoll(int *lx,int *ly)
{
    std::lock_guard<std::mutex> lock(evdevlock);
    uint16_t all=0,b;
    int x,y,rx,ry,lt,rt;
    pollpads();
    *lx=*ly=0;
    for(int slot=0;slot<MAX_JOYSTICKS;slot++)
    {
        if(!evdev_pad_raw(slot,&b,&x,&y,&rx,&ry,&lt,&rt)) continue;
        all|=b;
        if(abs(x)>abs(*lx)) *lx=x;
        if(abs(y)>abs(*ly)) *ly=y;
    }
    return all;
}

static_assert(AAE_JOYBTN_A==PADIN_A && AAE_JOYBTN_B==PADIN_B && AAE_JOYBTN_X==PADIN_X &&
              AAE_JOYBTN_Y==PADIN_Y && AAE_JOYBTN_START==PADIN_START &&
              AAE_JOYBTN_DPAD_UP==PADIN_DPAD_UP && AAE_JOYBTN_DPAD_DOWN==PADIN_DPAD_DOWN &&
              AAE_JOYBTN_DPAD_LEFT==PADIN_DPAD_LEFT && AAE_JOYBTN_DPAD_RIGHT==PADIN_DPAD_RIGHT &&
              AAE_JOYBTN_LEFT_SHOULDER==PADIN_LSHOULDER && AAE_JOYBTN_RIGHT_SHOULDER==PADIN_RSHOULDER,
              "AAE_JOYBTN_* and PADIN_* are both XInput's bits");
