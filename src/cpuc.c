#include "ultra.h"
#include "opdefs.h"
#include <fenv.h>
#include <float.h>

// FPU arithmetic runs under the game's FCSR rounding mode (op_fpu): the
// compiler must not fold or move floating-point code across fesetround
#pragma fenv_access(on)

#define DUMP64 0 // dump 64 bit arithmetic

// 64-bit FPU register values. With Status.FR=0 (libultra) a 64-bit value
// is an even/odd pair: low word in f[r], high in f[r+1], as LDC1 loads it.
// With FR=1 (libdragon, 64-bit ABI) each of the 32 registers is 64 bits:
// f[r] keeps the low word, fpr_hi[r] the high one. Honoured in LLE mode
// only, where Status is the game's own. Not in save states.
static dword fpr_hi[32];

static int fpr_fr1(void)
{
    return(st.lleos && (st.mmu[12].d&0x04000000));
}

// The hardware has 32 physical 64-bit registers (n64-systemtest
// full_vs_half_mode.rs). FR=0 (half mode): register 2n+1 is the upper half
// of physical register 2n, 64-bit accesses and all arithmetic ignore bit 0
// of the register number. The storage follows the current mode: FR=1 keeps
// the upper halves in fpr_hi, FR=0 in the odd st.f entries, and
// cpu_fpusetfr converts when Status.FR changes.
static qword fpr_get64(int r)
{
    r&=31;
    if(fpr_fr1()) return(((qword)fpr_hi[r]<<32)|(dword)st.f[r].d);
    r&=~1;
    return(((qword)(dword)st.f[r+1].d<<32)|(dword)st.f[r].d);
}

static void fpr_set64(int r,qword v)
{
    r&=31;
    if(fpr_fr1())
    {
        st.f[r].d=(dword)v;
        fpr_hi[r]=(dword)(v>>32);
        return;
    }
    r&=~1;
    st.f[r].d=(dword)v;
    st.f[r+1].d=(dword)(v>>32);
}

// Physical register r in either layout. FR=0 keeps an even register's upper
// half in st.f[r+1] and an odd register in fpr_hi[r-1] (low), fpr_hi[r].
static qword fpr_phys(int r)
{
    r&=31;
    if(fpr_fr1()) return(((qword)fpr_hi[r]<<32)|(dword)st.f[r].d);
    if(!(r&1)) return(((qword)(dword)st.f[r+1].d<<32)|(dword)st.f[r].d);
    return(((qword)fpr_hi[r]<<32)|fpr_hi[r-1]);
}

static void fpr_setphys(int r,qword v)
{
    r&=31;
    if(fpr_fr1())      { st.f[r].d=(int)(dword)v; fpr_hi[r]=(dword)(v>>32); }
    else if(!(r&1))    { st.f[r].d=(int)(dword)v; st.f[r+1].d=(int)(dword)(v>>32); }
    else               { fpr_hi[r-1]=(dword)v; fpr_hi[r]=(dword)(v>>32); }
}

// FPU arithmetic works on physical registers in both modes; in FR=0 only
// fs drops its bit 0 (n64-systemtest "... with odd indices (half mode)":
// ADD.S $1,$29,$31 reads 28 and 31, writes 1). 32-bit results (S, W)
// clear the upper half of the register (LWC1/MTC1 don't).
static int fpu_fs(int r)
{
    return(fpr_fr1()?(r&31):(r&30));
}

static dword fpu_get32(int r)   { return((dword)fpr_phys(r)); }
static float fpu_getf(int r)    { dword b=fpu_get32(r); float f; memcpy(&f,&b,4); return(f); }
static double fpu_getd(int r)   { qword q=fpr_phys(r); double d; memcpy(&d,&q,8); return(d); }
static void fpu_put32(int r,dword bits) { fpr_setphys(r,bits); }
static void fpu_putf(int r,float f)     { dword b; memcpy(&b,&f,4); fpr_setphys(r,b); }
static void fpu_putd(int r,double d)    { qword q; memcpy(&q,&d,8); fpr_setphys(r,q); }

// Status.FR changes: move the upper halves between the two layouts. The odd
// physical registers, invisible in FR=0, wait in fpr_hi[2n] (low word) and
// fpr_hi[2n+1] (high word).
void cpu_fpusetfr(int oldfr,int newfr)
{
    int n;
    if(!oldfr==!newfr) return;
    for(n=0;n<32;n+=2)
    {
        dword p0hi,p1lo,p1hi;
        if(oldfr)
        { // 64-bit -> half mode
            p0hi=fpr_hi[n]; p1lo=(dword)st.f[n+1].d; p1hi=fpr_hi[n+1];
            st.f[n+1].d=(int)p0hi;
            fpr_hi[n]=p1lo; fpr_hi[n+1]=p1hi;
        }
        else
        { // half mode -> 64-bit
            p0hi=(dword)st.f[n+1].d; p1lo=fpr_hi[n]; p1hi=fpr_hi[n+1];
            fpr_hi[n]=p0hi;
            st.f[n+1].d=(int)p1lo; fpr_hi[n+1]=p1hi;
        }
    }
}

#define GETREGS \
        rs=&st.g[OP_RS(opcode)].d; \
        rt=&st.g[OP_RT(opcode)].d; \
        rd=&st.g[OP_RD(opcode)].d;

#define GETREGSIMM \
        rs=&st.g[OP_RS(opcode)].d; \
        rd=&st.g[OP_RT(opcode)].d; \
        imm[0]=SIGNEXT16(OP_IMM(opcode)); \
        rt=imm;

#define GETREGSIMM64 \
        rs=&st.g[OP_RS(opcode)].d; \
        rd=&st.g[OP_RT(opcode)].d; \
        imm[0]=SIGNEXT16(OP_IMM(opcode)); \
        if(imm[0]&0x80000000) imm[1]=-1; else imm[1]=0; \
        rt=imm;

#define GETREGSIMMUNS \
        rs=&st.g[OP_RS(opcode)].d; \
        rd=&st.g[OP_RT(opcode)].d; \
        imm[0]=OP_IMM(opcode); \
        rt=imm;

// HLE only: UltraHLE's OS routines write only the low words of registers,
// so the first 64-bit operation after a return rebuilds the argument
// registers' high words. LLE registers are always real 64-bit values, and
// this would destroy them (a0=0x100000000 became 0).
static void op_64bitexpand(void)
{
    if(!st.expanded64bit && !st.lleos)
    {
        if(DUMP64)
        {
            print("expandargs\n");
        }
        // expand input argument regs
        if(A0.d2[0]&0x80000000) A0.d2[1]=-1; else A0.d2[1]=0;
        if(A1.d2[0]&0x80000000) A1.d2[1]=-1; else A1.d2[1]=0;
        if(A2.d2[0]&0x80000000) A2.d2[1]=-1; else A2.d2[1]=0;
        if(A3.d2[0]&0x80000000) A3.d2[1]=-1; else A3.d2[1]=0;
    }
    st.expanded64bit=1;
}

static void mult64to128(uint64_t op1, uint64_t op2, uint64_t* hi, uint64_t* lo)
{
    uint64_t u1 = (op1 & 0xffffffff);
    uint64_t v1 = (op2 & 0xffffffff);
    uint64_t t = (u1 * v1);
    uint64_t w3 = (t & 0xffffffff);
    uint64_t k = (t >> 32);

    op1 >>= 32;
    t = (op1 * v1) + k;
    k = (t & 0xffffffff);
    uint64_t w1 = (t >> 32);

    op2 >>= 32;
    t = (u1 * op2) + k;
    k = (t >> 32);

    *hi = (op1 * op2) + w1 + k;
    *lo = (t << 32) + w3;
}

// the unsigned product, then the two's complement correction of the high
// half: a negative operand contributes -2^64 times the other operand
static void mult64to128_signed(int64_t op1, int64_t op2, uint64_t* hi, uint64_t* lo)
{
    mult64to128((uint64_t)op1, (uint64_t)op2, hi, lo);
    if (op1 < 0) *hi -= (uint64_t)op2;
    if (op2 < 0) *hi -= (uint64_t)op1;
}

static void op_dmult(int reg1, int reg2)
{
    qreg a, b;
    op_64bitexpand();
    a = st.g[reg1];
    b = st.g[reg2];
    mult64to128_signed((int64_t)a.q, (int64_t)b.q, &st.mhi.q, &st.mlo.q);
    if (DUMP64)
    {
        print("(%08X) ", st.pc);
        print("dmult  %08X%08X*%08X%08X = %08X%08X %08X%08X\n",
            a.d2[1], a.d2[0], b.d2[1], b.d2[0],
            st.mhi.d2[1], st.mhi.d2[0],
            st.mlo.d2[1], st.mlo.d2[0]);
    }
}

static void op_dmultu(int reg1,int reg2)
{
    qreg a,b;
    op_64bitexpand();
    a=st.g[reg1];
    b=st.g[reg2];
    mult64to128((uint64_t)a.q, (uint64_t)b.q, &st.mhi.q, &st.mlo.q);
    if(DUMP64)
    {
        print("(%08X) ",st.pc);
        print("dmultu  %08X%08X*%08X%08X = %08X%08X %08X%08X\n",
            a.d2[1],a.d2[0],b.d2[1],b.d2[0],
            st.mhi.d2[1],st.mhi.d2[0],
            st.mlo.d2[1],st.mlo.d2[0]);
    }
}

static void op_ddivu(int reg1,int reg2)
{
    qreg a,b;
    op_64bitexpand();
    a=st.g[reg1];
    b=st.g[reg2];
    if(!b.q)
    { // as op_div: all ones, remainder the dividend
        st.mlo.q=~0ull;
        st.mhi.q=a.q;
    }
    else
    {
        st.mlo.q=(unsigned __int64)a.q/(unsigned __int64)b.q;
        st.mhi.q=(unsigned __int64)a.q%(unsigned __int64)b.q;
    }
    if(DUMP64)
    {
        print("(%08X) ",st.pc);
        print("ddivu  %08X%08X/%08X%08X = %08X%08X , %08X%08X\n",
            a.d2[1],a.d2[0],b.d2[1],b.d2[0],
            st.mhi.d2[1],st.mhi.d2[0],
            st.mlo.d2[1],st.mlo.d2[0]);
    }
}

static void op_ddiv(int reg1,int reg2)
{
    qreg a,b;
    op_64bitexpand();
    a=st.g[reg1];
    b=st.g[reg2];
    if(!b.q)
    { // as op_div: -1 (1 for a negative dividend), remainder the dividend
        st.mlo.q=((qint)a.q<0)?1:~0ull;
        st.mhi.q=a.q;
    }
    else if(a.q==0x8000000000000000ull && b.q==~0ull)
    { // INT64_MIN/-1 overflows (a host fault in C): the quotient wraps
        st.mlo.q=a.q;
        st.mhi.q=0;
    }
    else
    {
        st.mlo.q=(__int64)a.q/(__int64)b.q;
        st.mhi.q=(__int64)a.q%(__int64)b.q;
    }
    if(DUMP64)
    {
        print("(%08X) ",st.pc);
        print("ddiv   %08X%08X/%08X%08X = %08X%08X , %08X%08X\n",
            a.d2[1],a.d2[0],
            b.d2[1],b.d2[0],
            st.mhi.d2[1],st.mhi.d2[0],
            st.mlo.d2[1],st.mlo.d2[0]);
    }
}

void op_shift64(dword opcode,int type,int amount)
{
    dword *s,*d;
    dword t[2];

    if(amount==-1)
    {
        // from reg
        s=st.g[OP_RT(opcode)].d2;
        d=st.g[OP_RD(opcode)].d2;
        amount=st.g[OP_RS(opcode)].d;
    }
    else
    {
        s=st.g[OP_RT(opcode)].d2;
        d=st.g[OP_RD(opcode)].d2;
    }

    op_64bitexpand();
    t[0]=s[0];
    t[1]=s[1];
    switch(type)
    {
    case 0: // left
        {
            uint64_t v=((uint64_t)t[1]<<32)|t[0];
            v<<=(amount&63);
            t[0]=(dword)v;
            t[1]=(dword)(v>>32);
        }
        break;
    case 1: // right (logical)
        {
            uint64_t v=((uint64_t)t[1]<<32)|t[0];
            v>>=(amount&63);
            t[0]=(dword)v;
            t[1]=(dword)(v>>32);
        }
        break;
    case 2: // right arithmetic
        {
            int64_t v=(int64_t)(((uint64_t)t[1]<<32)|t[0]);
            v>>=(amount&63);
            t[0]=(dword)v;
            t[1]=(dword)((uint64_t)v>>32);
        }
        break;
    }
    if(DUMP64)
    {
        print("(%08X) ",st.pc);
        print("shift%i %08X%08X,%i = %08X%08X (s=%i d=%i)\n",
            type,s[1],s[0],amount,t[1],t[0],
            OP_RT(opcode),OP_RD(opcode));
    }
    d[0]=t[0];
    d[1]=t[1];
}

void addi64(dword *d,dword *a,dword *b)
{
    op_64bitexpand();
    {
        uint64_t x=(((uint64_t)a[1]<<32)|a[0])+(((uint64_t)b[1]<<32)|b[0]);
        d[0]=(dword)x;
        d[1]=(dword)(x>>32);
    }
    if(DUMP64)
    {
        print("(%08X) ",st.pc);
        print("addi   %08X%08X+%08X%08X = %08X%08X\n",
            a[1],a[0],b[1],b[0],d[1],d[0]);
    }
}

