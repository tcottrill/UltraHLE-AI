// padin.h on Windows: GetAsyncKeyState, joyGetPos and XInput.

#include <windows.h>
#include <xinput.h>
#include "ultra.h"
#include "padin.h"

// padin.h's numbers are Windows' own
#define SAME(a,b) typedef char check_##a[((a)==(b))?1:-1]
SAME(PADIN_VK_SHIFT,VK_SHIFT);   SAME(PADIN_VK_CONTROL,VK_CONTROL);
SAME(PADIN_VK_PRIOR,VK_PRIOR);   SAME(PADIN_VK_NEXT,VK_NEXT);
SAME(PADIN_VK_END,VK_END);       SAME(PADIN_VK_HOME,VK_HOME);
SAME(PADIN_VK_LEFT,VK_LEFT);     SAME(PADIN_VK_UP,VK_UP);
SAME(PADIN_VK_RIGHT,VK_RIGHT);   SAME(PADIN_VK_DOWN,VK_DOWN);
SAME(PADIN_VK_F6,VK_F6);
SAME(PADIN_DPAD_UP,XINPUT_GAMEPAD_DPAD_UP);       SAME(PADIN_DPAD_DOWN,XINPUT_GAMEPAD_DPAD_DOWN);
SAME(PADIN_DPAD_LEFT,XINPUT_GAMEPAD_DPAD_LEFT);   SAME(PADIN_DPAD_RIGHT,XINPUT_GAMEPAD_DPAD_RIGHT);
SAME(PADIN_START,XINPUT_GAMEPAD_START);           SAME(PADIN_BACK,XINPUT_GAMEPAD_BACK);
SAME(PADIN_LTHUMB,XINPUT_GAMEPAD_LEFT_THUMB);     SAME(PADIN_RTHUMB,XINPUT_GAMEPAD_RIGHT_THUMB);
SAME(PADIN_LSHOULDER,XINPUT_GAMEPAD_LEFT_SHOULDER); SAME(PADIN_RSHOULDER,XINPUT_GAMEPAD_RIGHT_SHOULDER);
SAME(PADIN_A,XINPUT_GAMEPAD_A); SAME(PADIN_B,XINPUT_GAMEPAD_B);
SAME(PADIN_X,XINPUT_GAMEPAD_X); SAME(PADIN_Y,XINPUT_GAMEPAD_Y);
SAME(PADIN_SLOTS,XUSER_MAX_COUNT);

// GetAsyncKeyState, but only while one of our windows has focus (keys typed
// into other applications must not drive the game)
int padin_key(int vk)
{
    DWORD pid=0;
    GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    if(pid!=GetCurrentProcessId()) return(0);
    return(GetAsyncKeyState(vk)<0);
}

void padin_joy(int *x,int *y,int *buttons)
{
    JOYINFO joy;
    memset(&joy,0,sizeof(joy));
    joyGetPos(JOYSTICKID1,&joy);
    *x=joy.wXpos;
    *y=joy.wYpos;
    *buttons=joy.wButtons;
}

/****************************************************************************
** XInput. Polling an empty slot is slow, so an empty slot is retried only
** every XPAD_RETRY samples, or at once after a WM_DEVICECHANGE
** (pad_devicechange, called on the window thread).
*/

#define XPAD_RETRY 60

static int           connected[XUSER_MAX_COUNT];
static int           retry[XUSER_MAX_COUNT];
static volatile LONG changed;

void pad_devicechange(void)
{
    InterlockedExchange(&changed,1);
}

int padin_pad(int slot,PadInState *s)
{
    XINPUT_STATE x;
    int i;

    if(slot==0 && InterlockedExchange(&changed,0))
    {
        for(i=0;i<XUSER_MAX_COUNT;i++) retry[i]=0;
    }
    memset(s,0,sizeof(*s));
    if(!connected[slot] && retry[slot]>0)
    {
        retry[slot]--;
        return(0);
    }
    memset(&x,0,sizeof(x));
    if(XInputGetState(slot,&x)!=ERROR_SUCCESS)
    {
        connected[slot]=0;
        retry[slot]=XPAD_RETRY;
        return(0);
    }
    connected[slot]=1;
    s->buttons=x.Gamepad.wButtons;
    s->lx=x.Gamepad.sThumbLX;
    s->ly=x.Gamepad.sThumbLY;
    s->rx=x.Gamepad.sThumbRX;
    s->ry=x.Gamepad.sThumbRY;
    s->lt=x.Gamepad.bLeftTrigger;
    s->rt=x.Gamepad.bRightTrigger;
    return(1);
}

void padin_rumble(int slot,int heavy,int light)
{
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed =(WORD)heavy;
    v.wRightMotorSpeed=(WORD)light;
    XInputSetState(slot,&v);
}
