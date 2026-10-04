// What the emulator core asks of the front end (the program around it):
// ui_win32.c is the Windows GUI's side. A Linux front end provides the same.
// The text console and keys are console.h, sound output sound.h, and the
// OpenGL window xgl/xgl_internal.h (xgl_ctx_*).

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// the program's command line as one string, the program name first
// (quoted when it has spaces), as Windows' GetCommandLine
const char *ui_commandline(void);

// the program's own file name with its full path
void ui_exepath(char *out,int size);

// status bar field 1..6 (frame or cycle count, cpu, gfx, sound, idle, fps)
void ui_statustext(int field,const char *text);

// a line for the log list (errors, warnings, notes); NULL clears it
void ui_debugline(const char *text);

// a message the user must see (usage help, fatal errors)
void ui_message(const char *title,const char *text);

// main_thread runs on the emulation thread: it tells the front end so
void ui_setemuthread(void);

// raises (1) or restores (0) the emulation thread's priority while the
// window thread waits for it to stop
void ui_emupriority(int high);

// waits ms on the window thread, still serving what the emulation thread
// needs from it meanwhile (on Windows its SendMessage calls)
void ui_wait(int ms);

#ifdef __cplusplus
};
#endif