//----

__inline dword op_memaddr(dword opcode)
{
    dword a;
    a = OP_IMM(opcode);
    a = SIGNEXT16(a);
    a+= st.g[OP_RS(opcode)].d;
    return(a);
}

// Comparison operands. LLE registers are real 64-bit values; HLE's OS
// routines write only low words, so HLE compares the sign-extended low word
// as it always has (the same order for signed and unsigned tests).
static qint gpr_s(int r)
{
    return(st.lleos?(qint)st.g[r].q:(qint)(int)st.g[r].d);
}

static qword gpr_u(int r)
{
    return((qword)gpr_s(r));
}

// LLE: a misaligned load/store (mask = size-1) is an address error, with
// the address in BadVAddr; the access doesn't happen. 1 = exception taken.
// an address error: BadVAddr, and Context's BadVPN2 as for TLB misses
// (n64-systemtest "Context during AdEL exception")
static void op_adderror(qword a,int write)
{
    lle_badvaddr(a);
    lle_exception(write?EXC_ADES:EXC_ADEL);
}

static int op_misaligned(dword a,dword mask,int write)
{
    if(!st.lleos || !(a&mask)) return(0);
    op_adderror((qword)(qint)(int)a,write);
    return(1);
}

// LLE: in 32-bit addressing the 64-bit base+offset must be a sign-extended
// 32-bit value, else an address error with the full address in BadVAddr
// (n64-systemtest "LW/SW with address not sign extended")
// With Status.KX (64-bit kernel addressing) the unmapped window XKPHYS
// (0x8000... to 0xBFFF..., bits 58..32 zero) reaches physical memory: the
// access goes through KSEG1 for cache attribute 2 (uncached), else KSEG0,
// and *pa is replaced. The mapped 64-bit segments aren't emulated.
// Physical addresses are 32 bits; 0x20000000 and up has nothing behind it:
// physnone is set, loads read 0 and stores are dropped (n64-systemtest
// "(limits)" loads from 0x90000000_7FFFFFF8).
static int physnone;

// The segments each mode may use (n64-systemtest "Privilege: memory
// accesses", tlb64): user: (x)useg; supervisor: (x)suseg, xsseg, csseg
// (0xC0000000..0xDFFFFFFF); kernel: everything, with the 64-bit segments
// when KX. Mapped 64-bit addresses go through lle_tlb64. mask: the access's
// alignment mask, checked before the TLB (an unaligned address is AdEL).
static int cpu_mode(void) // 0 kernel, 1 supervisor, 2 user (EXL/ERL: kernel)
{
    dword s=st.mmu[12].d;
    if(s&6) return(0);
    return((s>>3)&3)==0?0:((s>>3)&3)==1?1:2;
}

// Status.RE in user mode (LLE): memory is seen little-endian, byte x of the
// CPU's view being byte x^7 of memory. Loads/stores XOR the address by 7, 6,
// 4 or 0 for 1, 2, 4 or 8 bytes; LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR become the
// big-endian operation at address^7; fetches use ^4. Applied after the
// address checks: BadVAddr and the TLB see the raw address (n64-systemtest
// endian_re).
static int cpu_re(void)
{
    dword s=st.mmu[12].d;
    return(st.lleos && (s&0x02000000) && (s&0x18)==0x10 && !(s&6));
}

static int re_xor(int bytes)
{
    if(bytes==0x18 || bytes==8 || bytes==-8) return(0);
    if(bytes>0x10) bytes-=0x10;
    if(bytes<0) bytes=-bytes;
    return(bytes==1?7:bytes==2?6:4);
}

static int op_checkaddr(qword a,int write,int *pa,dword mask);

static int op_badaddr64(dword opcode,int write,int *pa,dword mask)
{
    physnone=0;
    if(!st.lleos) return(0);
    return(op_checkaddr(st.g[OP_RS(opcode)].q+(qword)(qint)SIGNEXT16(OP_IMM(opcode)),
                        write,pa,mask));
}

// the checks of op_badaddr64 for a 64-bit address a (also instruction
// fetches from 64-bit PCs); *pa gets the 32-bit address to access for the
// 64-bit segments
static int op_checkaddr(qword a,int write,int *pa,dword mask)
{
    dword s;
    int   m,is64,r;
    physnone=0;
    s=st.mmu[12].d;
    m=cpu_mode();
    if(a==(qword)(qint)(int)(dword)a)
    { // sign-extended 32-bit: the usual path, if the mode may use it
        dword lo=(dword)a;
        if(m==0 || lo<0x80000000u) return(0);
        if(m==1 && lo>=0xC0000000u && lo<0xE0000000u) return(0);
        op_adderror(a,write);
        return(1);
    }
    is64=(m==0)?(s&0x80)!=0:(m==1)?(s&0x40)!=0:(s&0x20)!=0;
    r=(int)(a>>62);
    if(is64 && m==0 && r==2 && !((a>>32)&0x07ffffff))
    { // XKPHYS: unmapped, cache attribute in bits 61..59
        if((dword)a>=0x20000000)
        {
            physnone=1;
            *pa=(int)(0x80000000u|((dword)a&7)); // alignment checks only
            return(0);
        }
        *pa=(int)((dword)a|((((a>>59)&7)==2)?0xA0000000u:0x80000000u));
        return(0);
    }
    if(is64 && (r==0 || (r==1 && m<2) || (r==3 && m==0)) &&
       (a&0x3FFFFFFFFFFFFFFFull)<(r==3?0x000000FF80000000ull:0x0000010000000000ull))
    { // xuseg, xsseg, xkseg: mapped
        dword phys;
        if((dword)a&mask)
        {
            op_adderror(a,write);
            return(1);
        }
        if(lle_tlb64(a,write,&phys)) return(1);
        if(phys>=0x20000000)
        {
            physnone=1;
            *pa=(int)(0x80000000u|((dword)a&7));
            return(0);
        }
        // the KSEG0 (cached) or KSEG1 alias, for the cache model
        *pa=(int)(phys|(lle_tlb64cached?0x80000000u:0xA0000000u));
        return(0);
    }
    op_adderror(a,write);
    return(1);
}

// LLAddr for the LL/LLD at opcode: the physical address >> 4
static dword op_lladdr(dword opcode)
{
    qword a=st.g[OP_RS(opcode)].q+(qword)(qint)SIGNEXT16(OP_IMM(opcode));
    if(st.lleos && (a>>62)==2 && a!=(qword)(qint)(int)(dword)a)
        return((dword)a>>4); // XKPHYS
    if(st.lleos) return(lle_virt2phys(op_memaddr(opcode))>>4);
    return((op_memaddr(opcode)&0x1fffffff)>>4);
}

// an encoding the VR4300 doesn't define: the reserved instruction exception
// in LLE mode; HLE only reports it, as before
static void op_reserved(char *what)
{
    if(st.lleos)
    {
        st.mmu[13].d&=~0x30000000u; // CE 0: not a coprocessor instruction
        lle_exception(EXC_RI);
    }
    else error(what);
}

// COP2: the N64 has none, but with Status.CU2 set the moves work on one
// 64-bit latch: MTC2/DMTC2/CTC2 store the whole register, MFC2/CFC2 read
// it sign-extended from 32 bits, DMFC2 all of it; DCFC2/DCTC2 and the
// rest are reserved, with CE=2. CU2 clear: coprocessor unusable, CE=2.
// LWC2..SDC2 do nothing. (n64-systemtest cop_unusable)
static qword cop2latch;

static void op_cop2(dword opcode)
{
    int op=OP_OP(opcode);
    st.mmu[13].d=(st.mmu[13].d&~0x30000000u)|0x20000000u; // CE=2
    if(!(st.mmu[12].d&0x40000000))
    {
        lle_exception(EXC_CPU);
        return;
    }
    if(op!=COP2) return; // LWC2 LDC2 SWC2 SDC2
    switch(OP_RS(opcode))
    {
    case 0: case 2: // MFC2 CFC2
        st.g[OP_RT(opcode)].q=(qword)(qint)(int)(dword)cop2latch;
        break;
    case 1: // DMFC2
        st.g[OP_RT(opcode)].q=cop2latch;
        break;
    case 4: case 5: case 6: // MTC2 DMTC2 CTC2
        cop2latch=st.g[OP_RT(opcode)].q;
        break;
    default:
        lle_exception(EXC_RI);
        break;
    }
}

// CPU memory accesses: through the caches in LLE mode (cpucache.c)
#define CRD8(a)     (st.lleos?cache_read8(a):mem_vread8(a))
#define CRD16(a)    (st.lleos?cache_read16(a):mem_vread16(a))
#define CRD32(a)    (st.lleos?cache_read32(a):mem_vread32(a))
#define CWR8(a,v)   { if(st.lleos) cache_write8(a,v); else mem_vwrite8(a,v); }
#define CWR16(a,v)  { if(st.lleos) cache_write16(a,v); else mem_vwrite16(a,v); }
#define CWR32(a,v)  { if(st.lleos) cache_write32(a,v); else mem_vwrite32(a,v); }

// CACHE (LLE): the address must be word aligned, else AdEL (n64-systemtest
// "Unaligned CACHE ... address"); the operation itself is in the cache model
static void op_cache(dword opcode)
{
    int a=(int)op_memaddr(opcode);
    if(op_badaddr64(opcode,0,&a,3) || op_misaligned(a,3,0)) return;
    if(physnone || LLE_TLBMISS(a,0)) return;
    cache_op(OP_RT(opcode),(dword)a);
}

// LLE: a 32-bit ADD/ADDI/SUB result outside int32 raises the overflow
// exception and leaves the destination alone. 1 = exception taken.
static int op_overflow32(qint x)
{
    if(!st.lleos || x==(qint)(int)x) return(0);
    lle_exception(EXC_OV);
    return(1);
}

// the same for DADD/DADDI/DSUB: x=a+b (or a-b), overflow when the operands
// have the same sign (different for subtraction) and the result's differs
static int op_overflow64(qword a,qword b,qword x,int sub)
{
    qword ov=sub?((a^b)&(a^x)):(~(a^b)&(a^x));
    if(!st.lleos || !(ov>>63)) return(0);
    lle_exception(EXC_OV);
    return(1);
}

// LL/SC reservation (LLbit): set by LL/LLD, cleared by ERET; SC/SCD store
// only while it is set. One CPU, so nothing else breaks the link.
int llbit; // (tests/cpu_stubs.c t_reset clears it)

// PI write latch: a CPU store to cartridge space (KSEG0/1 of 10000000..
// 1FBFFFFF) doesn't change the ROM, but the next CPU read there returns the
// stored value once (Project64 m_RomWrittenTo, Mupen64Plus last_write). A
// Bug's Life relocates its wavetable header in ROM; the latched values end
// the relocation loop, without them it walks ~1.3G entries past ROM end.
// Details from n64-systemtest (cart_memory/write.rs): only the first store
// counts; SB/SH put the register on their byte lanes (0x..56BA stored with
// SB at offset 1 latches 0x56BA0000); SD latches its upper word; LB/LH read
// the top bits; the value is gone after one read or ~70 loop iterations
// (~200 instructions), and PI_STATUS.IOBUSY is set while it lasts.
static int   cartlatched;
static dword cartlatch;
static qword cartlatchat;         // icount when the store happened
static qword icount;              // instructions executed (c_execop)
#define CARTLATCH_LIFE 200

static void cartlatch_clear(void)
{
    cartlatched=0;
    RPI[4]&=~2u; // PI_STATUS.IOBUSY
}

// PIF RAM: the last 64 bytes of the PIF's 2 KB (1FC007C0..1FC007FF)
static int ispifram(dword a)
{
    return(((dword)a&0x1fffffc0)==0x1fc007c0);
}

static int iscartaddr(dword a)
{
    dword phys=a&0x1fffffff;
    if(st.lleos && phys>=LLE_ISVIEWER && phys<LLE_ISVIEWER+0x10000) return 0;
    return (a&0xC0000000)==0x80000000 && phys>=0x10000000 && phys<0x1FC00000;
}

// alignment mask of an op_readmem/op_writemem size code
static dword op_sizemask(int bytes)
{
    if(bytes==0x18) return(7);
    if(bytes>0x10) bytes-=0x10;
    if(bytes<0) bytes=-bytes;
    return((dword)bytes-1);
}

