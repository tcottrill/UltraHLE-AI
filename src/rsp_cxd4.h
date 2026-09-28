// Interface to the cxd4 RSP interpreter (rsp_cxd4.c, sources in rsp_cxd4/).
// Plain types only: rsp_cxd4.c can't include ultra.h (cxd4's register enum
// has `sp`) and ultra.h code doesn't see cxd4's headers.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    unsigned char *rdram;       // mem.ram, host-order 32-bit words
    unsigned long  rdramsize;
    unsigned char *dmem;
    unsigned char *imem;
    unsigned int  *mi_intr;     // bit 0 set by the RSP: SP interrupt
    unsigned int  *spreg;       // SP_MEM_ADDR .. SP_SEMAPHORE (8 registers)
    unsigned int  *sppc;        // SP_PC
    unsigned int  *dpcreg;      // DPC_START .. DPC_TMEM (8 registers)
    void (*checkinterrupts)(void);
    void (*processrdplist)(void); // the RSP wrote DPC_END
    void (*message)(const char *text); // cxd4 warnings
} RspCxd4Info;

void rsp_cxd4_init(const RspCxd4Info *info);
// run from SP_PC until BREAK or halt; returns 1 if it stopped only because
// it kept polling SP_STATUS (halt left clear: run it again later)
int  rsp_cxd4_run(void);
// instructions per rsp_cxd4_run before it returns 2 with the RSP still
// running (0: run until it halts)
void rsp_cxd4_budget(int instructions);
// SP DMA with the length already in spreg[2] (read) or spreg[3] (write)
void rsp_cxd4_dmaread(void);
void rsp_cxd4_dmawrite(void);

#ifdef __cplusplus
}
#endif
