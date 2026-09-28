// Hardware and Memory mapped io emulation (devices)

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void    hw_init(void);

void    hw_check(void);
// these in cpu.c temporarily
void    hw_retrace(void);
void    hw_gfxframedone(void);

void    hw_rspcheck(void);

void    hw_memio(void);
int     hw_ismemiorange(dword addr);

void    hw_gfxthread(void);

void    hw_selectpad(int pad);

// SP register handlers, also used by LLE OS mode (lle.c)
void    hw_sp_statuswrite(void);
void    hw_sp_dmawrite(void);
void    hw_sp_taskdone(void);

#ifdef __cplusplus
};
#endif
