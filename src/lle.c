// LLE OS mode: emulated CPU exceptions and hardware interrupts, so games whose
// libultra version UltraHLE doesn't recognise can run their own OS instead of
// UltraHLE's replacement functions. See lle.h and
// docs/superpowers/specs/2026-09-24-lle-os-mode-design.md.

#include "ultra.h"

int lle_jumped;
static int lle_trace=0; // debugging aid: number of register writes to log

typedef struct
{
    dword hi,lo0,lo1,mask;
} LleTlb;

static struct
{
    dword  mi_intr;          // pending MI interrupts
    dword  mi_mask;          // MI_INTR_MASK
    qword  pending[6];       // cputime at which MI bit n gets raised (0 = none)
    qword  countbase;        // Count = countbase + (now-countref)*3/4
    qword  countref;
    dword  lastcount;        // Count at the previous burst (Compare check)
    qword  dpclockref;       // cputime at the last DPC_CLOCK clear
    int    burst;            // length of the current burst
    LleTlb tlb[32];
    int    excseen[32];      // first exception of each code logged
    int    hwseen[64];       // first unknown register write per block logged
    int    irqs[7];          // interrupts raised since last report (6 = timer)
    int    vis;              // VI interrupts since last report
    int    excs;             // exceptions taken since last report
    int    reports;          // once-a-second reports printed
} lle;

static const char *mi_names[6]={"SP","SI","AI","VI","PI","DP"};

// AI DMA FIFO (Mupen64Plus ai_controller): two buffers, the first playing.
// AI_STATUS shows BUSY/FULL and AI_LEN the bytes left in the playing one;
// the AI interrupt comes when a buffer ends. Games that mix on the CPU fill
// until FULL (Bassmasters 2000 spun forever while AI_STATUS read 0). Time is
// retraces, which the sync code paces to the host clock. Not in save states.
static struct
{
    int   cnt;     // buffers queued, 0..2
    dword len[2];  // bytes; len[0] is what's left of the playing buffer
    dword addr[2]; // their RDRAM addresses (KSEG0)
    int   frac;    // bytes per retrace remainder (x60 or x50)
    qword endtime; // countperop games: cputime at which len[0] has played
} ai;

static void lle_raise(dword bit,int delay);
static void lle_sireaddone(void);
static qword lle_now(void);

// countperop games: the CPU runs in step with real time, so a buffer ends
// after its playing time in instructions, not at a retrace. TWINE queues
// 448-byte buffers and refills on each AI interrupt; one per retrace fed a
// third of the audio and the sound stuck.
static int ai_timed(void)
{
    return(cart.countperop>0);
}

static qword ai_duration(dword len)
{
    dword rate=WAI[4]?48681812/(WAI[4]+1):0;
    if(!rate) return(1);
    return((qword)(len/4)*46875000/rate/cart.countperop+1);
}
static int  dp_heldint; // DP interrupt held while DPC_STATUS.FREEZE is set
static void tlb_remap(void);

static void ai_regs(void)
{
    RAI[1]=ai.cnt?ai.len[0]:0;
    RAI[3]=(ai.cnt==2?0x80000001u:0)|(ai.cnt?0x40000000u:0);
}

// A buffer is read when it starts playing, as the AI DMA does, not when it
// is queued: Paper Mario queues a buffer before its audio task has mixed
// into it, and reading it at AI_LEN played the last round's samples.
static void ai_push(dword addr,dword len)
{
    if(!len) return;
    if(ai.cnt<2) ai.cnt++;
    ai.addr[ai.cnt-1]=addr;
    ai.len[ai.cnt-1]=len;
    if(ai.cnt==1)
    {
        slist_aiplay(addr,len);
        if(ai_timed()) ai.endtime=lle_now()+ai_duration(len);
    }
    ai_regs();
}

// the playing buffer ended: the next one, if queued, starts
static void ai_next(void)
{
    ai.len[0]=ai.len[1];
    ai.addr[0]=ai.addr[1];
    ai.cnt--;
    if(ai.cnt) slist_aiplay(ai.addr[0],ai.len[0]);
}

// countperop games, per burst: finished buffers leave the FIFO with an AI
// interrupt, and AI_LEN shows what is left of the playing one
static void ai_update(void)
{
    qword now=lle_now();
    if(!ai_timed()) return;
    while(ai.cnt && now>=ai.endtime)
    {
        ai_next();
        if(ai.cnt) ai.endtime+=ai_duration(ai.len[0]);
        lle_raise(MI_AI,0);
    }
    ai_regs();
    if(ai.cnt)
    {
        qword d=ai_duration(ai.len[0]),left=ai.endtime-now;
        if(left<d) RAI[1]=(dword)((qword)ai.len[0]*left/d)&~7u;
    }
}

// one retrace of playback at the DAC rate
static void ai_retrace(void)
{
    int   hz=st.timing?50:60;
    dword rate=WAI[4]?48681812/(WAI[4]+1):0;
    dword bytes;
    if(ai_timed() || !ai.cnt || !rate) return;
    ai.frac+=rate*4;
    bytes=ai.frac/hz;
    ai.frac%=hz;
    while(ai.cnt && bytes)
    {
        if(bytes<ai.len[0])
        {
            ai.len[0]-=bytes;
            break;
        }
        bytes-=ai.len[0];
        ai_next();
        lle_raise(MI_AI,0);
    }
    if(!ai.cnt) ai.frac=0;
    ai_regs();
}

/****************************************************************************
** time and Count
*/

static qword lle_now(void)
{
    // st.cputime advances per burst; add what ran of the current one
    qword n=st.cputime;
    if(st.bailout>0 && st.bailout<=lle.burst) n+=lle.burst-st.bailout;
    return(n);
}

// Count ticks per VI retrace (46.875 MHz)
static dword lle_countpervi(void)
{
    return(st.timing?46875000/50:46875000/60);
}

// Count follows the instructions run, but only up to the next retrace: a
// retrace (paced by the host clock) moves it to the next frame's start. So
// Count keeps real time on any host, as libdragon's get_ticks timing needs
// (Flappy Bird scrolled in slow motion); countbase is the frame's start.
// Count ticks for n instructions: 3/4, or countperop from ultra.ini
// (Mupen64Plus CountPerOp)
static qword lle_ticks(qword n)
{
    if(cart.countperop>0) return(n*cart.countperop);
    return(n*COUNT_NUM/COUNT_DEN);
}

static dword lle_count(void)
{
    qword d=lle_ticks(lle_now()-lle.countref);
    if(d>=lle_countpervi()) d=lle_countpervi()-1;
    return((dword)(lle.countbase+d));
}

// countperop games: the CPU has run a whole frame and waits for the retrace
// (sync.c), so VIs come after the same work on any host. Xena's scheduler
// is set up by its first retrace message (every 2nd VI); a fast host ran
// init past it into its first task, and the task thread read a null frame.
int lle_framedone(void)
{
    if(!st.lleos || cart.countperop<=0) return(0);
    return(lle_ticks(lle_now()-lle.countref)>=lle_countpervi());
}

static void lle_countretrace(void)
{
    lle.countbase+=lle_countpervi();
    // countperop games: the next frame starts where this one's instructions
    // ended, not at the retrace, which comes a few bursts after the frame is
    // done (sync.c). Counting from the retrace made each frame about 6%
    // long, and Rogue Squadron ran at 56 VI/s with the audio sync holding
    // it there. More than a frame behind (a load, the debugger): restart.
    if(cart.countperop>0)
    {
        qword frame=lle_countpervi()/cart.countperop;
        lle.countref+=frame;
        if(lle_now()-lle.countref>frame) lle.countref=lle_now();
    }
    else lle.countref=lle_now();
}

// ask cpu.c to end the burst after this instruction, so interrupts are
// checked right away (after MTC0 Status, ERET, MI mask writes). Only when
// one can be taken now: libdragon toggles Status.IE all the time, and
// cutting every burst there left the emulator mostly doing burst overhead.
static void lle_checksoon(void)
{
    dword status=st.mmu[12].d;
    if(!((status&1) && !(status&6) && (st.mmu[13].d&status&0xff00))) return;
    if(st.bailout>1)
    { // the burst ends early: it only lasts what ran, so Count (lle_now)
      // and cputime don't jump ahead by the part never executed
        lle.burst-=st.bailout-1;
        st.bailout=1;
    }
}

