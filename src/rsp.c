#include "ultra.h"
#include "rsp_cxd4.h"

SPState sp;

// The interpreter's view of MI_INTR bit 0 (SP). It is loaded from the LLE
// MI before each run and compared after changes, so SET_INTR raises and
// CLR_INTR clears the CPU-visible interrupt; consuming the notification
// never clears a pending interrupt by itself.
static dword rsp_mi;
static dword rsp_milast;
// the RSP stopped while polling SP_STATUS or at the end of its slice: it is
// still running
static int   rsp_waiting;
// RSP instructions per rsp_run: a short first slice when the CPU starts or
// signals it, then rsp_runfor from lle_burststart in step with the CPU
// (2 RSP instructions per 3 CPU ones, 62.5 vs 93.75 MHz). A fixed 20,000
// per burst let a polling loop run out before the CPU could answer
// (n64-systemtest "RSP running in parallel to the CPU").
#define RSP_START 64
static int   rsp_budget=RSP_START;
// the task on the RSP is an audio one (rspaudio=1, MusyX): its time goes to
// the sound share of the GUI's performance display, as the HLE's does
int          rsp_audiotask;

static void rsp_checkinterrupts(void)
{
    dword now=rsp_mi&1;
    if(now==rsp_milast) return;
    rsp_milast=now;
    if(st.lleos) lle_rspinterrupt(now);
}

static void rsp_loadmi(void)
{
    rsp_mi=rsp_milast=(st.lleos && lle_mipending(MI_SP))?1:0;
}

// DP commands wait while DPC_STATUS.FREEZE is set: resume them once clear
static void rsp_dpcresume(void)
{
    if(!(RDP[3]&2) && (RDP[2]&0xfffff8)<(RDP[1]&0xfffff8)) rsp_dpclist();
}

static void rsp_message(const char *text)
{
    static int count;
    // libdragon's WRITE_STATUS command writes its whole command word to
    // SP_STATUS; the command id bits are ignored by the hardware
    if(!strncmp(text,"MTC0\nSP_STATUS",14)) return;
    if(count<20)
    {
        count++;
        print("rsp: cxd4 says \"%s\" (pc %03X)\n",text,RSP2[0]&0xfff);
    }
}

int rsp_init()
{
    RspCxd4Info info;
    dword a,seg;

    // Clear DMEM/IMEM

    memset(sp.dmem, 0, DMEM_SIZE);
    memset(sp.imem, 0, IMEM_SIZE);

    // Map DMEM/IMEM, readable and writable, in every segment (mem_init
    // already copied the old I/O page mappings there). They repeat every
    // 8K up to the SP registers at 0x04040000.

    for(seg=0;seg<5;seg++)
    {
        static const dword base[5]={0x00000000,0x80000000,0xa0000000,0xc0000000,0xe0000000};
        for(a=0x04000000;a<0x04040000;a+=0x2000)
        {
            mem_mapexternal(base[seg]+a,MAP_RW,sp.dmem);
            mem_mapexternal(base[seg]+a+0x1000,MAP_RW,sp.imem);
        }
    }

    // The interpreter works on the registers the CPU reads: SP registers in
    // the SP read page, SP_PC in SP2, DPC registers in the DP read page.
    // CPU stores go to the write pages and are applied by lle_hwwrite.
    rsp_mi=rsp_milast=0;
    rsp_waiting=0;
    softrdp_reset(); // a new game: no software-RDP state or framebuffer
    memset(&info,0,sizeof(info));
    info.rdram=mem.ram;
    info.rdramsize=mem.ramsize;
    info.dmem=sp.dmem;
    info.imem=sp.imem;
    info.mi_intr=&rsp_mi;
    info.spreg=RSP;
    info.sppc=RSP2;
    info.dpcreg=RDP;
    info.checkinterrupts=rsp_checkinterrupts;
    info.processrdplist=rsp_dpclist;
    info.message=rsp_message;
    rsp_cxd4_init(&info);

    return 0;
}

void rsp_dma(int toram)
{
    RSP[0]=WSP[0];
    RSP[1]=WSP[1];
    if(toram)
    {
        RSP[3]=WSP[3];
        rsp_cxd4_dmawrite();
    }
    else
    {
        RSP[2]=WSP[2];
        rsp_cxd4_dmaread();
    }
}