static void op_readmem(dword opcode,int bytes)
{
    int a,x,*d;
    a=op_memaddr(opcode);
    if(op_badaddr64(opcode,0,&a,op_sizemask(bytes)) || op_misaligned(a,op_sizemask(bytes),0)) return;
    if(physnone)
    {
        if(bytes==0x18) fpr_set64(OP_RT(opcode),0);
        else if(bytes>0x10) st.f[OP_RT(opcode)].d=0;
        else st.g[OP_RT(opcode)].q=0;
        return;
    }
    if(cpu_re())
    {
        if(LLE_TLBMISS(a,0)) return; // on the raw address
        a^=re_xor(bytes);
    }
    if(cartlatched && icount-cartlatchat>CARTLATCH_LIFE) cartlatch_clear();
    if(bytes==0x18)
    { // LDC1: one 64-bit FPU register (or pair)
        dword hi,lo;
        if(LLE_TLBMISS(a,0)) return;
        cpu_notify_readmem(a,8);
        hi=CRD32(a);
        lo=CRD32(a+4);
        fpr_set64(OP_RT(opcode),((qword)hi<<32)|lo);
        return;
    }
    if(bytes>0x10)
    { // fpu
        d =&st.f[OP_RT(opcode)].d;
        bytes-=0x10;
    }
    else
    {
        d =&st.g[OP_RT(opcode)].d;
    }

    if(LLE_TLBMISS(a,0)) return;
    cpu_notify_readmem(a,bytes);

    if(iscartaddr(a))
    {
        if(cartlatched && bytes>=-4 && bytes<=4)
        { // the latched store, once; narrower reads get its top bits
            dword v=cartlatch;
            cartlatch_clear();
            switch(bytes)
            {
            case -1: d[0]=SIGNEXT8(v>>24); break;
            case  1: d[0]=v>>24; break;
            case -2: d[0]=SIGNEXT16(v>>16); break;
            case  2: d[0]=v>>16; break;
            default: d[0]=v; break;
            }
            if(d>=&st.g[0].d && d<=&st.g[31].d) d[1]=(bytes>0 && bytes<4)?0:(int)d[0]>>31;
            return;
        }
        // LB/LH from the cart bus see only every other halfword: a halfword
        // read returns the one at (a+2)&~3, a byte read the byte there plus
        // a&1 (n64-systemtest "cart: Read16/Read8")
        if(bytes==2 || bytes==-2) a=(a+2)&~3;
        else if(bytes==1 || bytes==-1) a=((a+2)&~3)+(a&1);
    }

    switch(bytes)
    {
    case -1:
        x=CRD8(a);
        d[0]=SIGNEXT8(x);
        break;
    case 1:
        x=CRD8(a);
        d[0]=x;
        break;
    case -2:
        x=CRD16(a);
        d[0]=SIGNEXT16(x);
        break;
    case 2:
        x=CRD16(a);
        d[0]=x;
        break;
    case -4:
    case 4:
        if(st.lleos && ((dword)a&0xC0000000)==0x80000000 &&
           ((dword)a&0x1ff00000)==0x04100000) lle_hwpreread((dword)a&0x1ffffffc);
        x=CRD32(a);
        d[0]=x;
        // LLE: registers that change when read (KSEG0/1 of 0x04000000..)
        if(st.lleos && ((dword)a&0xC0000000)==0x80000000 &&
           ((dword)a&0x1ff00000)==0x04000000) lle_hwread((dword)a&0x1ffffffc);
        break;
    case 8:
    case -8:
        d[1]=CRD32(a);
        d[0]=CRD32(a+4);
        return;
    }
    // GPR loads fill all 64 bits: LB/LH/LW sign-extend, LBU/LHU zero-extend
    // (LWU clears the high word in op_main)
    if(d>=&st.g[0].d && d<=&st.g[31].d) d[1]=(bytes>0 && bytes<4)?0:(int)d[0]>>31;
}

// the top n bytes of v, big-endian, at physical RDRAM address a
static void mirep_put(dword a,qword v,int n)
{
    int i;
    for(i=0;i<n;i++) mem_vwrite8(0xA0000000|(a+i),(int)(byte)(v>>(56-8*i)));
}

// MI repeat mode (ares MI::writeRdramRepeat): an uncached store to RDRAM
// writes the value (rotated onto its lanes, doubled to 64 bits) repeatedly
// from its address to the 8-byte-aligned address + length, wrapping within
// 2 KB. size: 1, 2, 4 or 8 bytes; v the register value.
static void op_mirepeat(dword a,int size,qword v,int length)
{
    dword end;
    if(size==1)
    {
        v&=0xFFFFFFFFull>>(24-(a&3)*8);
        v=(dword)((v<<24)|(v>>8));
    }
    else if(size==2)
    {
        v&=0xFFFFFFFFull>>(16-(a&2)*8);
        v=(dword)((v<<16)|(v>>16));
    }
    if(size!=8) v=(v<<32)|(dword)v;

    end=(a&~7u)+length;
    if(end>(dword)mem.ramsize) end=mem.ramsize;
    if(end<=a) return;
    length=(int)(end-a);

    if(a&1)
    {
        mirep_put(a,v,1); v=(v<<8)|(v>>56);
        a=(a&~0x7FFu)|((a+1)&0x7FF); length-=1;
    }
    if((a&2) && length>=2)
    {
        mirep_put(a,v,2); v=(v<<16)|(v>>48);
        a=(a&~0x7FFu)|((a+2)&0x7FF); length-=2;
    }
    if((a&4) && length>=4)
    {
        mirep_put(a,v,4); v=(v<<32)|(v>>32);
        a=(a&~0x7FFu)|((a+4)&0x7FF); length-=4;
    }
    while(length>=8)
    {
        mirep_put(a,v,8);
        a=(a&~0x7FFu)|((a+8)&0x7FF); length-=8;
    }
    if(length>=4) { mirep_put(a,v,4); v<<=32; a+=4; length-=4; }
    if(length>=2) { mirep_put(a,v,2); v<<=16; a+=2; length-=2; }
    if(length==1) mirep_put(a,v,1);
}

static void op_writemem(dword opcode,int bytes)
{
    int a,*d;
    dword pair[2];
    a=op_memaddr(opcode);
    if(op_badaddr64(opcode,1,&a,op_sizemask(bytes)) || op_misaligned(a,op_sizemask(bytes),1)) return;
    if(physnone) return;
    if(bytes==0x18)
    { // SDC1: one 64-bit FPU register (or pair)
        qword v=fpr_get64(OP_RT(opcode));
        pair[0]=(dword)v;
        pair[1]=(dword)(v>>32);
        d=(int *)pair;
        bytes=8;
    }
    else if(bytes>0x10)
    { // fpu
        d =&st.f[OP_RT(opcode)].d;
        bytes-=0x10;
    }
    else
    {
        d =&st.g[OP_RT(opcode)].d;
    }

    if(LLE_TLBMISS(a,1)) return;
    if(cpu_re()) a^=re_xor(bytes); // Status.RE (cpu_re)
    // MI repeat mode: armed by MI_MODE, taken by the next uncached store to
    // RDRAM (n64-systemtest "MI Repeat")
    if(st.lleos && ((dword)a&0xE0000000)==0xA0000000 &&
       ((dword)a&0x1fffffff)<0x03F00000 && bytes<=8)
    {
        int n=lle_mirepeat();
        if(n)
        {
            qword v=(bytes==8)?(((qword)(dword)d[1]<<32)|(dword)d[0]):(dword)d[0];
            op_mirepeat((dword)a&0x1fffffff,bytes,v,n);
            cpu_notify_writemem(a&~7,8);
            return;
        }
    }
    // SP memory is written a word at a time: SB/SH store the register
    // shifted onto its byte lanes over the whole word, SD its upper word
    // (n64-systemtest "spmem: SB/SH/SD", the same bus rule as the cart latch)
    // PIF RAM likewise (n64-systemtest "pifram: SB/SH")
    if(st.lleos && bytes!=4 && ((dword)a&0xC0000000)==0x80000000 &&
       (((dword)a&0x1fffe000)==0x04000000 || ispifram(a)))
    {
        switch(bytes)
        {
        case 1:  pair[0]=(dword)d[0]<<(8*(3-(a&3))); break;
        case 2:  pair[0]=(dword)d[0]<<(8*(2-(a&2))); break;
        default: pair[0]=d[1]; break;
        }
        d=(int *)pair;
        a&=~3;
        bytes=4;
    }
    switch(bytes)
    {
    case 1:
        CWR8(a,d[0])
        break;
    case 2:
        CWR16(a,d[0])
        break;
    case 4:
        CWR32(a,d[0])
        break;
    case 8:
        CWR32(a,d[1])
        CWR32(a+4,d[0])
        break;
    }

    cpu_notify_writemem(a,bytes);

    // PIF RAM is mapped write-then-read (mem.c): the CPU's store must reach
    // the side that loads and SI DMA read
    if(st.lleos && ((dword)a&0xC0000000)==0x80000000 && ispifram(a) && bytes==4)
        RPIF[((dword)a&0x7ff)>>2]=d[0];

    if(iscartaddr(a))
    {
        if(cartlatched && icount-cartlatchat>CARTLATCH_LIFE) cartlatch_clear();
        if(!cartlatched)
        { // the first store only, on its byte lanes
            switch(bytes)
            {
            case 1:  cartlatch=(dword)d[0]<<(8*(3-(a&3))); break;
            case 2:  cartlatch=(dword)d[0]<<(8*(2-(a&2))); break;
            case 8:  cartlatch=d[1]; break;
            default: cartlatch=d[0]; break;
            }
            cartlatched=1;
            cartlatchat=icount;
            RPI[4]|=2u; // PI_STATUS.IOBUSY
        }
    }

    // LLE OS mode: hardware registers react to the store immediately
    // (KSEG0/1 address of 0x04040000..0x04FFFFFF)
    if(st.lleos && ((dword)a&0xC0000000)==0x80000000)
    {
        dword phys=(dword)a&0x1fffffff;
        if(phys>=0x04040000 && phys<0x05000000) lle_hwwrite(phys&~3u);
        else if(flash_contains(phys)) flash_hwwrite(phys&~3u);
        else if((phys&~3u)==LLE_ISVIEWER+0x14) lle_isviewer();
    }
}

static void op_rwmemrl(dword opcode,int write,int right)
{
    dword x,y,a,s,m;
    int   pa;
    pa=(int)op_memaddr(opcode);
    if(op_badaddr64(opcode,write,&pa,0)) return;
    a=(dword)pa;
    if(!physnone && LLE_TLBMISS(a,write)) return;
    if(cpu_re()) a^=7; // Status.RE: the big-endian operation at address^7
    s=a&3;
    a&=~3;
    if(write)
    {
        if(physnone) return;
        x=CRD32(a);
        y=st.g[OP_RT(opcode)].d;
        if(right)
        {
            m=0x00ffffff >> (s*8);
            y<<=(3-s)*8;
            x&=m;
            x|=y;
        }
        else
        {
            m=0xffffff00 << ((3-s)*8);
            y>>=s*8;
            x&=m;
            x|=y;
        }
        CWR32(a,x)
        cpu_notify_writemem(a,4);
    }
    else
    {
        x=st.g[OP_RT(opcode)].d;
        y=physnone?0:CRD32(a);
        if(right)
        {
            m=0xffffff00 << (s*8);
            y>>=(3-s)*8;
            x&=m;
            x|=y;
        }
        else
        {
            m=0x00ffffff >> ((3-s)*8);
            y<<=s*8;
            x&=m;
            x|=y;
        }
        // the result is sign-extended when all four bytes were loaded (LWL
        // always); LWR of fewer bytes keeps the high word (n64-systemtest
        // "LWR result": 0xBEEF0000_010203BA)
        if(OP_RT(opcode))
        {
            st.g[OP_RT(opcode)].d=x;
            if(!right || s==3) st.g[OP_RT(opcode)].d2[1]=(int)x>>31;
        }
    }
}
// LDL/LDR/SDL/SDR: unaligned doubleword access (big-endian, like LWL/LWR
// above but 64-bit). Registers hold the high word in d2[1], low in d2[0].
static void op_rwmemrl64(dword opcode,int write,int right)
{
    dword    a;
    int      shift,pa;
    uint64_t m64,reg;
    dword   *r=st.g[OP_RT(opcode)].d2;

    op_64bitexpand();
    pa=(int)op_memaddr(opcode);
    if(op_badaddr64(opcode,write,&pa,0)) return;
    a=(dword)pa;
    if(physnone && write) return;
    if(!physnone && LLE_TLBMISS(a,write)) return;
    if(cpu_re()) a^=7; // Status.RE: the big-endian operation at address^7
    shift=right?(7-(a&7))*8:(a&7)*8;
    a&=~7;
    m64=physnone?0:((uint64_t)CRD32(a)<<32)|CRD32(a+4);
    reg=((uint64_t)r[1]<<32)|r[0];
    if(write)
    {
        if(right) m64=(m64&(((uint64_t)1<<shift)-1))|(reg<<shift);
        else      m64=(m64&~(~(uint64_t)0>>shift))|(reg>>shift);
        CWR32(a,(dword)(m64>>32))
        CWR32(a+4,(dword)m64)
        cpu_notify_writemem(a,8);
    }
    else
    {
        cpu_notify_readmem(a,8);
        if(right) reg=(reg&~(~(uint64_t)0>>shift))|(m64>>shift);
        else      reg=(reg&(((uint64_t)1<<shift)-1))|(m64<<shift);
        r[0]=(dword)reg;
        r[1]=(dword)(reg>>32);
    }
}

// DSUB/DSUBU (DSUB's overflow check is in op_main)
static void op_sub64(int *d,int *a,int *b)
{
    uint64_t x,y;
    op_64bitexpand();
    x=((uint64_t)(dword)a[1]<<32)|(dword)a[0];
    y=((uint64_t)(dword)b[1]<<32)|(dword)b[0];
    x-=y;
    d[0]=(dword)x;
    d[1]=(dword)(x>>32);
}

