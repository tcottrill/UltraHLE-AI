// PIF joybus commands (controllers and the Controller Pak), shared by HLE
// (hw.c) and LLE OS mode (lle.c).

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// load (or create and format) the Controller Pak image for the loaded rom:
// <savepath or exe folder\saves>\<rom title>.mpk. Called on every rom boot.
void pif_reset(void);

// save file for the loaded rom: <savepath or exe folder\saves>\<rom title>.<ext>
// (dst holds at least MAXFILE+48 characters)
void pif_savename(char *dst,const char *ext);

// run the joybus commands the game wrote to PIF RAM (RPIF[0x1f0..0x1ff]),
// replies in place. The pad and its Controller Pak are on channel padport.
void pif_process(int padport);

// the game wrote PIF RAM: honour the control byte (0x3F), which is either
// joybus commands or a CIC-6105 challenge answered in place
void pif_write(int padport);

// the game reads PIF RAM back: current replies (none after a challenge)
void pif_read(int padport);

#ifdef __cplusplus
}
#endif