int lle_burstlength(void)
{
    return(lle.burst);
}

/****************************************************************************
** MI interrupts
*/

static void lle_updatecause(void)
{
    if(lle.mi_intr&lle.mi_mask) st.mmu[13].d|= 0x400;  // IP2: RCP
    else                        st.mmu[13].d&=~0x400;
    RMI[2]=lle.mi_intr;
    RMI[3]=lle.mi_mask;
}

static int mibit(dword bit)
{
    int n;
    for(n=0;n<6;n++) if(bit==(1u<<n)) return(n);
    return(0);
}

// an HLE task finished (lle_event): its SP interrupt is on the way
static int lle_sptaskdone;

// the RSP still counts as running (hw.c keeps SP_STATUS halt clear)
int lle_sptaskpending(void)
{
    return(lle_sptaskdone);
}

// an MI interrupt has just become pending: device status that shows it
// (PI_STATUS: DMA done, interrupt set). An HLE task's end (halt, broke,
// signal 2) shows in SP_STATUS with its interrupt, as the microcode's BREAK
// sets both at once (ares RSP::BREAK). Shown earlier, Ogre Battle 64 started
// its next task in the gap (it polls halt), both ends made one interrupt,
// and its graphics thread waited forever for its task-done message: black.
static void lle_irqfired(dword bit)
{
    if(bit==MI_PI) RPI[4]=(RPI[4]&~1u)|8u;
    if(bit==MI_SI) lle_sireaddone();
    if(bit==MI_SP && lle_sptaskdone)
    {
        lle_sptaskdone=0;
        hw_sp_taskdone();
    }
}

static void lle_raise(dword bit,int delay)
{
    int n=mibit(bit);
    if(delay<=0)
    {
        lle_irqfired(bit);
        lle.mi_intr|=bit;
        lle.irqs[n]++;
        lle_updatecause();
        lle_checksoon();
    }
    else
    {
        qword at=lle_now()+delay;
        if(!lle.pending[n] || at<lle.pending[n]) lle.pending[n]=at;
    }
}

static void lle_clear(dword bit)
{
    lle.mi_intr&=~bit;
    lle_updatecause();
}

/****************************************************************************
** CPU exceptions
*/

void lle_exception(int code)
{
    dword status=st.mmu[12].d;
    dword cause =st.mmu[13].d;

    if(st.dumphw && code!=0)
        print("lle: exception %i at %08X BadVAddr %08X RA %08X\n",
            code,st.pc,st.mmu[8].d,st.g[31].d);

    if(!lle.excseen[code&31])
    {
        lle.excseen[code&31]=1;
        print("lle: first exception code %i at %08X\n",code,st.pc);
        if(code==EXC_TLBL || code==EXC_TLBS)
        { // a stray pointer: where it came from
            int r;
            print("lle:   BadVAddr %08X RA %08X SP %08X\n",
                st.mmu[8].d,st.g[31].d,st.g[29].d);
            for(r=0;r<32;r+=4)
                print("lle:   r%-2i %08X %08X %08X %08X\n",r,
                    st.g[r].d,st.g[r+1].d,st.g[r+2].d,st.g[r+3].d);
        }
        if(code==EXC_SYS)
        { // libdragon: SYSCALL 1 is a failed assert, a0 the expression and
          // a1 the message format (inspector.c)
            int a,i;
            for(a=4;a<6;a++)
            {
                char text[128];
                dword p=st.g[a].d;
                if(p<0x80000000 || p>=0x80000000+(dword)mem.ramsize) continue;
                for(i=0;i<127;i++)
                {
                    text[i]=(char)mem_read8(p+i);
                    if(!text[i]) break;
                }
                text[i]=0;
                print("lle:   a%i \"%s\"\n",a-4,text);
            }
        }
    }

    lle.excs++;
    cause&=~(0x7cu|0x80000000u);
    // CE names the coprocessor for Coprocessor Unusable and for reserved
    // COP2 instructions (set by the caller); the others read 0 there
    // (n64-systemtest "Cause during AdEL exception", cop_unusable)
    if(code!=EXC_CPU && code!=EXC_RI) cause&=~0x30000000u;
    cause|=(code&31)<<2;
    if(!(status&2)) // EXL clear: record where to return
    {
        qword epc=cpu_pc64get(); // 64 bits (cpuc.c cpu_pc64)
        if(st.branchdelay>0)
        { // in a branch delay slot: return to the branch
            epc-=4;
            cause|=0x80000000u;
        }
        st.mmu[14].q=epc;
    }
    st.mmu[13].d=cause;
    st.mmu[12].d=status|2;  // EXL
    st.branchdelay=0;
    cpu_pc64=0;             // the vectors are in KSEG0/1
    st.pc=(status&0x00400000)?0xBFC00380:0x80000180; // BEV
    lle_jumped=1;
}

// libultra starts threads with CU1 clear and saves a thread's FPU
// registers on a switch only after its first FPU opcode took this exception
// (handle_CpU sets THREAD_FP). Without it a preempted thread loses its FPU
// state: Banjo-Tooie's inflate keeps integers in FPU registers.
int lle_cop1unusable(void)
{
    if(st.mmu[12].d&0x20000000) return(0);
    st.mmu[13].d=(st.mmu[13].d&~0x30000000u)|0x10000000u; // CE=1
    lle_exception(EXC_CPU);
    return(1);
}

static void lle_checkint(void)
{
    dword status=st.mmu[12].d;
    dword cause =st.mmu[13].d;
    if((status&1) && !(status&6) && (cause&status&0xff00))
    {
        lle_exception(EXC_INT);
        lle_jumped=0; // taken between instructions, nothing to skip
    }
}

void lle_eret(void)
{
    dword status=st.mmu[12].d;
    qword pc;
    if(status&4)
    {
        pc=st.mmu[30].q;     // ErrorEPC
        status&=~4u;
    }
    else
    {
        pc=st.mmu[14].q;     // EPC
        status&=~2u;
    }
    // all 64 bits: code may run in xkseg (cpuc.c cpu_pc64)
    st.pc=(dword)pc;
    cpu_pc64=(pc!=(qword)(qint)(int)(dword)pc);
    cpu_pchi=(dword)(pc>>32);
    st.mmu[12].d=status;
    st.branchdelay=0;
    lle_jumped=1;
    lle_checksoon();
}

/****************************************************************************
** COP0 registers
*/

// timer: Count reached Compare since the last check -> IP7. Checked at
// each burst and on Cause reads, so a polling loop sees IP7 as soon as
// Count gets there (n64-systemtest "Compare (signalling)").
static void lle_timer(void)
{
    dword count=lle_count();
    if((dword)(st.mmu[11].d-lle.lastcount-1)<(dword)(count-lle.lastcount))
    {
        if(st.dumphw) print("lle: timer interrupt, Count %08X Compare %08X\n",count,st.mmu[11].d);
        st.mmu[13].d|=0x8000;
        lle.irqs[6]++;
        lle_checksoon();
    }
    lle.lastcount=count;
}

dword lle_mfc0(int reg)
{
    switch(reg)
    {
    case 13: // Cause
        lle_timer();
        break;
    case 1: // Random: Wired..31
        {
            dword wired=st.mmu[6].d&63;
            if(wired>31)
            { // 0..63 (ares); a step per read, as reads within one burst
              // all see the same time (n64-systemtest "Wired OOB/Random")
                static dword x=0x2545F491;
                x^=x<<13; x^=x>>17; x^=x<<5;
                return((x^(dword)lle_now())&63);
            }
            return(31-(dword)(lle_now()%(32-wired)));
        }
    case 9: // Count
        return(lle_count());
    }
    return(st.mmu[reg].d);
}

