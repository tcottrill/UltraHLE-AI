// Software RDP: raw RDP command lists (DPC_START..DPC_END) drawn into the
// RDRAM color image. For LLE games whose own RSP microcode talks to the RDP
// directly (libdragon's rdpq), which UltraHLE's HLE renderer can't follow.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// run one command: w = its 32-bit words (2 for most, up to 44 for a
// triangle with shade, texture and z)
void softrdp_cmd(const dword *w,int words);

// words in the command whose first word is w0
int  softrdp_cmdwords(dword w0);

// 1 while softrdp is selected and a command list was drawn since the last
// reset: the VI framebuffer is the picture
int  softrdp_active(void);

// forget all state (TMEM, modes, the active latch): at boot, and when the
// renderer is switched
void softrdp_reset(void);

#ifdef __cplusplus
}
#endif