// TEQ/TNE/TGE/... : only a taken trap is an error (compilers emit
// e.g. "teq divisor,zero" after divides, which normally never fires)
static void op_trap(int taken)
{
    if(!taken) return;
    if(st.lleos) lle_exception(EXC_TR);
    else exception("opcode trap");
}

static void op_mult(int a,int b)
{
    int lo,hi;
    {
        int64_t r=(int64_t)a*(int64_t)b;
        lo=(int)(dword)r;
        hi=(int)(dword)((uint64_t)r>>32);
    }
    st.mlo.d=lo;
    st.mhi.d=hi;
}

static void op_multu(int a,int b)
{
    int lo,hi;
    {
        uint64_t r=(uint64_t)(dword)a*(uint64_t)(dword)b;
        lo=(int)(dword)r;
        hi=(int)(dword)(r>>32);
    }
    st.mlo.d=lo;
    st.mhi.d=hi;
}

// Division by zero doesn't trap: the quotient is -1 (1 for a negative
// dividend; all ones for DIVU/DDIVU) and the remainder the dividend
// (n64-systemtest DIV/DIVU/DDIV/DDIVU)
static void op_div(int a,int b)
{
    int lo,hi;
    if(!b)
    {
        st.mlo.d=(a<0)?1:-1;
        st.mhi.d=a;
        return;
    }
    if(a==(int)0x80000000 && b==-1)
    { // MIPS gives lo=INT_MIN, hi=0; C would trap
        lo=a;
        hi=0;
    }
    else
    {
        lo=a/b;
        hi=a%b;
    }
    st.mlo.d=lo;
    st.mhi.d=hi;
}

static void op_divu(int a,int b)
{
    int lo,hi;
    if(!b)
    {
        st.mlo.d=-1;
        st.mhi.d=a;
        return;
    }
    lo=(int)((dword)a/(dword)b);
    hi=(int)((dword)a%(dword)b);
    st.mlo.d=lo;
    st.mhi.d=hi;
}

// the return address (after the delay slot) into register r, sign-extended
// LLE branch model (ares Pipeline): while an instruction runs, lle_pcnext is
// the address of the next one to run (the pending target in a delay slot).
// Links are lle_pcnext+4, branch targets lle_pcnext+offset, and J keeps its
// top bits. A branch only records itself (lle_br); c_execop then makes the
// next instruction its delay slot, taken or not. So a jump inside a delay
// slot runs the first jump's target as its own delay slot (n64-systemtest
// jumps: "BGEZAL: Within delay slot of J", "J: Within delay slot of another
// J"), and an exception in the slot of a not-taken branch reports BD.
// In 64-bit addressing the PC can leave the sign-extended 32-bit space
// (code in xkseg, n64-systemtest tlb cross_page_exec "64-bit VA"): st.pc
// keeps the low word, cpu_pchi the high one while cpu_pc64 is set.
// lle_next64/lle_brto64 are lle_pcnext/lle_brto in 64 bits.
dword cpu_pchi;
int   cpu_pc64;
static dword lle_pcnext;
static int   lle_br;     // 1 taken, 2 not taken, 3 likely not taken (skip)
static dword lle_brto;
static qword lle_next64,lle_brto64;
static int   lle_bt64;   // st.branchto is a 64-bit PC, high word lle_bthi
static dword lle_bthi;

static qword sext32(dword v) { return((qword)(qint)(int)v); }

// the current PC in 64 bits
qword cpu_pc64get(void)
{
    return(cpu_pc64?(((qword)cpu_pchi<<32)|st.pc):sext32(st.pc));
}

// set the PC from a 64-bit address
static void cpu_pc64set(qword v)
{
    st.pc=(dword)v;
    cpu_pc64=(v!=sext32((dword)v));
    cpu_pchi=(dword)(v>>32);
}

static void op_link(int r)
{
    if(!r) return;
    if(st.lleos) st.g[r].q=lle_next64+4;
    else         st.g[r].q=(qword)(qint)(int)(st.pc+8);
}

// J/JAL (reg=-1): the target keeps the top 4 bits of the delay slot's
// address (PC+4), so a jump in the last slot of a 256 MB region leaves it.
// JR/JALR: the register, read before JALR writes its link register RD.
static void op_jump(dword opcode,int link,int reg)
{
    int   to;
    qword jumpreg64=0;

    st.branchtype=BRANCH_NORMAL;

    if(reg==-1)
    {
        to=((OP_TAR(opcode)<<2)&0x0fffffff)
          |((st.lleos?lle_pcnext:st.pc+4)&0xf0000000);
    }
    else
    {
        to=st.g[reg].d;
        jumpreg64=st.g[reg].q;
        if(reg==31)
        {
            st.branchtype=BRANCH_RET;
            st.expanded64bit=0;
        }
    }

    if(link)
    { // JAL links r31, JALR its RD field (r0: no link)
        st.branchtype=BRANCH_CALL;
        op_link(reg==-1?31:OP_RD(opcode));
    }

    if(st.lleos)
    {
        lle_br=1;
        lle_brto=to;
        // 64 bits: the register (read before JALR's link), or J's region
        // of the next PC
        if(reg==-1) lle_brto64=(lle_next64&~0x0FFFFFFFull)|((dword)to&0x0FFFFFFF);
        else        lle_brto64=jumpreg64;
        return;
    }
    st.branchdelay=2;
    st.branchto=to;
}

// the linking branches write r31 whether or not they branch (the caller
// has already evaluated the condition, so BLTZAL r31 tests the old value)
static void op_branch(dword opcode,int doit,int likely,int link)
{
    if(link) op_link(31);
    if(st.lleos)
    {
        if(doit)
        {
            st.branchtype=link?BRANCH_CALL:BRANCH_NORMAL;
            lle_br=1;
            lle_brto=lle_pcnext+(SIGNEXT16(OP_IMM(opcode))<<2);
            lle_brto64=lle_next64+(qword)(qint)(SIGNEXT16(OP_IMM(opcode))<<2);
        }
        else lle_br=likely?3:2;
        return;
    }
    if(doit)
    {
        int imm=SIGNEXT16(OP_IMM(opcode));
        st.branchtype=link?BRANCH_CALL:BRANCH_NORMAL;
        imm<<=2;
        imm+=st.pc+4;
        st.branchdelay=2;
        st.branchto=imm;
    }
    else
    {
        if(likely)
        {
            st.pc+=4; // likely, skip delay slot instruction
        }
    }
}

double readdouble(int reg)
{
    qword  q=fpr_get64(reg);
    double d;
    memcpy(&d,&q,8);
    return(d);
}

void writedouble(int reg,double value)
{
    qword q;
    memcpy(&q,&value,8);
    fpr_set64(reg,q);
}

static void op_scc( dword opcode )
{
#define  MF           0  // Move From Coprocessor (COPx Sub OpCode)
#define  MT           4  // Move to Coprocessor (COPx Sub OpCode)
//   print( "at %08X cop0 (0x%08X) Rs (0x%02X) Func (0x%02X)\n",
//          st.pc, opcode, OP_RS(opcode), OP_FUNC(opcode));

   if(st.lleos)
   { // LLE OS mode: real COP0 (Count, Compare, exceptions, TLB); lle.c
      switch( OP_RS(opcode) )
      {
      case 0: // MFC0: the low word, sign-extended
         st.g[OP_RT(opcode)].q=(qword)(qint)(int)(dword)lle_dmfc0(OP_RD(opcode));
         return;
      case 1: // DMFC0: 64-bit registers whole, the others zero-extended
         st.g[OP_RT(opcode)].q=lle_dmfc0(OP_RD(opcode));
         return;
      case 4: // MTC0: the whole 64-bit register, as DMTC0 (ares; n64-systemtest
         // "Context (sign extension)", "ExceptPC (no masking)", the latch)
         lle_dmtc0(OP_RD(opcode),st.g[OP_RT(opcode)].q);
         return;
      case 5: // DMTC0
         lle_dmtc0(OP_RD(opcode),st.g[OP_RT(opcode)].q);
         return;
      }
      if(OP_RS(opcode)&16)
      { // CO (bit 25 set)
         switch(OP_FUNC(opcode))
         {
         case 1:  lle_tlbr();  break;
         case 2:  lle_tlbwi(); break;
         case 6:  lle_tlbwr(); break;
         case 8:  lle_tlbp();  break;
         case 24: llbit=0; lle_eret(); break; // ERET clears LLbit
         case 16: op_reserved("COP0 function 0x10"); break; // whatever the operand bits
         }
         return;
      }
      // rs 3, 7 and 9..15 are reserved (n64-systemtest "Reserved
      // integer/COP0 encodings raise RI"). The rest (CFC0, CTC0, BC0, CO
      // functions other than the above) do nothing, as in Mupen64Plus:
      // n64-systemtest runs a CO function 0x20 at boot, before it has any
      // exception handler
      if(OP_RS(opcode)==3 || OP_RS(opcode)==7 || OP_RS(opcode)>=9)
         op_reserved("reserved COP0 encoding");
      return;
   }

   switch( OP_RS(opcode) )
   {
      case MF:

//         print( "at %08X read mmu %s\n",st.pc,mmuregnames[OP_RD(opcode)]);
         st.g[OP_RT(opcode)].d = st.mmu[OP_RD(opcode)].d;

         break;

      case MT:

         // If write to the Compare Register, Clear the Timer Interrupt
         // in the Cause Register

         if( OP_RD(opcode) == 11 )
            st.mmu[13].d &= ~0x00008000;


         // If write to the Cause Register, mask out all bits except
         // IP0 & IP1

         if( OP_RD(opcode) == 13 )
         {
            st.mmu[13].d = st.g[OP_RT(opcode)].d & 0x00000300;
            return;
         }

         st.mmu[OP_RD(opcode)].d = st.g[OP_RT(opcode)].d;
         logh("TLB regwrite at %08X: %s=%08X\n",
            st.pc,
            mmuregnames[OP_RD(opcode)],
            st.g[OP_RT(opcode)].d);

         // Later Implementation:
         // If Status Register written to and that write included any
         // interrupt bits then check for CPU Interrupts.

         break;

      case 16:
         switch(OP_FUNC(opcode))
         {
          case 2: // TLBWI - write index
          case 6: // TLBWR - write random
             {
                 int mask,size;
                 int virt[2],phys[2];

                 mask=st.mmu[5].d;
                 virt[0]=st.mmu[10].d&~255;
                 phys[0]=st.mmu[2].d&~63;
                 phys[1]=st.mmu[3].d&~63;
                 size=mask/2+4096;
                 virt[1]=virt[0]+size;

                 logh("TLB at %08X: ind:%i lo0:%08X lo1:%08X hi:%08X mask:%08X\n",
                    st.pc,
                    st.mmu[0].d,
                    st.mmu[2].d,
                    st.mmu[3].d,
                    st.mmu[10].d,
                    st.mmu[5].d);
                 logh("TLB %08X,%08X = %08X,%08X size %08X\n",
                    virt[0],virt[1],phys[0],phys[1],size);

                 osMapMem(virt[0],phys[0],size);
                 osMapMem(virt[1],phys[1],size);

                 /*
                 if(st.dmatransfers<0x7fff0000)
                 {
                     st.dmatransfers++;
                     inifile_patches(st.dmatransfers);
                 }
                 */
             } break;
         }
         break;

      default:

         st2.cpuerrorcnt++; if(st2.cpuerrorcnt>100) break;
         error( "RM - Unimplemented COP0-MMU Opcode" );
         print( "RM - Opcode (0x%08X) Rs (0x%02X) Func (0x%02X)\n", opcode, OP_RS(opcode), OP_FUNC(opcode));

         break;
   }
}

/****************************************************************************
** FPU control: FCR0 reads the implementation number, FCR31 holds the
** rounding mode (bits 0-1), flags (2-6), enables (7-11), cause (12-17), the
** condition bit (23, kept in st.fputrue) and FS (24).
*/

#define FCR0_VALUE   0x00000A00 // VR4300 FPU implementation 0x0A, revision 0 (n64-systemtest, ares)
#define FCR31_MASK   0x0183ffff // writable bits
#define FCR31_C      0x00800000
#define FCR31_CAUSE  0x0003f000
#define FCR31_V      0x00010040 // invalid operation: cause and flag
#define FCR31_EV     0x00000800 // invalid operation enable
#define FCR31_Z      0x00008020 // division by zero: cause and flag
#define FCR31_EZ     0x00000400 // division by zero enable
#define FCR31_FS     0x01000000 // flush denormalized results to zero

static dword fcr31_get(void)
{
    return((st.fcr31&~FCR31_C)|(st.fputrue?FCR31_C:0));
}

// CTC1: a cause bit whose exception is enabled (Unimplemented always is)
// raises the floating-point exception right away
static void fcr31_set(dword v)
{
    st.fcr31=v&FCR31_MASK;
    st.fputrue=(v&FCR31_C)!=0;
    if(st.lleos && ((st.fcr31>>12)&(((st.fcr31>>7)&0x1f)|0x20))) lle_exception(EXC_FPE);
}