void lle_mtc0(int reg,dword v)
{
    switch(reg)
    {
    case 9: // Count
        lle.countbase=v;
        lle.countref =lle_now();
        lle.lastcount=v;
        break;
    case 11: // Compare: clears the timer interrupt; only Count values
        // after the write can match (a Compare in the past waits for the
        // wrap, n64-systemtest "Compare (past)")
        st.mmu[11].d=v;
        st.mmu[13].d&=~0x8000u;
        lle.lastcount=lle_count();
        if(st.dumphw) print("lle: Compare <- %08X, Count %08X (pc %08X ra %08X)\n",v,lle.lastcount,st.pc,st.g[31].d);
        break;
    case 12: // Status (FR moves the FPU registers' upper halves, cpuc.c)
        cpu_fpusetfr(st.mmu[12].d&0x04000000,v&0x04000000);
        st.mmu[12].d=v;
        lle_checksoon();
        break;
    case 13: // Cause: only the software interrupt bits are writable
        st.mmu[13].d=(st.mmu[13].d&~0x300u)|(v&0x300u);
        lle_checksoon();
        break;
    case 10: // EntryHi: a new ASID selects other TLB entries
        {
            dword old=st.mmu[10].d;
            st.mmu[10].d=v;
            if((old^v)&0xff) tlb_remap();
        }
        break;
    default:
        st.mmu[reg].d=v;
        break;
    }
}

// COP0 as the CPU sees it (MFC0/DMFC0/MTC0/DMTC0), register widths and
// write masks from n64-systemtest (cop0 masking tests). 64-bit registers:
// Context, BadVAddr, EntryHi, EPC, XContext, ErrorEPC; DMFC0 of the others
// is zero-extended. Every write also lands in a latch that the unused
// registers (7, 21-25, 31) read back.
static qword cop0latch;

static int cop0_is64(int reg)
{
    return(reg==4 || reg==8 || reg==10 || reg==14 || reg==20 || reg==30);
}

qword lle_dmfc0(int reg)
{
    reg&=31;
    switch(reg)
    {
    case 7: case 21: case 22: case 23: case 24: case 25: case 31:
        return(cop0latch);
    case 27: case 29: // CacheErr, TagHi
        return(0);
    }
    if(cop0_is64(reg)) return(st.mmu[reg].q);
    return((qword)lle_mfc0(reg));
}

void lle_dmtc0(int reg,qword v)
{
    reg&=31;
    cop0latch=v;
    switch(reg)
    {
    case 0:  st.mmu[0].q=v&0x8000003Full; break;             // Index
    case 1: case 8: case 15: case 27: case 29: break;        // read-only
    case 4:  // Context: PTEBase (bits 63..23); BadVPN2 is the CPU's
        st.mmu[4].q=(v&~0x7fffffull)|(st.mmu[4].q&0x7ffff0ull);
        break;
    case 2: case 3: st.mmu[reg].q=v&0x3fffffffull; break;    // EntryLo0/1
    case 5:  st.mmu[5].q=v&0x01ffe000ull; break;             // PageMask
    case 6:  st.mmu[6].q=v&0x3full; break;                   // Wired
    case 10: // EntryHi: R (63..62), VPN2 (39..13), ASID (7..0)
        v&=0xC00000FFFFFFE0FFull;
        lle_mtc0(10,(dword)v); st.mmu[10].q=v;
        break;
    case 12: lle_mtc0(12,(dword)v&~0x00080000u); break;      // Status: bit 19 reads 0
    case 9: case 11: case 13: lle_mtc0(reg,(dword)v); break;
    case 14: case 30: st.mmu[reg].q=v; break;                // EPC, ErrorEPC
    case 16: // Config: EP, BE, CU, K0 writable
        st.mmu[16].q=(st.mmu[16].q&~0x0f00800full)|(v&0x0f00800full);
        break;
    case 17: case 28: st.mmu[reg].q=(dword)v; break;         // LLAddr, TagLo
    case 20: // XContext: PTEBase (bits 63..33)
        st.mmu[20].q=(v&0xfffffffe00000000ull)|(st.mmu[20].q&0x1ffffffffull);
        break;
    case 26: st.mmu[26].q=v&0xff; break;                     // PErr
    default: st.mmu[reg].q=v; break;
    }
}

/****************************************************************************
** TLB
**
** KUSEG and 0xC0000000.. can be mapped; KSEG0/1 are fixed. UltraHLE's own
** accesses go through the KSEG0/1 aliases in LLE mode (mem_phys in mem.h),
** so the game may remap all of KUSEG (Acclaim games run code at
** 0x00400000, n64-systemtest at 0x12345000). Mappings are copied from the
** KSEG0 alias, never from
** low pages the game may itself have remapped. Unmapped KUSEG and KSEG2/3
** pages point at a miss page: CPU accesses there raise TLB exceptions, which
** the Turok engine uses to map code on demand. Only entries of the current
** ASID (or global) are mapped, clean pages (D=0) for reads only, and an
** ASID change in EntryHi remaps. osUnmapTLBAll writes invalid entries with
** EntryHi in KSEG0, which must not change anything.
*/

dword lle_misspage[1024];

// all of KUSEG and KSEG2/3 (UltraHLE's own accesses use KSEG0/1, mem_phys)
static int tlb_mappable(dword page)
{
    return(page<0x80000000 || page>=0xC0000000);
}

// the KSEG0 alias of a physical address (fixed, never TLB mapped)
static dword tlb_physpage(dword phys)
{
    return((phys&0x1fffffff)|0x80000000);
}

// what an unmapped page reverts to: the miss page, in KUSEG and in the
// mapped kernel segments KSEG2/3 alike (no physical alias there)
static void tlb_defaultpage(dword page)
{
    mem_mapexternal(page,MAP_RW,lle_misspage);
}

// every TLB-mapped page back to the miss page (boot, state load)
static void tlb_defaultall(void)
{
    dword page=0;
    do
    {
        if(tlb_mappable(page)) tlb_defaultpage(page);
        page+=4096;
    } while(page);
}

// does the entry translate addresses of the current address space? Global
// entries (G set in both halves) match any ASID, others EntryHi's ASID.
static int tlb_asidmatch(LleTlb *e)
{
    return((e->lo0&e->lo1&1) || ((e->hi^st.mmu[10].d)&0xff)==0);
}

// EntryHi VPN2 bits 39..32 of an entry. hi keeps VPN2 31..13, the region
// in bits 9..8 and, in bit 10, "bits 39..32 are not the sign extension of a
// 32-bit address" (0 for region 0, 0xFF for region 3); those entries keep
// the bits in tlb_vpnhi (not in save states: games use 32-bit mappings).
#define TLB_HI64 0x400u
static dword tlb_vpnhi[32];

static dword tlb_hibits(int i)
{
    LleTlb *e=&lle.tlb[i];
    if(e->hi&TLB_HI64) return(tlb_vpnhi[i]);
    return(((e->hi>>8)&3)==3?0xffu:0u);
}

// an entry a 32-bit (sign-extended) address can hit: KUSEG in region 0,
// KSEG2/3 in region 3
static int tlb_is32(LleTlb *e)
{
    dword r=(e->hi>>8)&3;
    if(e->hi&TLB_HI64) return(0);
    if(r==0) return(e->hi<0x80000000u);
    if(r==3) return(e->hi>=0xC0000000u);
    return(0);
}

// the entry translating the 32-bit address addr now, or 0
static LleTlb *tlb_find(dword addr)
{
    int i;
    for(i=0;i<32;i++)
    {
        LleTlb *e=&lle.tlb[i];
        if(((e->hi^addr)&~(e->mask|0x1fff))==0 && tlb_is32(e) && tlb_asidmatch(e)) return(e);
    }
    return(0);
}

// the entry translating the 64-bit address a (region, VPN2 39..13, ASID)
static int tlb_find64(qword a)
{
    int i;
    for(i=0;i<32;i++)
    {
        LleTlb *e=&lle.tlb[i];
        if(((e->hi>>8)&3)!=(dword)(a>>62)) continue;
        if(tlb_hibits(i)!=(dword)((a>>32)&0xff)) continue;
        if((((dword)a^e->hi)&~(e->mask|0x1fff))!=0) continue;
        if(!tlb_asidmatch(e)) continue;
        return(i);
    }
    return(-1);
}