void rsp_run(void)
{
    static int first=1;
    if(first)
    {
        first=0;
        print("rsp: running a task on the RSP interpreter (pc %03X)\n",RSP2[0]&0xfff);
    }
    rsp_loadmi();
    // a slice at a time, so microcode that waits on the CPU (n64-systemtest
    // "RSP running in parallel to the CPU") doesn't hang the emulator; the
    // rest runs from lle_burststart while rsp_pending()
    rsp_cxd4_budget(rsp_budget);
    rsp_budget=RSP_START;
    {
        int t0=rsp_audiotask?timer_us(&st2.timer):0;
        prof_begin(PROF_RSP);
        rsp_waiting=rsp_cxd4_run();
        prof_end(PROF_RSP);
        if(rsp_audiotask) st.us_audio+=timer_us(&st2.timer)-t0;
    }
    RSP2[0]&=0xffc; // cxd4 keeps SP_PC as an IMEM address; the CPU reads the PC
    rsp_checkinterrupts();
    rsp_dpcresume(); // the RSP may have cleared DPC_STATUS.FREEZE
}

void rsp_runfor(int instructions)
{
    rsp_budget=instructions>0?instructions:1;
    rsp_run();
}

int rsp_pending(void)
{
    return(rsp_waiting);
}

// stopped at the end of its slice (not waiting on the CPU): continue soon
int rsp_sliced(void)
{
    return(rsp_waiting==2);
}

// HLE OS mode: a task type UltraHLE has no HLE for, run on the interpreter
// as osSpTaskLoad and osSpTaskStartGo set it up: the OSTask (16 words, its
// pointers made physical) at DMEM 0xFC0, the boot microcode at IMEM 0, SP_PC
// 0. The boot code loads the task's microcode itself. Pokemon Stadium's
// title and menu pictures are JPEGs decoded by a type 4 task.
void rsp_runtask(const void *task)
{
    dword w[16],boot;
    int   i,n;

    memcpy(w,task,sizeof(w));
    for(i=2;i<16;i+=2) if(w[i]) w[i]&=0x1fffffff; // the pointer fields
    for(i=0;i<16;i++) *(dword *)(sp.dmem+0xfc0+i*4)=w[i];

    boot=w[2];
    n=w[3];
    if(n<=0 || n>IMEM_SIZE) n=IMEM_SIZE;
    if(boot+n>(dword)mem.ramsize) return;
    for(i=0;i<n;i+=4) *(dword *)(sp.imem+i)=*(dword *)(mem.ram+boot+i);

    {
        static int told;
        if(!told++) print("rsp: task type %i runs on the RSP interpreter\n",w[0]);
    }
    RSP2[0]=0;                // SP_PC
    RSP[4]&=~0x003u;          // SP_STATUS: not halted, not broken
    rsp_cxd4_budget(0);       // to completion (the HLE caller waits for it)
    rsp_audiotask=(w[0]==2);
    {
        int t0=rsp_audiotask?timer_us(&st2.timer):0;
        for(i=0;i<1000;i++)   // it may stop to poll SP_STATUS: resume
            if(!rsp_cxd4_run()) break;
        if(rsp_audiotask) st.us_audio+=timer_us(&st2.timer)-t0;
    }
    RSP[4]|=0x001u;           // halted
}

// DP command list: DPC_CURRENT..DPC_END, in RDRAM or (DPC_STATUS bit 0,
// XBUS) in DMEM, drawn by the OpenGL renderer (rdp_rawcmd) or, with
// ultra.ini softrdp=1, by the software RDP (rdp_soft.c). FULL_SYNC raises
// the DP interrupt: the game's frame is complete. Nothing runs while
// DPC_STATUS.FREEZE is set; clearing it resumes (lle_hwwrite, rsp_run).
void rsp_dpclist(void)
{
    dword cur=RDP[2]&0xfffff8,end=RDP[1]&0xfffff8;
    int   xbus=RDP[3]&1;

    if(RDP[3]&2) return; // frozen: DPC_CURRENT stays, the list waits

    while(cur<end)
    {
        dword w[44];
        int   n,i;
        for(i=0;i<2;i++)
        {
            dword a=cur+i*4;
            w[i]=xbus?*(dword *)(sp.dmem+(a&0xffc)):*(dword *)(mem.ram+(a&(mem.ramsize-1)));
        }
        n=softrdp_cmdwords(w[0]);
        if(cur+n*4>end) break; // the rest of it isn't there yet
        for(i=2;i<n;i++)
        {
            dword a=cur+i*4;
            w[i]=xbus?*(dword *)(sp.dmem+(a&0xffc)):*(dword *)(mem.ram+(a&(mem.ramsize-1)));
        }
        prof_begin(PROF_RDP);
        if(inifile_softrdp()) softrdp_cmd(w,n);
        else                  rdp_rawcmd(w,n);
        prof_end(PROF_RDP);
        if(((w[0]>>24)&0x3f)==0x29)
        { // FULL_SYNC: the frame is done (also reads the pad, counts fps);
          // the pipe goes idle (DPC_STATUS PIPE_BUSY, START_GCLK)
            RDP[3]&=~0x28u;
            sync_gfxframedone();
            os_event(OS_EVENT_DP);
        }
        cur+=n*4;
    }
    RDP[2]=cur;
    RDP[3]&=~0x400u; // START_VALID: the new buffer was taken
}