// an invalid operation (NaN operand of a signalling compare, a conversion
// out of the integer range): flag and cause V, and the exception when
// enabled (LLE). 1 = exception taken, the destination is not written.
static int fpu_invalid(void)
{
    st.fcr31|=FCR31_V;
    if(st.lleos && (st.fcr31&FCR31_EV))
    {
        lle_exception(EXC_FPE);
        return(1);
    }
    return(0);
}

// an FPU encoding the VR4300 doesn't implement (reserved formats and
// functions, CVT.S.S, DCFC1/DCTC1...): cause E alone, and the
// floating-point exception, which can't be masked (n64-systemtest
// "Reserved COP1 encodings raise FPE"). HLE only reports it, as before.
static void fpu_unimplemented(char *what)
{
    if(!st.lleos)
    {
        st2.cpuerrorcnt++;
        if(st2.cpuerrorcnt<=100) error(what);
        return;
    }
    st.fcr31=(st.fcr31&~FCR31_CAUSE)|0x00020000u;
    lle_exception(EXC_FPE);
}

// division of a finite nonzero value by zero: flag and cause Z, and the
// exception when enabled (LLE). 1 = exception taken, nothing stored.
static int fpu_divzero(void)
{
    st.fcr31|=FCR31_Z;
    if(st.lleos && (st.fcr31&FCR31_EZ))
    {
        lle_exception(EXC_FPE);
        return(1);
    }
    return(0);
}

// the host rounding mode for FCSR.RM (nearest, zero, up, down)
static const int fpu_hostrm[4]={FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD};

// round to an integral value: rm 0 = nearest (ties to even), 1 = toward
// zero, 2 = up, 3 = down. Done explicitly, whatever the host's mode is.
static double fpu_round(double v,int rm)
{
    double f;
    switch(rm&3)
    {
    case 1: return(trunc(v));
    case 2: return(ceil(v));
    case 3: return(floor(v));
    }
    f=floor(v);
    if(v-f>0.5) return(f+1);
    if(v-f<0.5) return(f);
    return(fmod(f,2.0)==0?f:f+1);
}

// ROUND/TRUNC/CEIL/FLOOR/CVT to a 32- or 64-bit integer. NaN, infinity or
// a result out of range is invalid: 2^31-1 (2^63-1) unless trapped.
// 0 = trapped, nothing to store.
static int fpu_toint(double a,int rm,int bits,qword *out)
{
    double v=fpu_round(a,rm);
    double lim=(bits==32)?2147483648.0:9223372036854775808.0;
    if(v!=v || v>=lim || v<-lim)
    {
        if(fpu_invalid()) return(0);
        *out=(bits==32)?0x7fffffffull:0x7fffffffffffffffull;
        return(1);
    }
    *out=(qword)(qint)v;
    return(1);
}

/****************************************************************************
** LLE FPU arithmetic, transcribed from ares (ares/n64/cpu/interpreter-fpu.cpp
** and algorithms.cpp, commit 4cb8d92b) for n64-systemtest cop1:
** - singles are computed in single precision on the host; the host's
**   exception flags (fetestexcept) become FCSR causes and flags
** - MIPS legacy NaNs: mantissa MSB set = signalling. A signalling NaN operand
**   is an invalid operation; a quiet NaN or denormal operand is an
**   unimplemented operation (cause E, always traps)
** - NaN results are the default NaN 0x7fbfffff / 0x7ff7ffffffffffff; a
**   denormal result is unimplemented unless FS is set (and underflow and
**   inexact are disabled), then it is flushed
** - conversions to integers of NaN, infinity, denormals or out-of-range
**   values are unimplemented
** HLE mode keeps the older, lenient code in op_fpu.
*/
#include <immintrin.h>

#define FPE_I 0 // FCSR bit offsets within flags (2), enables (7), causes (12)
#define FPE_U 1
#define FPE_O 2
#define FPE_Z 3
#define FPE_V 4

// cause e; 1 when its exception is enabled (the flag is then not set)
static int fpe_cause(int e)
{
    st.fcr31|=1u<<(12+e);
    if(st.fcr31&(1u<<(7+e))) return(1);
    st.fcr31|=1u<<(2+e);
    return(0);
}

static int fpe_unimpl(void)
{
    st.fcr31|=0x00020000u;
    return(1);
}

static int fpe_trap(void)
{
    lle_exception(EXC_FPE);
    return(1);
}

// after a host operation: 1 = stop (exception taken, or unimplemented)
static int fpe_host(int cvt)
{
    int exc=fetestexcept(FE_DIVBYZERO|FE_INEXACT|FE_UNDERFLOW|FE_OVERFLOW|FE_INVALID);
    int raise=0;
    if(!exc) return(0);
    if(cvt && (exc&FE_INVALID))
    {
        fpe_unimpl();
        return(fpe_trap());
    }
    if((exc&FE_UNDERFLOW) &&
       (!(st.fcr31&FCR31_FS) || (st.fcr31&(1u<<(7+FPE_U))) || (st.fcr31&(1u<<(7+FPE_I)))))
    {
        fpe_unimpl();
        return(fpe_trap());
    }
    if(exc&FE_DIVBYZERO) raise|=fpe_cause(FPE_Z);
    if(exc&FE_INEXACT)   raise|=fpe_cause(FPE_I);
    if(exc&FE_UNDERFLOW) raise|=fpe_cause(FPE_U);
    if(exc&FE_OVERFLOW)  raise|=fpe_cause(FPE_O);
    if(exc&FE_INVALID)   raise|=fpe_cause(FPE_V);
    if(raise) return(fpe_trap());
    return(0);
}

static dword f32bits(float f)  { dword b; memcpy(&b,&f,4); return(b); }
static qword f64bits(double d) { qword q; memcpy(&q,&d,8); return(q); }
static int snanf(float f)  { return((f32bits(f)>>22)&1); }
static int snand(double d) { return((int)((f64bits(d)>>51)&1)); }

// one operand (ABS, NEG, SQRT, CVT between S and D). 0 = stop
static int fpu_inf(float f)
{
    switch(fpclassify(f))
    {
    case FP_SUBNORMAL: if(fpe_unimpl()) return(!fpe_trap()); break;
    case FP_NAN: if(snanf(f)?fpe_cause(FPE_V):fpe_unimpl()) return(!fpe_trap()); break;
    }
    return(1);
}
static int fpu_ind(double f)
{
    switch(fpclassify(f))
    {
    case FP_SUBNORMAL: if(fpe_unimpl()) return(!fpe_trap()); break;
    case FP_NAN: if(snand(f)?fpe_cause(FPE_V):fpe_unimpl()) return(!fpe_trap()); break;
    }
    return(1);
}

// two operands (ADD SUB MUL DIV). 0 = stop
static int fpu_in2(int cl1,int s1,int cl2,int s2)
{
    if((cl1==FP_NAN && !s1) || (cl2==FP_NAN && !s2)) { fpe_unimpl(); return(!fpe_trap()); }
    if(cl1==FP_SUBNORMAL || cl2==FP_SUBNORMAL) { fpe_unimpl(); return(!fpe_trap()); }
    if(((cl1==FP_NAN && s1) || (cl2==FP_NAN && s2)) && fpe_cause(FPE_V)) return(!fpe_trap());
    return(1);
}

// the result: default NaN, denormals flushed with FS or unimplemented. 0 = stop
static int fpu_outf(float *f)
{
    int rm=st.fcr31&3;
    switch(fpclassify(*f))
    {
    case FP_SUBNORMAL:
        if(!(st.fcr31&FCR31_FS) || (st.fcr31&(1u<<(7+FPE_U))) || (st.fcr31&(1u<<(7+FPE_I))))
        {
            fpe_unimpl();
            fpe_trap();
            return(0);
        }
        fpe_cause(FPE_U); fpe_cause(FPE_I);
        if(rm<2)       *f=copysignf(0.0f,*f);
        else if(rm==2) *f=signbit(*f)?-0.0f:FLT_MIN;
        else           *f=signbit(*f)?-FLT_MIN:0.0f;
        return(1);
    case FP_NAN:
        {
            dword b=0x7fbfffff;
            memcpy(f,&b,4);
        }
        return(1);
    }
    return(1);
}
static int fpu_outd(double *f)
{
    int rm=st.fcr31&3;
    switch(fpclassify(*f))
    {
    case FP_SUBNORMAL:
        if(!(st.fcr31&FCR31_FS) || (st.fcr31&(1u<<(7+FPE_U))) || (st.fcr31&(1u<<(7+FPE_I))))
        {
            fpe_unimpl();
            fpe_trap();
            return(0);
        }
        fpe_cause(FPE_U); fpe_cause(FPE_I);
        if(rm<2)       *f=copysign(0.0,*f);
        else if(rm==2) *f=signbit(*f)?-0.0:DBL_MIN;
        else           *f=signbit(*f)?-DBL_MIN:0.0;
        return(1);
    case FP_NAN:
        {
            qword b=0x7ff7ffffffffffffull;
            memcpy(f,&b,8);
        }
        return(1);
    }
    return(1);
}

// to-integer conversions: which inputs are unimplemented (cls: fpclassify
// in the source format, a single denormal is normal as a double). 0 = stop
static int fpu_inconv(double f,int cls,int bits)
{
    switch(cls)
    {
    case FP_SUBNORMAL: case FP_INFINITE: case FP_NAN:
        fpe_unimpl();
        return(!fpe_trap());
    }
    if(bits==32 ? (f>=2147483648.0 || f<-2147483648.0)
                : (f>=9007199254740992.0 || f<=-9007199254740992.0))
    {
        fpe_unimpl();
        return(!fpe_trap());
    }
    return(1);
}

// round to an integral value the way the SSE4.1 ROUNDSS/SD instructions do
// (they raise inexact); mode: 0 nearest, 1 zero, 2 up, 3 down, 4 current
static float fpu_roundf(float f,int mode)
{
    __m128 t=_mm_set_ss(f);
    switch(mode)
    {
    case 0: t=_mm_round_ss(t,t,_MM_FROUND_TO_NEAREST_INT); break;
    case 1: t=_mm_round_ss(t,t,_MM_FROUND_TO_ZERO); break;
    case 2: t=_mm_round_ss(t,t,_MM_FROUND_TO_POS_INF); break;
    case 3: t=_mm_round_ss(t,t,_MM_FROUND_TO_NEG_INF); break;
    default: t=_mm_round_ss(t,t,_MM_FROUND_CUR_DIRECTION); break;
    }
    return(_mm_cvtss_f32(t));
}
static double fpu_roundd(double f,int mode)
{
    __m128d t=_mm_set_sd(f);
    switch(mode)
    {
    case 0: t=_mm_round_sd(t,t,_MM_FROUND_TO_NEAREST_INT); break;
    case 1: t=_mm_round_sd(t,t,_MM_FROUND_TO_ZERO); break;
    case 2: t=_mm_round_sd(t,t,_MM_FROUND_TO_POS_INF); break;
    case 3: t=_mm_round_sd(t,t,_MM_FROUND_TO_NEG_INF); break;
    default: t=_mm_round_sd(t,t,_MM_FROUND_CUR_DIRECTION); break;
    }
    return(_mm_cvtsd_f64(t));
}