// the current mode addresses 64 bits (KX/SX/UX for kernel/supervisor/user)
static int lle_mode64(void)
{
    dword s=st.mmu[12].d;
    int   m=(s&6)?0:(int)((s>>3)&3);
    return(m==0?(s&0x80)!=0:m==1?(s&0x40)!=0:(s&0x20)!=0);
}

// TLB refill vector: 0x000, or 0x080 (XTLB) in 64-bit addressing; BEV moves
// both to 0xBFC00200+
static dword lle_refillvector(void)
{
    dword off=lle_mode64()?0x80:0;
    return(((st.mmu[12].d&0x00400000)?0xBFC00200u:0x80000000u)+off);
}

// MI repeat mode (MI_MODE bit 7): the next uncached CPU store takes it.
// Returns the repeat length (1..128) and disarms, or 0 when not armed.
int lle_mirepeat(void)
{
    if(!(RMI[0]&0x80)) return(0);
    RMI[0]&=~0x80u;
    return((int)(RMI[0]&0x7f)+1);
}

// a TLB or address error exception on address a: BadVAddr, and the bad
// VPN2 in Context (bits 31..13 -> 22..4) and XContext (R 63..62 -> 32..31,
// VPN2 39..13 -> 30..4), keeping their PTEBase fields
void lle_badvaddr(qword a)
{
    st.mmu[8].q=a;
    st.mmu[4].q=(st.mmu[4].q&~0x7ffff0ull)|((a>>9)&0x7ffff0ull);
    st.mmu[20].q=(st.mmu[20].q&0xfffffffe00000000ull)|((a>>62)<<31)|
                 (((a>>13)&0x7ffffffull)<<4);
    // EntryHi: R and VPN2, the ASID stays (ares CPU::addressException,
    // for address errors too)
    st.mmu[10].q=(a&0xC00000FFFFFFE000ull)|(st.mmu[10].q&0xff);
}

// TLB generation: changes whenever a translation may have changed (TLB
// writes, ASID changes), so cpucache.c can memoize page lookups
dword lle_tlbgen;

// a TLB-mapped 32-bit address: its physical address and whether the page
// is cached (EntryLo C != 2). 0 = no valid entry.
int lle_tlbphys(dword addr,dword *phys,int *cached)
{
    LleTlb *e=tlb_find(addr);
    dword   size,lo;
    if(!e) return(0);
    size=((e->mask|0x1fff)+1)>>1;
    lo=(addr&size)?e->lo1:e->lo0;
    if(!(lo&2)) return(0);
    *phys=(((lo>>6)&0xfffff)<<12)+(addr&(size-1));
    *cached=((lo>>3)&7)!=2;
    return(1);
}

// the physical address of a 32-bit virtual one (KSEG0/1 fixed, the rest
// through the TLB; unmapped addresses give their low 29 bits)
dword lle_virt2phys(dword addr)
{
    LleTlb *e;
    if(!tlb_mappable(addr&~0xfffu)) return(addr&0x1fffffff);
    e=tlb_find(addr);
    if(e)
    {
        dword size=((e->mask|0x1fff)+1)>>1;
        dword lo=(addr&size)?e->lo1:e->lo0;
        return((((lo>>6)&0xfffff)<<12)+(addr&(size-1)));
    }
    return(addr&0x1fffffff);
}

// A CPU access to a miss page (read side, or write side for stores). No
// matching entry: TLB refill (vector 0x000 when EXL is clear). A matching
// entry that is invalid: TLBL/TLBS; a store to a valid clean (D=0) page:
// TLB Modified. Both of those go to the general vector.
int lle_checkmiss(dword addr,int write)
{
    byte   *host=(byte *)(write?memdataw(addr):memdatar(addr));
    dword   status=st.mmu[12].d;
    LleTlb *e;
    int     code=write?EXC_TLBS:EXC_TLBL,refill=1;
    if(host-(addr&0xfff)!=(byte *)lle_misspage) return(0);

    e=tlb_find(addr);
    if(e)
    {
        dword size=((e->mask|0x1fff)+1)>>1;
        dword lo=(addr&size)?e->lo1:e->lo0;
        if(!(lo&2)) refill=0;                            // invalid
        else if(write && !(lo&4)) { refill=0; code=EXC_MOD; } // clean
    }

    lle_badvaddr((qword)(qint)(int)addr);
    st.mmu[10].q=(qword)(qint)(int)((addr&0xffffe000)|(st.mmu[10].d&0xff)); // EntryHi
    st.mmu[10].q&=0xC00000FFFFFFE0FFull;
    lle_exception(code);
    if(refill && !(status&2)) st.pc=lle_refillvector();
    return(1);
}

int lle_tlb64cached; // the last lle_tlb64 translation's page is cached

// a load/store at a mapped 64-bit address (xuseg/xsseg/xkseg beyond 32
// bits): translate through the TLB (*phys), or raise the TLB exception
// (refill at 0x080). 1 = exception taken.
int lle_tlb64(qword a,int write,dword *phys)
{
    dword  status=st.mmu[12].d;
    int    code=write?EXC_TLBS:EXC_TLBL,refill=1;
    int    i=tlb_find64(a);
    if(i>=0)
    {
        LleTlb *e=&lle.tlb[i];
        dword size=((e->mask|0x1fff)+1)>>1;
        dword lo=((dword)a&size)?e->lo1:e->lo0;
        if(!(lo&2)) refill=0;
        else if(write && !(lo&4)) { refill=0; code=EXC_MOD; }
        else
        {
            *phys=(((lo>>6)&0xfffff)<<12)+((dword)a&(size-1));
            lle_tlb64cached=((lo>>3)&7)!=2;
            return(0);
        }
    }
    lle_badvaddr(a);
    st.mmu[10].q=(a&0xC00000FFFFFFE000ull)|(st.mmu[10].d&0xff);
    lle_exception(code);
    if(refill && !(status&2)) st.pc=lle_refillvector();
    return(1);
}

// apply (map=1) or remove (map=0) the mapping of one TLB entry. Only
// entries of the current ASID (or global) are mapped; a clean page (D=0) is
// mapped for reads only, its stores hit the miss page (TLB Modified).
static void tlb_apply(LleTlb *e,int map)
{
    dword size=((e->mask|0x1fff)+1)>>1; // bytes per half (even/odd page)
    dword vpn2=e->hi&~(e->mask|0x1fff);
    int   half;
    dword off;

    if(!(e->lo0&2) && !(e->lo1&2)) return; // nothing valid
    if(!tlb_is32(e)) return;               // 64-bit only: lle_tlb64
    if(map && !tlb_asidmatch(e)) return;
    for(half=0;half<2;half++)
    {
        dword lo=half?e->lo1:e->lo0;
        dword virt=vpn2+half*size;
        dword phys=((lo>>6)&0xfffff)<<12;
        if(!(lo&2)) continue;
        for(off=0;off<size;off+=4096)
        {
            if(!tlb_mappable(virt+off)) continue;
            if(!map) tlb_defaultpage(virt+off);
            else if(lo&4) mem_mapcopy(virt+off,MAP_RW,tlb_physpage(phys+off));
            else
            {
                mem_mapcopy(virt+off,MAP_R,tlb_physpage(phys+off));
                mem_mapexternal(virt+off,MAP_W,lle_misspage);
            }
        }
    }
}

// EntryHi's ASID changed: the other address space's entries apply now
static void tlb_remap(void)
{
    int i;
    lle_tlbgen++;
    for(i=0;i<32;i++) tlb_apply(&lle.tlb[i],0);
    for(i=0;i<32;i++) tlb_apply(&lle.tlb[i],1);
}

