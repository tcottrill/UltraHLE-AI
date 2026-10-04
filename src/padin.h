// Host input for pad.c: keyboard keys, a WinMM style joystick and up to
// PADIN_SLOTS gamepads in the Xbox layout. pad_win32.c reads Windows
// (GetAsyncKeyState, joyGetPos, XInput); the Linux build reads evdev.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// key codes for padin_key: Windows virtual key numbers ('A'-'Z' are the
// letters themselves); a Linux backend maps its keys onto these
#define PADIN_VK_SHIFT   0x10
#define PADIN_VK_CONTROL 0x11
#define PADIN_VK_PRIOR   0x21 // Page Up
#define PADIN_VK_NEXT    0x22 // Page Down
#define PADIN_VK_END     0x23
#define PADIN_VK_HOME    0x24
#define PADIN_VK_LEFT    0x25
#define PADIN_VK_UP      0x26
#define PADIN_VK_RIGHT   0x27
#define PADIN_VK_DOWN    0x28
#define PADIN_VK_F6      0x75

// 1 while the key is down and one of the program's windows has the focus
int  padin_key(int vk);

// joystick 1 (WinMM): axes 0..65535 with 32768 the centre, buttons 1 bit each
void padin_joy(int *x,int *y,int *buttons);

// gamepad buttons: XInput's bit values (pad_win32.c checks they agree)
#define PADIN_DPAD_UP    0x0001
#define PADIN_DPAD_DOWN  0x0002
#define PADIN_DPAD_LEFT  0x0004
#define PADIN_DPAD_RIGHT 0x0008
#define PADIN_START      0x0010
#define PADIN_BACK       0x0020
#define PADIN_LTHUMB     0x0040
#define PADIN_RTHUMB     0x0080
#define PADIN_LSHOULDER  0x0100
#define PADIN_RSHOULDER  0x0200
#define PADIN_A          0x1000
#define PADIN_B          0x2000
#define PADIN_X          0x4000
#define PADIN_Y          0x8000

#define PADIN_SLOTS 4

typedef struct
{
    unsigned short buttons;  // PADIN_*
    short          lx,ly;    // left stick, -32768..32767, up is positive
    short          rx,ry;    // right stick
    unsigned char  lt,rt;    // triggers 0..255
} PadInState;

// slot 0..PADIN_SLOTS-1: 1 and its state when a gamepad is connected there.
// Called once per pad sample for each slot; a backend may skip polling
// empty slots for a while (XInput's are slow), pad_devicechange ends that.
int  padin_pad(int slot,PadInState *s);

// rumble motors 0..65535 (heavy low frequency, light high frequency)
void padin_rumble(int slot,int heavy,int light);

#ifdef __cplusplus
};
#endif