// fmt 16 (S), 17 (D), 20 (W), 21 (L): the arithmetic, compare and convert
// encodings (not MOV)
static void op_fpu_lleop(dword opcode,int fmt,int op)
{
    const int fd=OP_SHAMT(opcode),fs=fpu_fs(OP_RD(opcode)),ft=OP_RT(opcode);
    const int single=(fmt==16);

    st.fcr31&=~FCR31_CAUSE;
    feclearexcept(FE_ALL_EXCEPT);

    if((fmt==16 || fmt==17) && op<=7 && op!=6)
    { // ADD SUB MUL DIV SQRT ABS NEG
        if(single)
        {
            volatile float a=fpu_getf(fs),b=fpu_getf(ft),r;
            if(op<=3)
            {
                if(!fpu_in2(fpclassify(a),snanf(a),fpclassify(b),snanf(b))) return;
            }
            else if(!fpu_inf(a)) return;
            feclearexcept(FE_ALL_EXCEPT);
            switch(op)
            {
            case 0: r=a+b; break;
            case 1: r=a-b; break;
            case 2: r=a*b; break;
            case 3: r=a/b; break;
            case 4: r=_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(a))); break;
            case 5: r=fabsf(a); break;
            default: r=-a; break;
            }
            {
                float o=r;
                if(op!=5 && fpe_host(0)) return;
                if(!fpu_outf(&o)) return;
                fpu_putf(fd,o);
            }
        }
        else
        {
            volatile double a=fpu_getd(fs),b=fpu_getd(ft),r;
            if(op<=3)
            {
                if(!fpu_in2(fpclassify(a),snand(a),fpclassify(b),snand(b))) return;
            }
            else if(!fpu_ind(a)) return;
            feclearexcept(FE_ALL_EXCEPT);
            switch(op)
            {
            case 0: r=a+b; break;
            case 1: r=a-b; break;
            case 2: r=a*b; break;
            case 3: r=a/b; break;
            case 4: r=_mm_cvtsd_f64(_mm_sqrt_sd(_mm_set_sd(a),_mm_set_sd(a))); break;
            case 5: r=fabs(a); break;
            default: r=-a; break;
            }
            {
                double o=r;
                if(op!=5 && fpe_host(0)) return;
                if(!fpu_outd(&o)) return;
                fpu_putd(fd,o);
            }
        }
        return;
    }

    if((fmt==16 || fmt==17) && op>=48)
    { // C.cond: bit 0 = true when unordered, 1 = equal, 2 = less; bit 3
      // (C.SF..C.NGT) = any NaN is invalid, else only a signalling one
        double a,b;
        int    nana,nanb,sa,sb;
        if(single)
        {
            float x=fpu_getf(fs),y=fpu_getf(ft);
            a=x; b=y; nana=isnan(x); nanb=isnan(y); sa=snanf(x); sb=snanf(y);
        }
        else
        {
            a=fpu_getd(fs); b=fpu_getd(ft);
            nana=isnan(a); nanb=isnan(b); sa=snand(a); sb=snand(b);
        }
        if(nana || nanb)
        {
            if(nana && ((op&8) || sa) && fpe_cause(FPE_V)) { fpe_trap(); return; }
            if(nanb && ((op&8) || sb) && fpe_cause(FPE_V)) { fpe_trap(); return; }
            st.fputrue=op&1;
            return;
        }
        st.fputrue=((op&2) && a==b) || ((op&4) && a<b);
        return;
    }

    if((fmt==16 || fmt==17) &&
       ((op>=8 && op<=15) || op==36 || op==37))
    { // ROUND/TRUNC/CEIL/FLOOR .L (8-11) .W (12-15), CVT.W (36), CVT.L (37)
        const int bits=(op==37 || op<12)?64:32;
        const int mode=(op>=36)?4:(op&3);
        double in=single?(double)fpu_getf(fs):fpu_getd(fs);
        double rounded;
        qword  v;
        if(!fpu_inconv(in,single?fpclassify(fpu_getf(fs)):fpclassify(in),bits)) return;
        feclearexcept(FE_ALL_EXCEPT);
        if(single) rounded=fpu_roundf(fpu_getf(fs),mode);
        else       rounded=fpu_roundd(in,mode);
        // .W: rounding up to 2^31 overflows the host's 32-bit conversion,
        // an invalid operation there, so unimplemented (ares CHECK_FPE_CONV)
        if(bits==32 && (rounded>=2147483648.0 || rounded<-2147483648.0))
        {
            fpe_unimpl(); fpe_trap();
            return;
        }
        v=(qword)(qint)rounded;
        if(fpe_host(bits==32)) return;
        // ROUND and TRUNC also test exactness themselves (ares)
        if((mode==0 || mode==1) && rounded!=in && fpe_cause(FPE_I)) { fpe_trap(); return; }
        if(bits==64) fpr_setphys(fd,v);
        else         fpu_put32(fd,(dword)v);
        return;
    }

    if(op==32 && fmt!=16)
    { // CVT.S from D, W, L
        volatile float r;
        if(fmt==17)
        {
            double a=fpu_getd(fs);
            if(!fpu_ind(a)) return;
            feclearexcept(FE_ALL_EXCEPT);
            r=(float)a;
        }
        else if(fmt==20)
        {
            r=(float)(int)fpu_get32(fs);
        }
        else if(fmt==21)
        {
            qint l=(qint)fpr_phys(fs);
            if(l>=(qint)0x0080000000000000ll || l<(qint)0xff80000000000000ll)
            {
                fpe_unimpl(); fpe_trap();
                return;
            }
            r=(float)l;
        }
        else
        {
            fpe_unimpl(); fpe_trap();
            return;
        }
        {
            float o=r;
            if(fpe_host(0)) return;
            if(!fpu_outf(&o)) return;
            fpu_putf(fd,o);
        }
        return;
    }

    if(op==33 && fmt!=17)
    { // CVT.D from S, W, L
        volatile double r;
        if(fmt==16)
        {
            float a=fpu_getf(fs);
            if(!fpu_inf(a)) return;
            feclearexcept(FE_ALL_EXCEPT);
            r=(double)a;
        }
        else if(fmt==20)
        {
            r=(double)(int)fpu_get32(fs);
        }
        else if(fmt==21)
        {
            qint l=(qint)fpr_phys(fs);
            if(l>=(qint)0x0080000000000000ll || l<(qint)0xff80000000000000ll)
            {
                fpe_unimpl(); fpe_trap();
                return;
            }
            r=(double)l;
        }
        else
        {
            fpe_unimpl(); fpe_trap();
            return;
        }
        {
            double o=r;
            if(fpe_host(0)) return;
            if(!fpu_outd(&o)) return;
            fpu_putd(fd,o);
        }
        return;
    }

    // everything else (CVT.S.S, CVT.D.D, W/L arithmetic, reserved functions)
    fpe_unimpl();
    fpe_trap();
}

// in the FCSR rounding mode (the host goes back to nearest afterwards)
static void op_fpu_lle(dword opcode,int fmt,int op)
{
    if(st.fcr31&3) fesetround(fpu_hostrm[st.fcr31&3]);
    op_fpu_lleop(opcode,fmt,op);
    if(st.fcr31&3) fesetround(FE_TONEAREST);
}

static void op_fpu(dword opcode)
{
    int fmt=OP_RS(opcode);
    int op=OP_FUNC(opcode);

    // 0x00-0x3f = basic ops
    // 0x40-0x7F = BC ops
    // 0x80-0x8F = BC ops
    if(fmt<8)
    { // Move ops
        int rt,fs;
        rt=OP_RT(opcode);
        fs=OP_RD(opcode);
        switch(fmt)
        {
        case 0: // MFC1 (sign-extended, as all 32-bit results)
            if(!rt) break;
            st.g[rt].d=st.f[fs].d;
            st.g[rt].d2[1]=(int)st.g[rt].d2[0]>>31;
            break;
        case 1: // DMFC1: the low word is the first of a pair, as LDC1 loads
            if(!rt) break;
            st.g[rt].q=fpr_get64(fs);
            break;
        case 4: // MTC1
            st.f[fs].d=st.g[rt].d;
            break;
        case 5: // DMTC1
            fpr_set64(fs,st.g[rt].q);
            break;
        case 2: // CFC1 (sign-extended)
            {
                dword v=(fs==0)?FCR0_VALUE:(fs==31)?fcr31_get():0;
                st.g[rt].q=(qword)(qint)(int)v;
            }
            break;
        case 6: // CTC1 (FCR31 only; FCR0 is read-only)
            if(fs==31) fcr31_set(st.g[rt].d);
            break;
        default:
            fpu_unimplemented("unimplemented FPU-Move opcode");
            break;
        }
    }
    else if(fmt==8)
    { // BC ops (branch)
        int rt=OP_RT(opcode);
        int ontrue=rt&1;
        int likely=(rt&2)>>1;
        int flag;

        if(st.lleos)
        { // BC1 clears the causes (ares fpuCheckStart); rt 4..31 are
          // unimplemented (n64-systemtest "Reserved COP1 encodings")
            st.fcr31&=~FCR31_CAUSE;
            if(rt>=4)
            {
                fpe_unimpl();
                fpe_trap();
                return;
            }
        }

        if(ontrue) flag=st.fputrue;
        else       flag=!st.fputrue;

        op_branch(opcode,flag,likely,0);
    }
    else
    { // generic ops
        double  r,a,b;
        int     storer=1;
        if(st.lleos && op!=6)
        {
            op_fpu_lle(opcode,fmt,op);
            return;
        }
        if(fmt==16)
        { // single
            a=(double)fpu_getf(fpu_fs(OP_RD(opcode)));
            b=(double)fpu_getf(OP_RT(opcode));
        }
        else if(fmt==17)
        { // double
            a=fpu_getd(fpu_fs(OP_RD(opcode)));
            b=fpu_getd(OP_RT(opcode));
        }
        else
        {
            if(op!=33 && op!=32) op=255; // force error
        }
        if(op!=6) st.fcr31&=~FCR31_CAUSE; // each operation (not MOV) sets its causes
        // The game's rounding mode (Pokemon Stadium runs with "toward
        // zero"). Singles are computed in double and then narrowed, both
        // in this mode, which rounds as a single operation would; the
        // host goes back to nearest below.
        if(st.fcr31&3) fesetround(fpu_hostrm[st.fcr31&3]);
        switch(op)
        {
        case 0: // ADD
            r=a+b;
            break;
        case 1: // SUB
            r=a-b;
            break;
        case 2: // MUL
            r=a*b;
            break;
        case 3: // DIV
            if(b==0 && a==a && a!=0 && !isinf(a) && fpu_divzero()) storer=0;
            r=a/b;
            break;
        case 4: // SQRT
            r=sqrt(a);
            break;
        case 5: // ABS [not used]
            r=fabs(a);
            break;
        case 7: // NEG
            r=-a;
            break;
        // C.cond: bit 0 = true if unordered, bit 1 = if equal, bit 2 = if
        // less; bit 3 (C.SF..C.NGT) = a NaN operand is an invalid operation
        case 48: case 49: case 50: case 51: // C.F C.UN C.EQ C.UEQ
        case 52: case 53: case 54: case 55: // C.OLT C.ULT C.OLE C.ULE
        case 56: case 57: case 58: case 59: // C.SF C.NGLE C.SEQ C.NGL
        case 60: case 61: case 62: case 63: // C.LT C.NGE C.LE C.NGT
            storer=0;
            if(a!=a || b!=b)
            {
                if((op&8) && fpu_invalid()) break;
                st.fputrue=op&1;
            }
            else st.fputrue=((op&2) && a==b) || ((op&4) && a<b);
            break;
        // convert & move
        case 6: // MOV
            // MOV.S copies all 64 bits too, like MOV.D (n64-systemtest)
            storer=0;
            fpr_setphys(OP_SHAMT(opcode),fpr_phys(fpu_fs(OP_RD(opcode))));
            break;
        // to integer: ROUND/TRUNC/CEIL/FLOOR have fixed rounding (func&3),
        // CVT.W/CVT.L use the FCSR mode (IDO sets "toward zero" with CTC1
        // around its cvt.w for C casts). libdragon's mixer steps channels by
        // (int64_t)(float): with CVT.L missing the step was 0 and every
        // sound stuck at its first sample (Flappy Bird had no sound effects)
        case 8:  case 9:  case 10: case 11: // ROUND.L TRUNC.L CEIL.L FLOOR.L
        case 12: case 13: case 14: case 15: // ROUND.W TRUNC.W CEIL.W FLOOR.W
        case 36: case 37:                   // CVT.W CVT.L
            {
                int   rm=(op>=36)?(int)(st.fcr31&3):(op&3);
                int   bits=(op==37 || op<12)?64:32;
                qword v;
                storer=0;
                if(!fpu_toint(a,rm,bits,&v)) break;
                if(bits==64) fpr_setphys(OP_SHAMT(opcode),v);
                else fpu_put32(OP_SHAMT(opcode),(dword)v);
            }
            break;
        case 32: // CVT.S [used]
            storer=0;
            if(fmt==20)      fpu_putf(OP_SHAMT(opcode),(float)(int)fpu_get32(fpu_fs(OP_RD(opcode))));
            else if(fmt==21)
            {
                fpu_putf(OP_SHAMT(opcode),(float)(qint)fpr_phys(fpu_fs(OP_RD(opcode))));
            }
            else if(fmt==17) fpu_putf(OP_SHAMT(opcode),(float)fpu_getd(fpu_fs(OP_RD(opcode))));
            else
            {
                 fpu_unimplemented("unimplemented FPU-CVT-opcode");
            }
            break;
        case 33: // CVT.D [used]
            storer=0;
            if(fmt==20)      fpu_putd(OP_SHAMT(opcode),(double)(int)fpu_get32(fpu_fs(OP_RD(opcode))));
            else if(fmt==21)
            {
                // [used in goldeneye]
                fpu_putd(OP_SHAMT(opcode),(double)(qint)fpr_phys(fpu_fs(OP_RD(opcode))));
            }
            else if(fmt==16) fpu_putd(OP_SHAMT(opcode),(double)fpu_getf(fpu_fs(OP_RD(opcode))));
            else
            {
                fpu_unimplemented("unimplemented FPU-CVT-opcode");
            }
            break;
        default:
            storer=0;
            fpu_unimplemented("unimplemented FPU-opcode");
            break;
        }
        // ADD..SQRT: a NaN from non-NaN operands (inf-inf, 0*inf, 0/0,
        // sqrt of a negative) is an invalid operation
        if(storer && op<=4 && r!=r && a==a && (op==4 || b==b) && fpu_invalid()) storer=0;
        if(storer)
        {
            if(fmt==16)
            { // single
                float f=(float)r;
                // FS: a denormalized result becomes zero
                if((st.fcr31&FCR31_FS) && f!=0 && fabsf(f)<FLT_MIN) f=copysignf(0.0f,f);
                fpu_putf(OP_SHAMT(opcode),f);
            }
            else if(fmt==17)
            { // double
                if((st.fcr31&FCR31_FS) && r!=0 && fabs(r)<DBL_MIN) r=copysign(0.0,r);
                fpu_putd(OP_SHAMT(opcode),r);
            }
        }
        if(st.fcr31&3) fesetround(FE_TONEAREST);
    }

}