// An entry as ares stores it (tlb.cpp Entry::synchronize): PageMask keeps
// its odd bits, copied down into pairs (0x2000 alone is 0); EntryLo keeps a
// 32-bit physical address (PFN bits 6..25) and G is the AND of both halves;
// VPN2 is masked by the page size. The region (EntryHi bits 63..62) lives in
// bits 9..8 of hi, which EntryHi doesn't use (and the save states don't
// change shape).
static void tlb_write(int i)
{
    LleTlb *e=&lle.tlb[i&31];
    dword   mask=st.mmu[5].d&(0xAAAu<<13);
    dword   g,region=(dword)(st.mmu[10].q>>62);
    dword   vpnhi=(dword)(st.mmu[10].q>>32)&0xff;
    mask|=mask>>1;
    tlb_apply(e,0);
    lle_tlbgen++;
    g=st.mmu[2].d&st.mmu[3].d&1;
    e->mask=mask;
    e->hi  =(st.mmu[10].d&~(mask|0x1fff))|(st.mmu[10].d&0xff)|(region<<8);
    if(vpnhi!=(region==3?0xffu:0u))
    {
        e->hi|=TLB_HI64;
        tlb_vpnhi[i&31]=vpnhi;
    }
    e->lo0 =(st.mmu[2].d&0x03fffffe)|g;
    e->lo1 =(st.mmu[3].d&0x03fffffe)|g;
    tlb_apply(e,1);
}

void lle_tlbwi(void) { tlb_write(st.mmu[0].d&31); }
void lle_tlbwr(void) { tlb_write(lle_mfc0(1)); }

void lle_tlbr(void)
{
    LleTlb *e=&lle.tlb[st.mmu[0].d&31];
    qword   vpn=e->hi&~(e->mask|0x1fff);
    vpn|=(qword)tlb_hibits(st.mmu[0].d&31)<<32;            // VPN2 bits 39..32
    st.mmu[5].d =e->mask;
    st.mmu[10].q=vpn|(e->hi&0xff)|((qword)((e->hi>>8)&3)<<62);
    st.mmu[2].q =e->lo0;
    st.mmu[3].q =e->lo1;
}

void lle_tlbp(void)
{
    dword hi=st.mmu[10].d;
    dword region=(dword)(st.mmu[10].q>>62);
    int   i;
    for(i=0;i<32;i++)
    {
        LleTlb *e=&lle.tlb[i];
        dword   m=~(e->mask|0x1fff);
        if(((e->hi^hi)&m)) continue;
        if(((e->hi>>8)&3)!=region) continue;
        if(tlb_hibits(i)!=((dword)(st.mmu[10].q>>32)&0xff)) continue;
        if(!((e->lo0&e->lo1&1) || ((e->hi^hi)&255)==0)) continue;
        st.mmu[0].d=i;
        return;
    }
    st.mmu[0].d=0x80000000u;
}

/****************************************************************************
** PIF (controllers) and SI
*/

// A PIF -> RDRAM transfer reaches RDRAM when it is done, with the SI
// interrupt, not when it is started. libultra fills its buffer with 0xFF
// words (in the D-cache), starts the read and calls osInvalDCache on the
// buffer, which writes a partly covered first and last cache line back to
// RDRAM before dropping them. On the console the PIF data lands after that.
// Copied at the start, the write-back went over it: Bottom of the 9th's
// buffer is at 80072F88, half way into a line, so its first 8 bytes, the
// first controller's reply, came back as the fill (no buttons, stick y -1)
// and the game never saw Start. Other games' buffers start on a line.
static dword si_readdram; // RDRAM address of the read in flight, 0: none

static void lle_sireaddone(void)
{
    int i;
    if(!si_readdram) return;
    // the devices answer when the game reads PIF RAM back (Mupen64Plus
    // dma_si_read -> update_pif_ram): libultra writes the read-buttons
    // block once and then only reads, every frame, so answering only
    // on the write froze the pad at its first state
    pif_read(0);
    for(i=0;i<16;i++) mem_write32(si_readdram+i*4,RPIF[0x1f0+i]);
    si_readdram=0;
    RSI[6]&=~1u; // not busy
}

// joybus commands run in pif.c (shared with HLE mode)
static void lle_si(int topif)
{
    dword dram=(WSI[0]&0x1fffffff)|0x80000000;
    int   i;
    if(!cart.first_pad)
    {
        print("note: first pad access\n");
        cart.first_pad=1;
    }
    lle_sireaddone(); // a read still in flight first: PIF RAM is about to change
    RSI[0]=WSI[0];
    RSI[6]=0; // not busy
    if(topif)
    {
        for(i=0;i<16;i++) RPIF[0x1f0+i]=mem_read32(dram+i*4);
        pif_write(0);
    }
    else
    {
        si_readdram=dram; // copied in lle_irqfired
        RSI[6]|=1u;       // DMA busy until then
    }
    lle_raise(MI_SI,SI_DELAY);
}

/****************************************************************************
** hardware register writes
*/

static void hw_unknown(int block,dword phys,dword v)
{
    int i=(block*8+((phys>>2)&7))&63;
    if(!lle.hwseen[i])
    {
        lle.hwseen[i]=1;
        print("lle: write %08X to %08X ignored\n",v,phys);
    }
}

// SP_SEMAPHORE: a read returns the old value (already loaded) and sets it,
// a write clears it (lle_hwwrite). The RSP's MFC0 does the same in su.c.
void lle_hwread(dword phys)
{
    if(phys==0x0404001C) RSP[7]=1;
}

// Registers whose value is made when read, before the load. DPC_CLOCK counts
// RCP cycles (62.5 MHz, 4/3 of Count) since the last clear, 24 bits (ares).
// Destruction Derby divides the busy counters by it and hit a divide-by-zero
// break when it stayed 0.
void lle_hwpreread(dword phys)
{
    if(phys==0x04100010)
        RDP[4]=(dword)(lle_ticks(lle_now()-lle.dpclockref)*4/3)&0xffffff;
}

// After a PI DMA of len bytes the address registers point past it
// (Mupen64Plus dma_pi_read/dma_pi_write): DRAM rounded up to 8 bytes, cart
// to 2. Cart->RDRAM lengths follow the PI's rules, as in osPiStartDma.
static void pi_advance(dword len,int toram)
{
    dword dram=WPI[0]&0x00fffffe,cart=WPI[1]&~1u;
    if(toram)
    {
        if(len>=0x7f && (len&1)) len++;
        if(len<=0x80) len=(len>=(dram&7))?len-(dram&7):0;
    }
    RPI[0]=WPI[0]=(dram+len+7)&~7u;
    RPI[1]=WPI[1]=(cart+len+1)&~1u;
}

// Cart ROM -> RDRAM (PI_WR_LEN), transcribed from ares (n64/pi/dma.cpp
// PI::dmaWrite, commit 4cb8d92b): the cart is read a halfword at a time
// into blocks of up to 128 bytes that end at RDRAM 2 KB page boundaries;
// the DRAM misalignment shortens blocks and the first short block is written
// bytewise. Leaves PI_DRAM_ADDR, PI_CART_ADDR and PI_WR_LEN as the hardware
// does (n64-systemtest cart_memory "DMA CART -> RDRAM ...").
static void pi_dmawrite(void)
{
    byte  buf[130];
    dword dram=WPI[0]&0x00fffffe,pbus=WPI[1]&~1u,wlen=0;
    int   length=(int)(WPI[3]&0x00ffffff)+1,maxblock=128,first=1;

    while(length>0)
    {
        int misalign=(int)(dram&7);
        int distend=0x800-(int)(dram&0x7ff);
        int blocklen=(maxblock-misalign<distend)?maxblock-misalign:distend;
        int curlen=(length<blocklen)?length:blocklen;
        int i;
        for(i=0;i<curlen;i+=2)
        {
            dword h=(dword)mem_read16(0xA0000000|(pbus&0x1fffffff));
            buf[i]=(byte)(h>>8);
            buf[i+1]=(byte)h;
            pbus+=2;
            length-=2;
        }
        if(first && curlen<127-misalign)
        {
            for(i=0;i<curlen-misalign;i++,dram++)
                if(dram<(dword)mem.ramsize) mem_write8(0xA0000000|dram,buf[i]);
        }
        else
        {
            for(i=0;i<curlen-misalign;i++,dram++)
                if(dram<(dword)mem.ramsize) mem_write8(0xA0000000|dram,buf[i]);
            if((curlen-misalign)&1) // whole halfwords
            {
                if(dram<(dword)mem.ramsize) mem_write8(0xA0000000|dram,buf[i]);
                dram++;
            }
        }
        dram=(dram+7)&~7u;
        wlen=(curlen<=8)?(dword)(127-misalign):127;
        first=0;
        maxblock=(distend<8)?128-misalign:128;
    }
    RPI[0]=WPI[0]=dram&0x00ffffff;
    RPI[1]=WPI[1]=pbus;
    RPI[3]=wlen;
}

