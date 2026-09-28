#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void sync_init(void);
void sync_thread(void);
void sync_gfxframedone(void);
void sync_retrace(void);
void sync_checkretrace(void);

extern int sync_swappending; // set by osViSwapBuffer until a retrace shows it

#ifdef __cplusplus
};
#endif