static void op_main(dword opcode)
{
    int op,flag;
    int *rs,*rt,*rd,imm[2];

    op=OP_OP(opcode);
    if(op==0) op=OP_FUNC(opcode)+0x40;
    else if(op==1) op=OP_RT(opcode)+0x80;

    // supervisor and user mode (LLE): COP0 and CACHE need Status.CU0 (else
    // coprocessor unusable, CE 0); without SX/UX the 64-bit instructions are
    // reserved (n64-systemtest "Privilege: ...")
    if(st.lleos && (st.mmu[12].d&0x18) && !(st.mmu[12].d&6))
    {
        int m=cpu_mode();
        if((op==COP0 || op==CACHE) && !(st.mmu[12].d&0x10000000))
        {
            st.mmu[13].d&=~0x30000000u;
            lle_exception(EXC_CPU);
            return;
        }
        if(!(st.mmu[12].d&(m==1?0x40:0x20)))
        {
            switch(op)
            {
            case 0x40+DADD: case 0x40+DADDU: case 0x40+DSUB: case 0x40+DSUBU:
            case 0x40+DMULT: case 0x40+DMULTU: case 0x40+DDIV: case 0x40+DDIVU:
            case 0x40+DSLL: case 0x40+DSRL: case 0x40+DSRA:
            case 0x40+DSLL32: case 0x40+DSRL32: case 0x40+DSRA32:
            case 0x40+DSLLV: case 0x40+DSRLV: case 0x40+DSRAV:
            case DADDI: case DADDIU: case LD: case LDL: case LDR: case LLD:
            case SCD: case SD: case SDL: case SDR:
                op_reserved("64-bit instruction in 32-bit mode");
                return;
            }
        }
    }

    switch(op)
    {
    //------------------------------ignored coprosessor stuff
    case COP0: // cop0
        op_scc( opcode );
        break;
    case COP2: // cop2 (the N64 has none)
    case LWC2: case LDC2: case SWC2: case SDC2:
        if(st.lleos) op_cop2(opcode);
        else op_reserved("unimplemented COP2 opcode");
        break;
    //------------------------------special ops!
    case OP_PATCH: // *PATCH* (opcode 0x1C, reserved on the VR4300)
        if(st.lleos && !st.patches)
        { // no OS patches installed: the game's own reserved opcode
            op_reserved("reserved opcode 0x1C");
            break;
        }
        {
            int p=OP_IMM(opcode);
            op_patch(p);
            if(p<10 || p>=50) st.bailout-=4;
            else st.bailout-=150; // approximate every os routine takes 100 cycles
        }
        break;
    case OP_GROUP: // *GROUP* (opcode 0x1D, reserved on the VR4300)
        if(st.lleos) op_reserved("reserved opcode 0x1D");
        else error("reserved group-opcode encountered in cpuc");
        break;
    //------------------------------loads
    case LB: // LB
        op_readmem(opcode,-1);
        break;
    case LBU: // LBU
        op_readmem(opcode,1);
        break;
    case LH: // LH
        op_readmem(opcode,-2);
        break;
    case LHU: // LHU
        op_readmem(opcode,2);
        break;
    case LW: // LW
        op_readmem(opcode,4);
        break;
    case LWU: // LWU
        op_readmem(opcode,4);
        if(OP_RT(opcode)) st.g[OP_RT(opcode)].d2[1]=0;
        break;
    case LWL: // LWL
        op_rwmemrl(opcode,0,0);
        break;
    case LWR: // LWR
        op_rwmemrl(opcode,0,1);
        break;
    case LDL: // LDL
        op_rwmemrl64(opcode,0,0);
        break;
    case LDR: // LDR
        op_rwmemrl64(opcode,0,1);
        break;
    case LL: // LL
    case LLD: // LLD
        if(op==LLD) st.expanded64bit=1;
        op_readmem(opcode,op==LL?4:8);
        if(!lle_jumped)
        {
            llbit=1;
            st.mmu[17].d=op_lladdr(opcode); // LLAddr
        }
        break;
    case LD: // LD
//        logi("doubleword load at (%08X)\n",st.pc);
        st.expanded64bit=1;
        op_readmem(opcode,8);
        break;
    //------------------------------stores
    case SB: // SB
        op_writemem(opcode,1);
        break;
    case SH: // SH
        op_writemem(opcode,2);
        break;
    case SW: // SW
        op_writemem(opcode,4);
        break;
    case SWL: // SWL
        op_rwmemrl(opcode,1,0);
        break;
    case SWR: // SWR
        op_rwmemrl(opcode,1,1);
        break;
    case SDL: // SDL
        op_rwmemrl64(opcode,1,0);
        break;
    case SDR: // SDR
        op_rwmemrl64(opcode,1,1);
        break;
    case SD: // SD
//        logi("doubleword store at (%08X)\n",st.pc);
        op_writemem(opcode,8);
        break;
    case SC: // SC: stores only with the LL reservation; rt = success
    case SCD: // SCD
        {
            int bytes=(op==SC)?4:8;
            if(llbit) op_writemem(opcode,bytes);
            else if(op_misaligned(op_memaddr(opcode),bytes-1,1)) break;
            if(lle_jumped) break; // faulted: rt keeps its value
            st.g[OP_RT(opcode)].q=llbit?1:0;
            // LLbit stays set: only ERET clears it (ares; n64-systemtest
            // "cache: Read cached vs uncached" runs SC then SCD after one LL)
            if(!st.lleos) llbit=0;
        }
        break;
    //------------------------------arithmetic
    case ADDI: // ADDI
    case ADDIU: // ADDIU
        GETREGSIMM;
        if(op==ADDI && op_overflow32((qint)(int)*rs+(qint)(int)*rt)) break;
        *rd=(int)((dword)*rs+(dword)*rt);
        break;
    case DADDI: // DADDI
    case DADDIU: // DADDIU
        GETREGSIMM64;
        if(op==DADDI)
        {
            qword a=st.g[OP_RS(opcode)].q,b=(qword)(qint)SIGNEXT16(OP_IMM(opcode));
            if(op_overflow64(a,b,a+b,0)) break;
        }
        addi64(rd,rs,rt);
        break;
    case SLTI: // SLTI
        st.g[OP_RT(opcode)].q=gpr_s(OP_RS(opcode))<(qint)SIGNEXT16(OP_IMM(opcode));
        break;
    case SLTIU: // SLTIU (immediate sign-extended, compared unsigned)
        st.g[OP_RT(opcode)].q=gpr_u(OP_RS(opcode))<(qword)(qint)SIGNEXT16(OP_IMM(opcode));
        break;
    case ANDI: // ANDI
        GETREGSIMMUNS;
        *rd=*rs & *rt;
        break;
    case ORI: // ORI
        GETREGSIMMUNS;
        *rd=*rs | *rt;
        break;
    case XORI: // XORI
        GETREGSIMMUNS;
        *rd=*rs ^ *rt;
        break;
    case LUI: // LUI
        GETREGSIMM;
//        print("at %08X lui %04X\n",st.pc,(*rt)&0xffff);
        if((*rt&0x1F00)==0x0400)
        {
            if(hw_ismemiorange(*rt<<16)) st.memiodetected=(*rt<<16);
        }
        *rd=*rt << 16;
        break;
    case 0x40+SLL: // SLL
        GETREGS;
        *rd=*rt << OP_SHAMT(opcode);
        break;
    case 0x40+SRL: // SRL
        GETREGS;
        *rd=(unsigned)*rt >> OP_SHAMT(opcode);
        break;
    case 0x40+SRA: // SRA: the VR4300 shifts the whole 64-bit register, keeps
        // the low word (n64-systemtest: 0123456789ABCDEF>>4 = 789ABCDE)
        GETREGS;
        *rd=(int)(dword)(gpr_s(OP_RT(opcode)) >> OP_SHAMT(opcode));
        break;
    case 0x40+SLLV: // SLLV (the shift amount is rs's low 5 bits)
        GETREGS;
        *rd=(int)((dword)*rt << (*rs&31));
        break;
    case 0x40+SRLV: // SRLV
        GETREGS;
        *rd=(unsigned)*rt >> (*rs&31);
        break;
    case 0x40+SRAV: // SRAV: as SRA
        GETREGS;
        *rd=(int)(dword)(gpr_s(OP_RT(opcode)) >> (*rs&31));
        break;
    case 0x40+DSLLV: // DSLLV
        op_shift64(opcode,0,-1);
        break;
    case 0x40+DSRLV: // DSRLV
        op_shift64(opcode,1,-1);
        break;
    case 0x40+DSRAV: // DSRAV
        op_shift64(opcode,2,-1);
        break;
    case 0x40+SYSCALL: // SYSCALL
        if(st.lleos) lle_exception(EXC_SYS);
        else exception("opcode syscall");
        break;
    case 0x40+BREAK: // BREAK
        if(st.lleos) lle_exception(EXC_BP);
        else exception("opcode break");
        break;
    case 0x40+MFHI: // MFHI
        GETREGS;
        rd[0]=st.mhi.d2[0];
        rd[1]=st.mhi.d2[1];
        break;
    case 0x40+MTHI: // MTHI
        GETREGS;
        st.mhi.d2[0]=rs[0];
        st.mhi.d2[1]=rs[1];
        break;
    case 0x40+MFLO: // MFLO
        GETREGS;
        rd[0]=st.mlo.d2[0];
        rd[1]=st.mlo.d2[1];
        break;
    case 0x40+MTLO: // MTLO
        GETREGS;
        st.mlo.d2[0]=rs[0];
        st.mlo.d2[1]=rs[1];
        break;
    case 0x40+MULT: // MULT
        if(st.lleos)
        { // rs as 64 bits, rt sign-extended from 35 bits (n64-systemtest
          // "MULT"); op_highword sign-extends both halves
            qint a=(qint)st.g[OP_RS(opcode)].q;
            qint b=((qint)(st.g[OP_RT(opcode)].q<<29))>>29;
            qword p=(qword)a*(qword)b;
            st.mlo.d2[0]=(dword)p;
            st.mhi.d2[0]=(dword)(p>>32);
            break;
        }
        GETREGS;
        op_mult(*rs,*rt);
        break;
    case 0x40+MULTU: // MULTU
        GETREGS;
        op_multu(*rs,*rt);
        break;
    case 0x40+DIV: // DIV
        GETREGS;
        op_div(*rs,*rt);
        break;
    case 0x40+DIVU: // DIVU
        GETREGS;
        op_divu(*rs,*rt);
        break;
    case 0x40+DMULT: // DMULT
        op_dmult(OP_RS(opcode),OP_RT(opcode));
        break;
    case 0x40+DMULTU: // DMULTU
        op_dmultu(OP_RS(opcode),OP_RT(opcode));
        break;
    case 0x40+DDIV: // DDIV
        op_ddiv(OP_RS(opcode),OP_RT(opcode));
        break;
    case 0x40+DDIVU: // DDIVU
        op_ddivu(OP_RS(opcode),OP_RT(opcode));
        break;
    case 0x40+ADD: // ADD
    case 0x40+ADDU: // ADDU
        GETREGS;
        if(op==0x40+ADD && op_overflow32((qint)(int)*rs+(qint)(int)*rt)) break;
        *rd=(int)((dword)*rs+(dword)*rt);
        break;
    case 0x40+SUB: // SUB
    case 0x40+SUBU: // SUBU
        GETREGS;
        if(op==0x40+SUB && op_overflow32((qint)(int)*rs-(qint)(int)*rt)) break;
        *rd=(int)((dword)*rs-(dword)*rt);
        break;
    case 0x40+AND: // AND
        GETREGS;
        *rd=*rs & *rt;
        rd[1]=rs[1] & rt[1]; // do 64bit too (goldeneye needs)
        break;
    case 0x40+OR: // OR
        GETREGS;
        *rd=*rs | *rt;
        rd[1]=rs[1] | rt[1]; // do 64bit too (goldeneye needs)
        break;
    case 0x40+XOR: // XOR
        GETREGS;
        *rd=*rs ^ *rt;
        rd[1]=rs[1] ^ rt[1]; // do 64bit too (goldeneye needs)
        break;
    case 0x40+NOR: // NOR
        GETREGS;
        *rd=~(*rs | *rt);
        rd[1]=~(rs[1] | rt[1]); // do 64bit too (goldeneye needs)
        break;
    case 0x40+SLT: // SLT
        st.g[OP_RD(opcode)].q=gpr_s(OP_RS(opcode))<gpr_s(OP_RT(opcode));
        break;
    case 0x40+SLTU: // SLTU
        st.g[OP_RD(opcode)].q=gpr_u(OP_RS(opcode))<gpr_u(OP_RT(opcode));
        break;
    case 0x40+DADD: // DADD
    case 0x40+DADDU: // DADDU
        GETREGS;
        if(op==0x40+DADD)
        {
            qword a=st.g[OP_RS(opcode)].q,b=st.g[OP_RT(opcode)].q;
            if(op_overflow64(a,b,a+b,0)) break;
        }
        addi64(rd,rs,rt);
        break;
    case 0x40+DSUB: // DSUB
    case 0x40+DSUBU: // DSUBU
        GETREGS;
        if(op==0x40+DSUB)
        {
            qword a=st.g[OP_RS(opcode)].q,b=st.g[OP_RT(opcode)].q;
            if(op_overflow64(a,b,a-b,1)) break;
        }
        op_sub64(rd,rs,rt);
        break;
    case 0x40+SYNC: // SYNC
        break;
    //------------------------------traps (full register width)
    case 0x40+TGE: // TGE
        op_trap(gpr_s(OP_RS(opcode))>=gpr_s(OP_RT(opcode)));
        break;
    case 0x40+TGEU: // TGEU
        op_trap(gpr_u(OP_RS(opcode))>=gpr_u(OP_RT(opcode)));
        break;
    case 0x40+TLT: // TLT
        op_trap(gpr_s(OP_RS(opcode))<gpr_s(OP_RT(opcode)));
        break;
    case 0x40+TLTU: // TLTU
        op_trap(gpr_u(OP_RS(opcode))<gpr_u(OP_RT(opcode)));
        break;
    case 0x40+TEQ: // TEQ
        op_trap(gpr_u(OP_RS(opcode))==gpr_u(OP_RT(opcode)));
        break;
    case 0x40+TNE: // TNE
        op_trap(gpr_u(OP_RS(opcode))!=gpr_u(OP_RT(opcode)));
        break;
    // immediates are sign-extended, then compared signed or unsigned
    case 0x80+TGEI: // TGEI
        op_trap(gpr_s(OP_RS(opcode))>=(qint)SIGNEXT16(OP_IMM(opcode)));
        break;
    case 0x80+TGEIU: // TGEIU
        op_trap(gpr_u(OP_RS(opcode))>=(qword)(qint)SIGNEXT16(OP_IMM(opcode)));
        break;
    case 0x80+TLTI: // TLTI
        op_trap(gpr_s(OP_RS(opcode))<(qint)SIGNEXT16(OP_IMM(opcode)));
        break;
    case 0x80+TLTIU: // TLTIU
        op_trap(gpr_u(OP_RS(opcode))<(qword)(qint)SIGNEXT16(OP_IMM(opcode)));
        break;
    case 0x80+TEQI: // TEQI
        op_trap(gpr_s(OP_RS(opcode))==(qint)SIGNEXT16(OP_IMM(opcode)));
        break;
    case 0x80+TNEI: // TNEI
        op_trap(gpr_s(OP_RS(opcode))!=(qint)SIGNEXT16(OP_IMM(opcode)));
        break;
    case 0x40+DSLL: // DSLL
        op_shift64(opcode,0,OP_SHAMT(opcode)+0);
        break;
    case 0x40+DSLR: // DSLR
        op_shift64(opcode,1,OP_SHAMT(opcode)+0);
        break;
    case 0x40+DSLA: // DSLA
        op_shift64(opcode,2,OP_SHAMT(opcode)+0);
        break;
    case 0x40+DSLL32: // DSLL32
        op_shift64(opcode,0,OP_SHAMT(opcode)+32);
        break;
    case 0x40+DSLR32: // DSLR32
        op_shift64(opcode,1,OP_SHAMT(opcode)+32);
        break;
    case 0x40+DSLA32: // DSLA32
        op_shift64(opcode,2,OP_SHAMT(opcode)+32);
        break;
    //------------------------------jump and branch
    case J: // J
        op_jump(opcode,0,-1);
        break;
    case JAL: // JAL
        op_jump(opcode,1,-1);
        break;
    case BEQ: // BEQ
        flag=gpr_u(OP_RS(opcode))==gpr_u(OP_RT(opcode));
        op_branch(opcode,flag,0,0);
        break;
    case BNE: // BNEQ
        flag=gpr_u(OP_RS(opcode))!=gpr_u(OP_RT(opcode));
        op_branch(opcode,flag,0,0);
        break;
    case BLEZ: // BLEZ
        flag=gpr_s(OP_RS(opcode))<=0;
        op_branch(opcode,flag,0,0);
        break;
    case BGTZ: // BGTZ
        flag=gpr_s(OP_RS(opcode))>0;
        op_branch(opcode,flag,0,0);
        break;
    case BEQL: // BEQL
        flag=gpr_u(OP_RS(opcode))==gpr_u(OP_RT(opcode));
        op_branch(opcode,flag,1,0);
        break;
    case BNEL: // BNEL BNEQL
        flag=gpr_u(OP_RS(opcode))!=gpr_u(OP_RT(opcode));
        op_branch(opcode,flag,1,0);
        break;
    case BLEZL: // BLEZL
        flag=gpr_s(OP_RS(opcode))<=0;
        op_branch(opcode,flag,1,0);
        break;
    case BGTZL: // BGTZL
        flag=gpr_s(OP_RS(opcode))>0;
        op_branch(opcode,flag,1,0);
        break;
    case 0x40+JR: // JR
        op_jump(opcode,0,OP_RS(opcode));
        break;
    case 0x40+JALR: // JALR
        op_jump(opcode,1,OP_RS(opcode));
        break;
    case 0x80+BLTZ: // BLTZ
        flag=gpr_s(OP_RS(opcode))<0;
        op_branch(opcode,flag,0,0);
        break;
    case 0x80+BGEZ: // BGEZ
        flag=gpr_s(OP_RS(opcode))>=0;
        op_branch(opcode,flag,0,0);
        break;
    case 0x80+BLTZL: // BLTZL
        flag=gpr_s(OP_RS(opcode))<0;
        op_branch(opcode,flag,1,0);
        break;
    case 0x80+BGEZL: // BGEZL
        flag=gpr_s(OP_RS(opcode))>=0;
        op_branch(opcode,flag,1,0);
        break;
    case 0x80+BLTZAL: // BLTZAL
        flag=gpr_s(OP_RS(opcode))<0;
        op_branch(opcode,flag,0,1);
        break;
    case 0x80+BGEZAL: // BGEZAL
        flag=gpr_s(OP_RS(opcode))>=0;
        op_branch(opcode,flag,0,1);
        break;
    case 0x80+BLTZALL: // BLTZALL
        flag=gpr_s(OP_RS(opcode))<0;
        op_branch(opcode,flag,1,1);
        break;
    case 0x80+BGEZALL: // BGEZALL
        flag=gpr_s(OP_RS(opcode))>=0;
        op_branch(opcode,flag,1,1);
        break;
    case CACHE: // CACHE
        if(st.lleos) op_cache(opcode);
        break;
    //------------------------------fpu
    case COP1:
        if(st.lleos && lle_cop1unusable()) break;
        op_fpu(opcode);
        break;
    case LDC1: // LDC1
        if(st.lleos && lle_cop1unusable()) break;
        op_readmem(opcode,0x18);
        break;
    case SDC1: // SDC1
        if(st.lleos && lle_cop1unusable()) break;
        op_writemem(opcode,0x18);
        break;
    case LWC1: // LWC1
        if(st.lleos && lle_cop1unusable()) break;
        op_readmem(opcode,0x14);
        break;
    case SWC1: // SWC1
        if(st.lleos && lle_cop1unusable()) break;
        op_writemem(opcode,0x14);
        break;
    //--------------
    default:
        op_reserved("unimplemented CPU opcode");
    }
}