void lle_hwwrite(dword phys)
{
    int reg=(phys&0xfffff)>>2;

    if(lle_trace>0)
    { // bring-up trace of register writes (skips the frequent VI/MI acks)
        dword pg=phys&0x1ff00000;
        if(pg!=0x04400000 && pg!=0x04300000)
        {
            lle_trace--;
            print("lle: hw %08X <- %08X (pc %08X)\n",phys,*memdataw(phys|0xA0000000),st.pc);
        }
    }

    switch(phys&0x1ff00000)
    {
    case 0x04000000: // SP registers (0x04040000) and SP PC (0x04080000)
        if((phys&0x1fffffff)==0x04080000)
        {
            RSP2[0]=WSP2[0]&0xffc; // SP_PC: where the RSP starts
        }
        else if((phys&0x1fff0000)==0x04040000)
        {
            switch((phys&0xffff)>>2)
            {
            case 0: RSP[0]=WSP[0]; break; // SP_MEM_ADDR
            case 1: RSP[1]=WSP[1]; break; // SP_DRAM_ADDR
            case 2: // SP_RD_LEN: DMA into DMEM/IMEM
                hw_sp_dmawrite();
                break;
            case 3: // SP_WR_LEN: DMA from DMEM/IMEM to RDRAM
                rsp_dma(1);
                WSP[3]=0;
                break;
            case 4: // SP_STATUS
                // clear/set interrupt; both at once changes nothing
                if((WSP[4]&0x18)==0x08) lle_clear(MI_SP);
                if((WSP[4]&0x18)==0x10) lle_raise(MI_SP,0);
                hw_sp_statuswrite();
                break;
            case 7: // SP_SEMAPHORE
                RSP[7]=0;
                break;
            }
        }
        break;
    case 0x04100000: // DP command registers
        switch(reg)
        {
        case 0: // DPC_START: the next list begins here, taken when END is
            // written (START_VALID until then; later START writes ignored,
            // n64-systemtest "RSP STATUS: start-valid")
            if(!(RDP[3]&0x400))
            {
                RDP[0]=WDP[0]&0xfffff8;
                RDP[3]|=0x400;
            }
            break;
        case 1: // DPC_END: run the list up to here
            RDP[1]=WDP[1]&0xfffff8;
            if(RDP[3]&0x400)
            {
                RDP[2]=RDP[0];  // CURRENT
                RDP[3]&=~0x400u;
            }
            // CBUF_READY always; START_GCLK and PIPE_BUSY until a SYNC_FULL
            // (rsp_dpclist; ares rdp, n64-systemtest "RDP STATUS")
            RDP[3]|=0xa8u;
            rsp_dpclist();
            break;
        case 3: // DPC_STATUS: clear/set XBUS (bit 0), freeze (1), flush (2)
            {
                dword v=WDP[3];
                if(v&0x01) RDP[3]&=~1u;
                if(v&0x02) RDP[3]|= 1u;
                if(v&0x04) RDP[3]&=~2u;
                if(v&0x08) RDP[3]|= 2u;
                if(v&0x10) RDP[3]&=~4u;
                if(v&0x20) RDP[3]|= 4u;
                // clear TMEM, PIPE, BUF busy and CLOCK counters
                if(v&0x040) RDP[7]=0;
                if(v&0x080) RDP[6]=0;
                if(v&0x100) RDP[5]=0;
                if(v&0x200) { RDP[4]=0; lle.dpclockref=lle_now(); }
                // START_VALID, CBUF_READY, PIPE_BUSY, START_GCLK stay
                RDP[3]=(RDP[3]&0x4afu)|0x80u;
                if(v&0x04)
                { // unfrozen: the held DP interrupt, then what waited
                    if(dp_heldint) lle_raise(MI_DP,0);
                    dp_heldint=0;
                    rsp_dpclist();
                }
            }
            break;
        default:
            hw_unknown(1,phys,WDP[reg&1023]);
            break;
        }
        break;
    case 0x04300000: // MI
        if(reg==0)
        { // MI_MODE: repeat length (6..0), clear/set repeat mode (bits 7/8),
          // clear/set ebus test (9/10), clear DP interrupt (11), clear/set
          // RDRAM register select (12/13). Reads: length, repeat mode (7),
          // ebus (8), register select (9) (ares mi/io.cpp)
            dword v=WMI[0];
            dword r=(RMI[0]&~0x7fu)|(v&0x7f);
            if(v&0x080) r&=~0x080u;
            if(v&0x100) r|= 0x080u;
            if(v&0x200) r&=~0x100u;
            if(v&0x400) r|= 0x100u;
            if(v&0x800) lle_clear(MI_DP);
            if(v&0x1000) r&=~0x200u;
            if(v&0x2000) r|= 0x200u;
            RMI[0]=r&0x3ff;
        }
        else if(reg==3)
        { // MI_INTR_MASK: bit 2n clears, bit 2n+1 sets mask bit n
            dword v=WMI[3];
            int   n;
            for(n=0;n<6;n++)
            {
                if(v&(1u<<(2*n)))   lle.mi_mask&=~(1u<<n);
                if(v&(1u<<(2*n+1))) lle.mi_mask|= (1u<<n);
            }
            lle_updatecause();
            lle_checksoon();
        }
        break;
    case 0x04400000: // VI
        if(reg==4) lle_clear(MI_VI);         // VI_CURRENT write acknowledges
        else
        {
            if(reg==1)
            { // VI_ORIGIN: next frame shown
                st.fb_next=WVI[1];
                rdp_viorigin(WVI[1]);
            }
            RVI[reg&1023]=WVI[reg&1023];     // other registers read back
        }
        break;
    case 0x04500000: // AI
        switch(reg)
        {
        case 0: RAI[0]=WAI[0]; break; // AI_DRAM_ADDR
        case 1: // AI_LEN: queue the buffer, and it plays. Always taken: the
                // FIFO here drains once per retrace, not continuously, so it
                // can look full when the hardware's has room. Refusing that
                // (as ares does when truly full) lost a buffer and its AI
                // interrupt, and Nuclear Strike froze on its title screen.
            ai_push((RAI[0]&0x1fffffff)|0x80000000,WAI[1]&0x3fff8);
            break;
        case 3: lle_clear(MI_AI); break; // AI_STATUS write acknowledges
        case 4: // AI_DACRATE
            if(WAI[4]) osAiSetFrequency(48681812/(WAI[4]+1));
            break;
        }
        break;
    case 0x04600000: // PI
        switch(reg)
        {
        case 0: RPI[0]=WPI[0]&0x00fffffe; break; // PI_DRAM_ADDR
        case 1: RPI[1]=WPI[1]&~1u; break;        // PI_CART_ADDR (n64-systemtest)
        case 2: // PI_RD_LEN: RDRAM -> cart (save memory)
            if(flash_contains(WPI[1])) flash_dmafromram(WPI[1],WPI[0]&0x1fffffff,WPI[2]+1);
            else hw_unknown(6,phys,WPI[2]);
            pi_advance(WPI[2]+1,0);
            WPI[2]=0;
            RPI[4]|=1u; // DMA busy until the interrupt (lle_irqfired)
            lle_raise(MI_PI,PI_DMA_DELAY);
            break;
        case 3: // PI_WR_LEN: cart -> RDRAM
            {
                dword cartaddr=WPI[1]&0x1fffffff;
                if(st.dumphw)
                    print("lle: PI DMA cart %08X -> ram %08X len %X (pc %08X ra %08X)\n",
                        WPI[1],WPI[0],WPI[3]+1,st.pc,st.g[31].d);
                if(cartaddr>=0x10000000 && cartaddr<0x1fc00000)
                    pi_dmawrite();
                else
                {
                    if(flash_contains(cartaddr)) flash_dmatoram(cartaddr,WPI[0]&0x1fffffff,WPI[3]+1);
                    else hw_unknown(7,phys,cartaddr); // SRAM probe
                    pi_advance(WPI[3]+1,1);
                }
                WPI[3]=0;
                RPI[4]|=1u; // DMA busy until the interrupt (lle_irqfired)
                lle_raise(MI_PI,PI_DMA_DELAY);
            }
            break;
        case 4: // PI_STATUS: bit 0 resets (DMA busy, error), bit 1 clears the
            // interrupt (read bit 3); IOBUSY (bit 1) stays with the cart latch
            if(WPI[4]&1) RPI[4]&=~5u;
            if(WPI[4]&2) { lle_clear(MI_PI); RPI[4]&=~8u; }
            break;
        }
        break;
    case 0x04800000: // SI
        switch(reg)
        {
        case 1: lle_si(0); WSI[1]=NULLFILL; break; // PIF -> RDRAM
        case 4: lle_si(1); WSI[4]=NULLFILL; break; // RDRAM -> PIF
        case 6: lle_clear(MI_SI); RSI[6]=0; break; // SI_STATUS acknowledges
        }
        break;
    }
}

