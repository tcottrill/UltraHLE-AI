// Pad emulation

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void  pad_frame(void);           // call this every frame (pad centering and stuff)
dword pad_getdata(int pad);
void  pad_writedata(dword addr); // write pad state to a memory location
void  pad_drawframe(void);

void  pad_enablejoy(int enable);

#define PAD_PORTS 4              // N64 controller ports
int   pad_present(int port);     // port 0-3 has a controller (port 0 always: keyboard)
void  pad_devicechange(void);    // WM_DEVICECHANGE: poll the empty Xbox controller slots now
void  pad_rumble(int port,int on); // Rumble Pak motor on port 0-3 -> Xbox controller there
void  pad_rumbleoff(void);       // all motors off (emulation stopped)

// A temporary solution for now. The controller emulation code is for some reason heavily tied to console.c, we need to untangle this doshirak.

extern int pad_automask,pad_autoperiod; // debugger "autopad"
extern int pad_autostickx,pad_autosticky;
extern int joyactive;
extern int mouseactive;
extern int mousedisablecnt;

#ifdef __cplusplus
};
#endif