// The VR4300's 32-bit operations write all 64 bits of the destination: the
// result sign-extended (ANDI zero-extends, ORI/XORI keep rs's high word).
// The ops above only write the low word, and a stale high word then leaked
// out through SD: libdragon's Flappy Bird stored "addiu a0,r0,0x82" with the
// 0xC8 left by an earlier DSLL32, and drew with a sprite index of 200.
static void op_highword(dword opcode)
{
    int   op=OP_OP(opcode),r;
    dword *g;

    if(op==0)
    {
        switch(OP_FUNC(opcode))
        {
        case SLL: case SRL: case SRA: case SLLV: case SRLV: case SRAV:
        case ADD: case ADDU: case SUB: case SUBU: case SLT: case SLTU:
        case JALR:
            r=OP_RD(opcode);
            break;
        case MULT: case MULTU: case DIV: case DIVU:
            st.mlo.d2[1]=(int)st.mlo.d2[0]>>31;
            st.mhi.d2[1]=(int)st.mhi.d2[0]>>31;
            return;
        default:
            return;
        }
    }
    else switch(op)
    {
    case ADDI: case ADDIU: case SLTI: case SLTIU: case LUI: // LWL/LWR: op_rwmemrl
        r=OP_RT(opcode);
        break;
    case ANDI:
        if(OP_RT(opcode)) st.g[OP_RT(opcode)].d2[1]=0;
        return;
    case ORI: case XORI:
        if(OP_RT(opcode)) st.g[OP_RT(opcode)].d2[1]=st.g[OP_RS(opcode)].d2[1];
        return;
    case JAL:
        r=31;
        break;
    case 1: // REGIMM: the linking branches
        if(OP_RT(opcode)<0x10 || OP_RT(opcode)>0x13) return;
        r=31;
        break;
    default:
        return;
    }
    if(!r) return;
    g=st.g[r].d2;
    g[1]=(int)g[0]>>31;
}

// LLE: the ares pipeline (see lle_pcnext). st.branchdelay is 1 while the
// instruction running is a delay slot, st.branchto the address after it.
static void c_execop_lle(dword opcode)
{
    const int   bd0=st.branchdelay;
    const dword bt0=st.branchto;
    const int   inslot=(bd0==1);

    icount++;
    if(inslot) lle_next64=lle_bt64?(((qword)lle_bthi<<32)|bt0):sext32(bt0);
    else       lle_next64=cpu_pc64get()+4;
    lle_pcnext=(dword)lle_next64;
    lle_br=0;
    op_main(opcode);
    if(!lle_jumped) op_highword(opcode);
    st.g[0].q=0;
    if(lle_jumped)
    { // exception entry or ERET already set the PC
        lle_jumped=0;
        return;
    }
    if(inslot && bt0!=st.pc+4)
    {
        if(st.branchtype==BRANCH_RET && st.memiodetected)
        {
            hw_memio();
            st.memiodetected=0;
        }
        cpu_notify_branch(bt0,st.branchtype);
    }
    switch(lle_br)
    {
    case 1: // taken: the next instruction is the delay slot
        cpu_pc64set(lle_next64);
        st.branchdelay=1;
        st.branchto=(dword)lle_brto64;
        lle_bt64=(lle_brto64!=sext32((dword)lle_brto64));
        lle_bthi=(dword)(lle_brto64>>32);
        break;
    case 2: // not taken: still a delay slot (BD on exceptions)
        cpu_pc64set(lle_next64);
        st.branchdelay=1;
        st.branchto=(dword)(lle_next64+4);
        lle_bt64=cpu_pc64;
        lle_bthi=(dword)((lle_next64+4)>>32);
        break;
    case 3: // likely, not taken: the delay slot is skipped
        cpu_pc64set(lle_next64+4);
        st.branchdelay=0;
        break;
    default:
        if(st.branchdelay!=bd0 || st.branchto!=bt0)
        { // an HLE patch (patch.c) asked to continue at branchto
            cpu_pc64set(sext32(st.branchto));
            st.branchdelay=0;
            break;
        }
        cpu_pc64set(lle_next64);
        st.branchdelay=0;
        break;
    }
}

void c_execop(dword opcode)
{
    if(st.lleos)
    {
        c_execop_lle(opcode);
        return;
    }
    icount++;
    op_main(opcode);

    if(!lle_jumped) op_highword(opcode);

    // $zero: the handlers write their destination unconditionally (a load
    // still reads, so its side effects and faults happen), and the result
    // is discarded here, before the next instruction can read it
    st.g[0].q=0;

    if(lle_jumped)
    { // LLE exception entry or ERET already set the PC
        lle_jumped=0;
        return;
    }

    st.pc+=4;

    if(st.branchdelay>0)
    {
        if(!--st.branchdelay)
        {
            if(st.branchtype==BRANCH_RET && st.memiodetected)
            {
                hw_memio();
                st.memiodetected=0;
            }
            cpu_notify_branch(st.branchto,st.branchtype);
            st.pc=st.branchto;
        }
    }
}

// supervisor/user mode instruction fetch outside the mode's segments: AdEL
static int op_fetchbad(void)
{
    int m=cpu_mode();
    if(st.pc<0x80000000u) return(0);
    if(m==1 && st.pc>=0xC0000000u && st.pc<0xE0000000u) return(0);
    op_adderror((qword)(qint)(int)st.pc,0);
    return(1);
}

void c_exec(void)
{
    while(st.bailout>0)
    {
        if(cpu_pc64)
        { // a 64-bit PC (xkseg, xkphys...): the data access checks and TLB
            int pa=(int)st.pc;
            if(op_checkaddr(cpu_pc64get(),0,&pa,3))
            {
                lle_jumped=0;
                st.bailout--;
                continue;
            }
            c_execop(physnone?0:cache_fetch((dword)pa));
            st.bailout--;
            cpu_notify_pc(st.pc);
            continue;
        }
        if((st.lleos && (st.mmu[12].d&0x18) && !(st.mmu[12].d&6) && op_fetchbad()) ||
           LLE_TLBMISS(st.pc,0) || op_misaligned(st.pc,3,0))
        { // instruction fetch missed or misaligned: the handler runs next
            lle_jumped=0;
            st.bailout--;
            continue;
        }
        c_execop(st.lleos?cache_fetch(cpu_re()?st.pc^4:st.pc):mem_readop(st.pc)); // Status.RE: ^4 (cpu_re)
        st.bailout--;
        cpu_notify_pc(st.pc);
    }
}