/****************************************************************************
** IS-Viewer 64 debug port
*/

static dword lle_isvbuf[0x10000/4];

static void isviewer_map(void)
{
    dword a;
    if(cart.size>LLE_ISVIEWER-0x10000000) return; // ROM there
    for(a=0;a<0x10000;a+=4096)
    {
        mem_mapexternal(LLE_ISVIEWER+a,MAP_RW,(byte *)lle_isvbuf+a);
        mem_mapexternal(0x80000000+LLE_ISVIEWER+a,MAP_RW,(byte *)lle_isvbuf+a);
        mem_mapexternal(0xA0000000+LLE_ISVIEWER+a,MAP_RW,(byte *)lle_isvbuf+a);
    }
}

// the game wrote the text length: print it, a line at a time
void lle_isviewer(void)
{
    static char line[256];
    static int  used;
    dword len=lle_isvbuf[0x14/4];
    dword i;
    if(len>0x10000-0x20) len=0x10000-0x20;
    for(i=0;i<len;i++)
    {
        char c=((char *)lle_isvbuf)[(0x20+i)^3];
        if(c=='\n' || used==sizeof(line)-1)
        {
            line[used]=0;
            print("isviewer: %s\n",line);            used=0;
            if(c=='\n') continue;
        }
        line[used++]=c;
    }
    lle_isvbuf[0x14/4]=0;
}

/****************************************************************************
** events from the device code (os_event in LLE mode)
*/

// The game's screen size (the renderer scales to it), once per retrace so a
// half-written VI mode is never seen. Width is VI_WIDTH; height is the shown
// half-lines times VI_Y_SCALE, widened by 240/237 for the lines NTSC crops,
// as GLideN64's VI_UpdateSize does. Scooby-Doo draws 480x240 this way.
// HLE passes the register values of the game's osViSetMode (p_osViSetMode).
void vi_screensize(dword viwidth,dword hvideo,dword vvideo,dword xscale,dword viyscale)
{
    int w=viwidth&0xfff;
    int vstart=(vvideo>>16)&0x3ff;
    int vend=vvideo&0x3ff;
    int yscale=viyscale&0xfff;
    int h,fields=0;

    // VI_WIDTH twice the width the VI shows (H_VIDEO times X_SCALE): each
    // field is every other line of the buffer. Rogue Squadron's missions
    // show a 512x448 buffer as 1024 wide, origin +0 or +1 line; taken as a
    // 1024 wide screen, its 512 wide buffer looked offscreen (render to
    // texture) and the screen stayed black.
    {
        int hs=(hvideo>>16)&0x3ff,he=hvideo&0x3ff,xs=xscale&0xfff,shown;
        if(he>hs && xs)
        {
            shown=(he-hs)*xs/1024;
            if(shown>=160 && w>=shown*2-8 && w<=shown*2+8) { w/=2; fields=1; }
        }
    }

    if(w<160 || w>1024) return;
    // VI off or not set up yet: no height. A transient or blanking V_START
    // (Scooby-Doo puts 0000xxxx for its first frames and some later ones;
    // real modes start at line ~0x23+) still has the mode's Y scale.
    if(!yscale) h=0;
    // a blanked or bogus V_START with the mode's width unchanged: keep the
    // height. Chef's Luv Shack flips V_VIDEO between 037-1E9 and 000-032
    // while running; as a standard field (below) its picture kept resizing
    // between 378 and 414 lines
    else if((vend<=vstart || vstart<0x10) && w==init.viewportwid && init.viewporthig) return;
    // a blanked or half-set V_START (Batman Beyond sets 0000-0097 while it
    // switches to 640x480): the Y scale over a standard field, 0x23..0x1FD
    // as libultra's NTSC and PAL modes use, not the old height (Batman
    // stayed 640x240, half the picture shown)
    else if(vend<=vstart || vstart<0x10) h=(int)(((0x1fd-0x23)>>1)*yscale/1024*1.0126582f)&~1;
    else h=(int)(((vend-vstart)>>1)*yscale/1024*1.0126582f)&~1;
    h*=1+fields; // both fields' lines
    // no height from the VI yet: leave it 0 so the frame's full-screen
    // fillrect sets it (rdp_fillrect). A guessed 240 stuck on All-Star
    // Baseball 2000's credits, drawn once before its 438-line mode is set.
    if(h<120 || h>768) h=init.viewporthig;
    if(w==init.viewportwid && h==init.viewporthig) return;
    init.viewportwid=w;
    init.viewporthig=h;
    if(h) print("lle: screen resolution %ix%i (VI)\n",w,h);
    else  print("lle: screen width %i (VI), height not set yet\n",w);
}

static void lle_visize(void)
{
    vi_screensize(RVI[2],RVI[9],RVI[10],RVI[12],RVI[13]);
}

// the RSP interpreter changed the SP interrupt: set (BREAK with interrupt
// on break, MTC0 SP_STATUS SET_INTR) or cleared (CLR_INTR)
void lle_rspinterrupt(int on)
{
    if(on) lle_raise(MI_SP,0);
    else   lle_clear(MI_SP);
}

int lle_mipending(dword bit)
{
    return((lle.mi_intr&bit)!=0);
}

