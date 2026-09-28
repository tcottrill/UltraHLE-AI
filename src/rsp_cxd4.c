// The cxd4 RSP interpreter (rsp_cxd4/, mupen64plus-rsp-cxd4, CC0; commit in
// rsp_cxd4/UPSTREAM) built as one translation unit, the way its lto.c does.
// Its sources are unmodified except as listed in UPSTREAM (the semaphore
// read). The Zilmar plugin glue (module.c, which also
// shows messages in CMD windows) is replaced by the functions below.

#define ARCH_MIN_SSE2 // x64: SSE2 always present

#include "rsp_cxd4/su.c"

#include "rsp_cxd4/vu/vu.c"
#include "rsp_cxd4/vu/multiply.c"
#include "rsp_cxd4/vu/add.c"
#include "rsp_cxd4/vu/select.c"
#include "rsp_cxd4/vu/logical.c"
#include "rsp_cxd4/vu/divide.c"

#include "rsp_cxd4.h"

RSP_INFO RSP_INFO_NAME;
p_func   GBI_phase;

static void (*cxd4_message)(const char *text);

NOINLINE void message(const char *body)
{
    char text[128];
    if(!cxd4_message) return;
    snprintf(text,sizeof(text),"%s (opcode %08X)",body,(unsigned)inst_word);
    cxd4_message(text);
}

void rsp_cxd4_init(const RspCxd4Info *info)
{
    int i;

    memset(&RSP_INFO_NAME,0,sizeof(RSP_INFO_NAME));
    RSP_INFO_NAME.RDRAM=info->rdram;
    RSP_INFO_NAME.DMEM=info->dmem;
    RSP_INFO_NAME.IMEM=info->imem;
    RSP_INFO_NAME.MI_INTR_REG     =(pu32)info->mi_intr;
    RSP_INFO_NAME.SP_MEM_ADDR_REG =(pu32)&info->spreg[0];
    RSP_INFO_NAME.SP_DRAM_ADDR_REG=(pu32)&info->spreg[1];
    RSP_INFO_NAME.SP_RD_LEN_REG   =(pu32)&info->spreg[2];
    RSP_INFO_NAME.SP_WR_LEN_REG   =(pu32)&info->spreg[3];
    RSP_INFO_NAME.SP_STATUS_REG   =(pu32)&info->spreg[4];
    RSP_INFO_NAME.SP_DMA_FULL_REG =(pu32)&info->spreg[5];
    RSP_INFO_NAME.SP_DMA_BUSY_REG =(pu32)&info->spreg[6];
    RSP_INFO_NAME.SP_SEMAPHORE_REG=(pu32)&info->spreg[7];
    RSP_INFO_NAME.SP_PC_REG       =(pu32)info->sppc;
    RSP_INFO_NAME.DPC_START_REG   =(pu32)&info->dpcreg[0];
    RSP_INFO_NAME.DPC_END_REG     =(pu32)&info->dpcreg[1];
    RSP_INFO_NAME.DPC_CURRENT_REG =(pu32)&info->dpcreg[2];
    RSP_INFO_NAME.DPC_STATUS_REG  =(pu32)&info->dpcreg[3];
    RSP_INFO_NAME.DPC_CLOCK_REG   =(pu32)&info->dpcreg[4];
    RSP_INFO_NAME.DPC_BUFBUSY_REG =(pu32)&info->dpcreg[5];
    RSP_INFO_NAME.DPC_PIPEBUSY_REG=(pu32)&info->dpcreg[6];
    RSP_INFO_NAME.DPC_TMEM_REG    =(pu32)&info->dpcreg[7];
    RSP_INFO_NAME.CheckInterrupts=info->checkinterrupts;
    RSP_INFO_NAME.ProcessRdpList =info->processrdplist;
    cxd4_message=info->message;

    DRAM=info->rdram;
    DMEM=info->dmem;
    IMEM=info->imem;
    for(i=0;i<8;i++)
    {
        CR[i]  =(pu32)&info->spreg[i];
        CR[8+i]=(pu32)&info->dpcreg[i];
    }
    GBI_phase=info->processrdplist;
    su_max_address=info->rdramsize-1;
    MF_SP_STATUS_TIMEOUT=32767;
    memset(conf,0,32); // no HLE hand-off, no semaphore hack

    // registers as at power-on, without module.c's random values
    memset(SR,0,sizeof(SR));
    memset(VR,0,sizeof(VR));
    for(i=0;i<N;i++)
    {
        VACC_H[i]=VACC_M[i]=VACC_L[i]=0;
        cf_ne[i]=cf_co[i]=cf_clip[i]=cf_comp[i]=cf_vce[i]=0;
    }
}

void rsp_cxd4_budget(int instructions)
{
    cxd4_budget=instructions;
}

int rsp_cxd4_run(void)
{
    int i,polled;

    for(;;)
    {
        for(i=0;i<NUMBER_OF_SCALAR_REGISTERS;i++) MFC0_count[i]=0;
        run_task();
        if(cxd4_budgetout) return(2); // slice used up: still running
        // halted by a BREAK in this run (BROKE alone may be left over from
        // an earlier one: the RSP runs again as soon as HALT is cleared)
        if((inst_word>>26)==0 && (inst_word&0x3f)==0x0d) return(0);

        // MTC0 SP_STATUS with SET_INTR also sets halt in cxd4 (so the plugin
        // host sees the interrupt); the RSP itself carries on
        if((inst_word>>26)==020 && ((inst_word>>21)&31)==4 &&
           ((inst_word>>11)&31)==4)
        {
            u32 w=SR[(inst_word>>16)&31];
            if((w&0x10) && !(w&0x02))
            {
                *CR[0x4]&=~SP_STATUS_HALT;
                if(RSP_INFO_NAME.CheckInterrupts) RSP_INFO_NAME.CheckInterrupts();
                continue;
            }
        }

        // module.c DoRspCycles: stopping after the SP_STATUS poll limit
        // means the RSP is waiting on the CPU; it hasn't really halted
        polled=0;
        for(i=0;i<NUMBER_OF_SCALAR_REGISTERS;i++)
            if(MFC0_count[i]>=MF_SP_STATUS_TIMEOUT) polled=1;
        if(polled)
        {
            *CR[0x4]&=~SP_STATUS_HALT;
            return(1);
        }
        return(0); // halted by MTC0 SP_STATUS SET_HALT
    }
}

void rsp_cxd4_dmaread(void)
{
    *CR[0x2]|=7;
    SP_DMA_READ();
}

void rsp_cxd4_dmawrite(void)
{
    *CR[0x3]|=7;
    SP_DMA_WRITE();
}
