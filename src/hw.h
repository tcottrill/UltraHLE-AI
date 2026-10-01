// Hardware and Memory mapped io emulation (devices)

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void    hw_init(void);

void    hw_check(void);
void    hw_si_later(dword m_queue); // HLE: a controller message after the SI delay
void    hw_si_flush(void);          // deliver the pending ones now
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
void    hw_dp_statuswrite(dword value);
void    hw_save(FILE *f1);   // save state block (after lle_save)
void    hw_load(FILE *f1);   // missing in older states: nothing pending

#ifdef __cplusplus
};
#endif
