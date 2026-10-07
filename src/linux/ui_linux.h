// Actions the Linux front end's main loop carries out (ui_linux.cpp), from
// keys or from controller shortcuts (pad_evdev.cpp), and the ROM picker
// (picker.cpp).
#pragma once

#include <stdint.h>

enum { ACT_NONE, ACT_QUIT, ACT_START, ACT_PAUSE, ACT_SAVE, ACT_LOAD, ACT_SHOT, ACT_FULL };

// any thread: run act on the main loop's next pass
void ui_linux_action(int act);

// ROM picker input, one bit per press
enum
{
    PK_UP=1<<0, PK_DOWN=1<<1, PK_PGUP=1<<2, PK_PGDN=1<<3, PK_HOME=1<<4, PK_END=1<<5,
    PK_OK=1<<6, PK_BACK=1<<7, PK_QUIT=1<<8, PK_RESCAN=1<<9
};

// what picker_frame asks for
enum { PICK_NONE, PICK_LAUNCH, PICK_QUIT, PICK_RESUME };

// main thread, emulation stopped
void picker_scan(const char *dir);
void picker_message(const char *msg);   // a line under the list ("" clears)
void picker_shutdown();                // main thread, before window destruction
int  picker_frame(int input,int canresume,char *path,int pathsize); // draws a frame
extern volatile int picker_capture;     // 1: save the next frame to picker.ppm

// the pads' buttons together (AAE_JOYBTN_*) and left stick, -128..127
uint16_t pad_linux_menupoll(int *lx,int *ly);