void lle_event(int ev)
{
    switch(ev)
    {
    case OS_EVENT_SP:
        lle_sptaskdone=1; // SP_STATUS shows the end with the interrupt
        lle_raise(MI_SP,RSP_TASK_DELAY);
        break;
    case OS_EVENT_DP:
        // a frame finished while the game has the DP frozen: its interrupt
        // waits for the unfreeze (Mupen64Plus DELAY_DP_INT). Banjo-Tooie
        // freezes the DP around its graphics tasks; the DP interrupt
        // arriving while frozen left it with a black screen.
        if(RDP[3]&2) dp_heldint=1;
        else lle_raise(MI_DP,RSP_TASK_DELAY+100);
        break;
    case OS_EVENT_SI: lle_raise(MI_SI,SI_DELAY); break;
    case OS_EVENT_PI: lle_raise(MI_PI,PI_DMA_DELAY); break;
    case OS_EVENT_AI: lle_raise(MI_AI,AI_DELAY); break;
    case OS_EVENT_RETRACE:
        lle_visize();
        lle_countretrace();
        ai_retrace();
        if(rsp_pending()) rsp_run(); // RSP left waiting on the CPU
        if(softrdp_active() || cart.vifb)
        { // the picture is the framebuffer the VI shows. Shown when the
          // game has swapped buffers, or every 6th retrace if it draws in
          // the one on screen: shown at every retrace, each present waited
          // for the monitor's refresh, the emulator was behind its clock
          // for good and the game got 30000 instructions a retrace
          // (2048-64 at one frame a second). An origin a line or two on
          // is the same buffer's other field.
            static dword shown;
            static int   age;
            if(RVI[1]-shown>=0x2000 || ++age>=6)
            {
                int h=init.viewporthig?init.viewporthig:240;
                shown=RVI[1];
                age=0;
                rdp_showvi(RVI[1],RVI[2]&0xfff,h,(RVI[0]&3)==3?4:2);
            }
        }
        // not while the game has the screen black (osViBlack: H_VIDEO 0).
        // Perfect Dark blacks it out to load its intro and uses the frame
        // buffers as work memory meanwhile: a second of static was shown.
        else if((RVI[0]&3)>=2 && init.viewporthig && RVI[9])
            rdp_cpupicture(RVI[1],RVI[2]&0xfff,init.viewporthig,(RVI[0]&3)==3?4:2);
        lle.vis++;
        RVI[4]=0;
        lle_raise(MI_VI,0);
        { // every 2 emulated seconds: where the host's time went (timer.c)
            static int n;
            if(++n>=120) { prof_report(n); n=0; }
        }
        if(lle.vis>=600)
        { // about every 10 seconds: interrupt activity (DP = graphics frames)
            print("lle: 600 VI; SP %i DP %i SI %i AI %i PI %i VI %i timer %i exc %i; MI %02X/%02X Status %08X Cause %08X EPC %08X PC %08X Count %08X Compare %08X\n",
                lle.irqs[0],lle.irqs[5],lle.irqs[1],lle.irqs[2],lle.irqs[4],lle.irqs[3],lle.irqs[6],lle.excs,
                lle.mi_intr,lle.mi_mask,st.mmu[12].d,st.mmu[13].d,st.mmu[14].d,st.pc,lle_count(),st.mmu[11].d);
            print("lle:   SP_STATUS %08X SP_PC %03X rsp %s\n",RSP[4],RSP2[0]&0xfff,
                rsp_pending()?(rsp_sliced()?"running":"waiting on the CPU"):"stopped");
            memset(lle.irqs,0,sizeof(lle.irqs));
            lle.excs=0;
            lle.vis=0;
        }
        break;
    }
}

/****************************************************************************
** per burst
*/

void lle_burststart(int burst)
{
    int   n;
    qword now=st.cputime;

    lle.burst=burst;

    // the RSP runs beside the CPU: 2 of its instructions per 3 CPU ones
    // since the last burst (see rsp.c). countperop games: an instruction is
    // countperop Count ticks, and the RSP does 4/3 per tick (62.5 vs
    // 46.875 MHz). At 2/3 per instruction Rogue Squadron's RSP ran at a
    // quarter speed; with its graphics on the RSP too, audio starved.
    {
        static qword rsptime;
        qword ran=now-rsptime;
        rsptime=now;
        if(ran>30000) ran=30000;
        if(rsp_sliced())
            rsp_runfor(cart.countperop>0?(int)(lle_ticks(ran)*4/3):(int)(ran*2/3));
    }

    for(n=0;n<6;n++)
    {
        if(lle.pending[n] && now>=lle.pending[n])
        {
            lle.pending[n]=0;
            lle_irqfired(1u<<n);
            lle.mi_intr|=1u<<n;
            lle.irqs[n]++;
        }
    }
    ai_update();
    lle_updatecause();
    lle_timer();

    RVI[4]=(RVI[4]+2)%525; // VI_CURRENT keeps moving

    lle_checkint();
}

/****************************************************************************
** save states: interrupts, Count timing and the TLB (the rest of the
** machine is in mem/st). Appended after os_save; states without the block
** load as before.
*/

#define LLE_STATEMAGIC 0x3145454C // "LEE1"

void lle_save(FILE *f1)
{
    dword magic=LLE_STATEMAGIC,size=sizeof(lle);
    fwrite(&magic,1,4,f1);
    fwrite(&size,1,4,f1);
    fwrite(&lle,1,sizeof(lle),f1);
    flash_savestate(f1);
}

void lle_load(FILE *f1)
{
    dword magic=0,size=0;
    int   i;
    if(fread(&magic,1,4,f1)!=4 || magic!=LLE_STATEMAGIC) return;
    if(fread(&size,1,4,f1)!=4 || size!=sizeof(lle))
    {
        warning("lle: save state from another version, interrupt state not loaded");
        return;
    }
    fread(&lle,1,sizeof(lle),f1);
    si_readdram=0; // not in the state: a read in flight is lost, the next one comes a frame later
    if(st.lleos)
    { // mapped segments as at boot (miss pages), then the saved TLB on top
        tlb_defaultall();
        for(i=0;i<32;i++) tlb_apply(&lle.tlb[i],1);
    }
    flash_loadstate(f1);
}

/****************************************************************************
** mode decision and boot state
*/

void lle_decide(int important,int importanttotal,int hasrescan)
{
    memset(&lle,0,sizeof(lle));
    si_readdram=0;
    cache_reset();
    cpu_pc64=0;
    memset(&ai,0,sizeof(ai));
    lle_jumped=0;
    dp_heldint=0;

    // HLE sends retrace messages only to the queue the game gives its
    // (patched) osViSetEvent: without it every thread waits after the first
    // frame. Zelda OoT had 18 important routines but no osViSetEvent.
    switch(inifile_osmode())
    {
    case OSMODE_LLE: st.lleos=1; break;
    case OSMODE_HLE: st.lleos=0; break;
    default:         st.lleos=((important<LLE_IMPORTANT_MIN || !sym_vievent) && !hasrescan); break;
    }
    print("OS mode: %s (%i/%i important routines found%s%s%s)\n",
        st.lleos?"LLE":"HLE",important,importanttotal,
        sym_vievent?"":", no osViSetEvent",
        hasrescan?", ini rescans":"",
        inifile_osmode()!=OSMODE_AUTO?", set by osmode":"");
    init.visize=0;
    if(!st.lleos) return;

    st.memiosp=1; // SP always through its registers

    // screen height comes from the VI (lle_visize), or until it has one
    // from the first full-screen fillrect: not the ultra.ini default
    init.viewporthig=0;

    // nothing in KUSEG or KSEG2/3 is mapped until the game writes the TLB
    tlb_defaultall();

    isviewer_map();

    // IPL3's low memory words (osTvType, osRomBase...) are set in boot_boot

    // IPL2 code left in IMEM, checked by CIC x105 games (Mupen64Plus
    // pif_bootrom_hle_execute)
    {
        static const dword ipl2[8]={
            0x3c0dbfc0,0x8da807fc,0x25ad07c0,0x31080080,
            0x5500fffc,0x3c0dbfc0,0x8da80024,0x3c0bb000 };
        int i;
        for(i=0;i<8;i++) ((dword *)sp.imem)[i]=ipl2[i];
    }

    // COP0 and RCP reset state
    st.mmu[12].d=0x34000000;  // Status: CU0, CU1, FR; interrupts off
    st.mmu[13].d=0;
    st.mmu[15].d=0x00000B22;  // PRId
    st.mmu[16].d=0x7006E463;  // Config (n64-systemtest StartupTest)
    st.mmu[6].d =0;           // Wired
    // libultra's osContInit sleeps until osGetTime is 0.5 s. With Count 0
    // here Indiana Jones got to its save check first: it stops the
    // controller thread, writes a blank EEPROM and waits on
    // __osEepromTimerQ, which the rest of osContInit creates
    lle.countbase=COUNT_BOOT;
    lle.lastcount=COUNT_BOOT;
    lle.countref=st.cputime;
    lle.dpclockref=st.cputime;
    RMI[1]=0x02020102;        // MI_VERSION
    RSP[4]=1;                 // SP halted
    RPI[4]=0;
    RSI[6]=0;
}
