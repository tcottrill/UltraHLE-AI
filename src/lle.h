// LLE OS mode: emulated CPU exceptions and hardware interrupts, so games whose
// libultra version UltraHLE doesn't recognise can run their own OS.
// Design: docs/superpowers/specs/2026-09-24-lle-os-mode-design.md

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// MI_INTR / MI_INTR_MASK bits
#define MI_SP  0x01
#define MI_SI  0x02
#define MI_AI  0x04
#define MI_VI  0x08
#define MI_PI  0x10
#define MI_DP  0x20

// Cause.ExcCode values
#define EXC_INT  0
#define EXC_MOD  1
#define EXC_TLBL 2
#define EXC_TLBS 3
#define EXC_ADEL 4
#define EXC_ADES 5
#define EXC_SYS  8
#define EXC_BP   9
#define EXC_RI  10
#define EXC_CPU 11
#define EXC_OV  12
#define EXC_TR  13
#define EXC_FPE 15

// Timing (instructions); Count advances COUNT_NUM/COUNT_DEN per instruction
#define COUNT_NUM        3
#define COUNT_DEN        4
#define RSP_TASK_DELAY   20000
#define PI_DMA_DELAY     1000
#define SI_DELAY         2000
#define AI_DELAY         1000

// fewer important OS routines than this found -> LLE OS mode
#define LLE_IMPORTANT_MIN 18

extern int lle_jumped; // set when exception entry/ERET changed the PC (cpuc.c)

void  lle_decide(int important,int importanttotal,int hasrescan); // after the boot OS search
void  lle_burststart(int burst); // cpu_exec, before each instruction burst
int   lle_burstlength(void);     // after it: its length, shortened if cut early
void  lle_event(int ev);         // os_event() in LLE mode: device completion -> MI interrupt
void  lle_exception(int code);   // raise a CPU exception (SYSCALL, BREAK, trap)
void  lle_rspinterrupt(int on);  // MI SP interrupt set/cleared by the RSP interpreter
int   lle_mipending(dword bit);  // MI_INTR bit raised (not yet acknowledged)?
int   lle_framedone(void);       // countperop game has run its frame: wait for the retrace

// IS-Viewer 64 debug port (cart space): libdragon's debugf/assert output.
// Text at +0x20, a store of its length to +0x14 prints it to ultra.log.
#define LLE_ISVIEWER 0x13FF0000
void  lle_isviewer(void);
int   lle_cop1unusable(void);    // FPU opcode with Status.CU1 clear: raise it, 1=skip the opcode
void  lle_hwwrite(dword phys);   // CPU store to a hardware register (0x04040000..)
void  lle_hwread(dword phys);    // CPU word load from 0x04000000..: read side effects
void  lle_hwpreread(dword phys); // CPU word load from 0x04100000..: make the value
void  lle_save(FILE *f1);        // save state block (after os_save)
void  lle_load(FILE *f1);        // missing in older states: nothing loaded

// TLB miss: unmapped KUSEG/KSEG2/KSEG3 pages (and the store side of clean
// pages) point at a miss page; a CPU fetch, load (write=0) or store
// (write=1) there raises TLBL/TLBS/Mod and returns 1
int   lle_checkmiss(dword addr,int write);
#define LLE_TLBMISS(a,w) (st.lleos && ((dword)(a)<0x80000000 || \
                          (dword)(a)>=0xC0000000) && \
                          lle_checkmiss((dword)(a),(w)))

// COP0 in LLE mode (cpuc.c op_scc)
dword lle_mfc0(int reg);
void  lle_mtc0(int reg,dword value);
qword lle_dmfc0(int reg);          // MFC0/DMFC0: register widths, latch
void  lle_dmtc0(int reg,qword v);  // MTC0/DMTC0: write masks, read-only regs
dword lle_virt2phys(dword addr);   // 32-bit virtual -> physical (KSEG0/1, TLB)
void  lle_badvaddr(qword a);       // BadVAddr, Context, XContext for exceptions
int   lle_mirepeat(void);          // MI repeat mode: length for this store, disarms
int   lle_tlb64(qword a,int write,dword *phys); // mapped 64-bit address; 1 = exception
extern int   lle_tlb64cached;      // ... and its page is cached (EntryLo C != 2)
extern dword lle_tlbgen;           // changes when TLB translations may change
int   lle_tlbphys(dword addr,dword *phys,int *cached); // TLB-mapped 32-bit address

// cpucache.c: the VR4300's caches in LLE mode (8 KB D-cache, 16 KB I-cache)
dword cache_read32(dword a);        // CPU accesses at 32-bit address a
dword cache_read16(dword a);        // (KSEG0/1 alias or TLB-mapped)
dword cache_read8(dword a);
void  cache_write32(dword a,dword v);
void  cache_write16(dword a,dword v);
void  cache_write8(dword a,dword v);
dword cache_fetch(dword a);         // instruction fetch
void  cache_op(int op,dword a);     // the CACHE instruction
void  cache_flushall(void);         // write back dirty D-cache lines (save state)
void  cache_reset(void);            // invalidate both (reset, state load)
// cpuc.c: a PC outside the sign-extended 32-bit space (st.pc low word,
// cpu_pchi high word while cpu_pc64 is set)
extern dword cpu_pchi;
extern int   cpu_pc64;
qword cpu_pc64get(void);
void  lle_eret(void);
void  lle_tlbwi(void);
void  lle_tlbwr(void);
void  lle_tlbr(void);
void  lle_tlbp(void);

#ifdef __cplusplus
}
#endif
