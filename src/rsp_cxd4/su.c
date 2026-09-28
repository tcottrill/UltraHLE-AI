/******************************************************************************\
* Project:  MSP Simulation Layer for Scalar Unit Operations                    *
* Authors:  Iconoclast                                                         *
* Release:  2019.08.06                                                         *
* License:  CC0 Public Domain Dedication                                       *
*                                                                              *
* To the extent possible under law, the author(s) have dedicated all copyright *
* and related and neighboring rights to this software to the public domain     *
* worldwide. This software is distributed without any warranty.                *
*                                                                              *
* You should have received a copy of the CC0 Public Domain Dedication along    *
* with this software.                                                          *
* If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.             *
\******************************************************************************/

#include "su.h"

/*
 * including modular interface structure to access configuration settings...
 * Some of the parallel timing features require perfect timing or configs.
 */
#include "module.h"

/* memcpy() and memset() in SP DMA */
#include <string.h>

u32 inst_word;

u32 SR[NUMBER_OF_SCALAR_REGISTERS];
typedef VECTOR_OPERATION(*p_vector_func)(v16, v16);

pu8 DRAM;
pu8 DMEM;
pu8 IMEM;
unsigned long su_max_address = 0x007FFFFFul;

static int temp_PC;

NOINLINE void res_S(void)
{
    message("RESERVED.");
    return;
}

void set_PC(unsigned int address)
{
    temp_PC = 0x04001000 + FIT_IMEM(address);
#ifndef EMULATE_STATIC_PC
    stage = 1;
#endif
    return;
}

pu32 CR[NUMBER_OF_CP0_REGISTERS];
u8 conf[32];

int MF_SP_STATUS_TIMEOUT;

void SP_CP0_MF(unsigned int rt, unsigned int rd)
{
    SR[rt] = *(CR[rd %= NUMBER_OF_CP0_REGISTERS]);
    SR[zero] = 0x00000000;
    if (rd == 0x7) {
     /* UltraHLE: reading the semaphore sets it, as on hardware, without
      * the plugin halt hack below (see rsp_cxd4/UPSTREAM). */
        GET_RCP_REG(SP_SEMAPHORE_REG) = 0x00000001;
        if (CFG_MEND_SEMAPHORE_LOCK == 0)
            return;
        GET_RCP_REG(SP_SEMAPHORE_REG) = 0x00000001;
        GET_RCP_REG(SP_STATUS_REG) |= SP_STATUS_HALT; /* temporary hack */
        return;
    }
#ifdef WAIT_FOR_CPU_HOST
    if (rd == 0x4) {
        MFC0_count[rt] += 1;
        GET_RCP_REG(SP_STATUS_REG) |= (MFC0_count[rt] >= MF_SP_STATUS_TIMEOUT);
    }
#endif
    return;
}

static void MT_DMA_CACHE(unsigned int rt)
{
    *CR[0x0] = SR[rt] & 0xFFFFFFF8ul; /* & 0x00001FF8 */
    return; /* Reserved upper bits are ignored during DMA R/W. */
}
static void MT_DMA_DRAM(unsigned int rt)
{
    *CR[0x1] = SR[rt] & 0xFFFFFFF8ul; /* & 0x00FFFFF8 */
    return; /* Let the reserved bits get sent, but the pointer is 24-bit. */
}
static void MT_DMA_READ_LENGTH(unsigned int rt)
{
    *CR[0x2] = SR[rt] | 07;
    SP_DMA_READ();
    return;
}
static void MT_DMA_WRITE_LENGTH(unsigned int rt)
{
    *CR[0x3] = SR[rt] | 07;
    SP_DMA_WRITE();
    return;
}
static void MT_SP_STATUS(unsigned int rt)
{
    pu32 MI_INTR_REG;
    pu32 SP_STATUS_REG;

    if (SR[rt] & 0xFE000040)
        message("MTC0\nSP_STATUS"); /* bits we don't know what to do with */
    static const u32 flag[11] = { /* halt, single step, intr on break, SIG0..7 */
        0x0001, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0x0800,
        0x1000, 0x2000, 0x4000,
    };
    static const int clrbit[11] = { 0, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23 };
    const u32 v = SR[rt];
    int i;

    SP_STATUS_REG = GET_RSP_INFO(SP_STATUS_REG);

 /* UltraHLE: each flag changes only when one of its clear/set bits is
  * written, not both (ares io.cpp; n64-systemtest "SP Set/Clear Signal").
  * SET_INTR no longer sets HALT. */
    if (v & 0x00000004)
        *SP_STATUS_REG &= ~0x00000002u; /* clear BROKE */
    for (i = 0; i < 11; i++) {
        const u32 two = (v >> clrbit[i]) & 3;

        if (two == 1)
            *SP_STATUS_REG &= ~flag[i];
        if (two == 2)
            *SP_STATUS_REG |= flag[i];
    }

    MI_INTR_REG = GET_RSP_INFO(MI_INTR_REG);
    if ((v & 0x00000018) == 0x00000008)
        *MI_INTR_REG &= ~0x00000001u; /* SP_CLR_INTR */
    if ((v & 0x00000018) == 0x00000010)
        *MI_INTR_REG |=  0x00000001u; /* SP_SET_INTR */
    return;
}
static void MT_SP_RESERVED(unsigned int rt)
{
    const u32 source = SR[rt] & 0x00000000ul; /* forced (zilmar, dox) */

    GET_RCP_REG(SP_SEMAPHORE_REG) = source;
    return;
}
static void MT_CMD_START(unsigned int rt)
{
    const u32 source = SR[rt] & 0xFFFFFFF8ul; /* Funnelcube demo by marshallh */

    if (GET_RCP_REG(DPC_BUFBUSY_REG)) /* lock hazards not implemented */
        message("MTC0\nCMD_START");
 /* UltraHLE: as from the CPU (lle.c), START only latches (START_VALID);
  * the END write moves it to CURRENT. Writes while START_VALID is set are
  * ignored. */
    if (!(GET_RCP_REG(DPC_STATUS_REG) & 0x00000400ul)) {
        GET_RCP_REG(DPC_START_REG) = source & 0x00FFFFF8ul;
        GET_RCP_REG(DPC_STATUS_REG) |= 0x00000400ul;
    }
    return;
}
static void MT_CMD_END(unsigned int rt)
{
    if (GET_RCP_REG(DPC_BUFBUSY_REG))
        message("MTC0\nCMD_END"); /* This is just CA-related. */
    GET_RCP_REG(DPC_END_REG) = SR[rt] & 0x00FFFFF8ul;
    if (GET_RCP_REG(DPC_STATUS_REG) & 0x00000400ul) {
        GET_RCP_REG(DPC_CURRENT_REG) = GET_RCP_REG(DPC_START_REG);
        GET_RCP_REG(DPC_STATUS_REG) &= ~0x00000400ul;
    }
    GET_RCP_REG(DPC_STATUS_REG) |= 0x000000A8ul; /* busy until SYNC_FULL */
    GBI_phase();
    return;
}
static void MT_CMD_STATUS(unsigned int rt)
{
    pu32 DPC_STATUS_REG;

    if (SR[rt] & 0xFFFFFD80ul) /* unsupported or reserved bits */
        message("MTC0\nCMD_STATUS");
    DPC_STATUS_REG = GET_RSP_INFO(DPC_STATUS_REG);

    *DPC_STATUS_REG &= ~(!!(SR[rt] & 0x00000001) << 0);
    *DPC_STATUS_REG |=  (!!(SR[rt] & 0x00000002) << 0);
    *DPC_STATUS_REG &= ~(!!(SR[rt] & 0x00000004) << 1);
    *DPC_STATUS_REG |=  (!!(SR[rt] & 0x00000008) << 1);
    *DPC_STATUS_REG &= ~(!!(SR[rt] & 0x00000010) << 2);
    *DPC_STATUS_REG |=  (!!(SR[rt] & 0x00000020) << 2);
/* Some NUS-CIC-6105 SP tasks try to clear some DPC cycle timers. */
    GET_RCP_REG(DPC_TMEM_REG)     &= !(SR[rt] & 0x00000040) ? ~0u : 0u;
 /* GET_RCP_REG(DPC_PIPEBUSY_REG) &= !(SR[rt] & 0x00000080) ? ~0u : 0u; */
 /* GET_RCP_REG(DPC_BUFBUSY_REG)  &= !(SR[rt] & 0x00000100) ? ~0u : 0u; */
    GET_RCP_REG(DPC_CLOCK_REG)    &= !(SR[rt] & 0x00000200) ? ~0u : 0u;
    return;
}
static void MT_CMD_CLOCK(unsigned int rt)
{
    message("MTC0\nCMD_CLOCK"); /* read-only?? */
    GET_RCP_REG(DPC_CLOCK_REG) = SR[rt];
    return; /* Appendix says this is RW; elsewhere it says R. */
}
static void MT_READ_ONLY(unsigned int rt)
{
    static char write_to_read_only[] = "Invalid MTC0 from SR[00].";

    write_to_read_only[21] = '0' + (unsigned char)rt/10;
    write_to_read_only[22] = '0' + (unsigned char)rt%10;
    message(write_to_read_only);
    return;
}

static void (*SP_CP0_MT[NUMBER_OF_CP0_REGISTERS])(unsigned int) = {
MT_DMA_CACHE       ,MT_DMA_DRAM        ,MT_DMA_READ_LENGTH ,MT_DMA_WRITE_LENGTH,
MT_SP_STATUS       ,MT_READ_ONLY       ,MT_READ_ONLY       ,MT_SP_RESERVED,
MT_CMD_START       ,MT_CMD_END         ,MT_READ_ONLY       ,MT_CMD_STATUS,
MT_CMD_CLOCK       ,MT_READ_ONLY       ,MT_READ_ONLY       ,MT_READ_ONLY
};

/*
 * UltraHLE: the registers after a DMA of `rows` rows of `length` bytes,
 * `skip` = length + the skip field (ares dma.cpp; n64-systemtest "SP-Address
 * after DMA"): SP_MEM_ADDR advances within DMEM or IMEM, DRAM_ADDR by every
 * row plus the skips between rows, and the length register reads 0xFF8
 * with a count of 0.
 */
static void sp_dma_done(unsigned rows, unsigned length, unsigned skip,
    pu32 len_reg)
{
    const u32 moved = rows * length;

    *CR[0x0] = (*CR[0x0] & 0x00001000ul) | ((*CR[0x0] + moved) & 0x00000FF8ul);
    *CR[0x1] = ((*CR[0x1] & 0x00FFFFF8ul) + moved + (rows - 1)*(skip - length))
             & 0x00FFFFF8ul;
    *len_reg = (*len_reg & 0xFFF00000ul) | 0x00000FF8ul;
}

void SP_DMA_READ(void)
{
    unsigned int offC, offD; /* SP cache and dynamic DMA pointers */
    register unsigned int length;
    register unsigned int count;
    register unsigned int skip;
    unsigned int rows;

    length = (GET_RCP_REG(SP_RD_LEN_REG) & 0x00000FFFul) >>  0;
    count  = (GET_RCP_REG(SP_RD_LEN_REG) & 0x000FF000ul) >> 12;
    skip   = (GET_RCP_REG(SP_RD_LEN_REG) & 0xFFF00000ul) >> 20;
#ifdef _DEBUG
    length |= 07; /* already corrected by mtc0 */
#endif
    ++length;
    ++count;
    skip += length;
    rows = count;
    do {
        register unsigned int i;

        i = 0;
        --count;
        do {
            offC = ((count*length + *CR[0x0] + i) & 0x00000FF8ul)
                 | (*CR[0x0] & 0x00001000ul);
            offD = (count*skip + *CR[0x1] + i) & 0x00FFFFF8ul;
            i += 0x008;
            if (offD > su_max_address) {
                memset(DMEM + offC, 0x00, 8);
                continue;
            }
            memcpy(DMEM + offC, DRAM + offD, 8);
        } while (i < length);
    } while (count);
    sp_dma_done(rows, length, skip, CR[0x2]);

    GET_RCP_REG(SP_DMA_BUSY_REG)  =  0x00000000;
    GET_RCP_REG(SP_STATUS_REG)   &= ~SP_STATUS_DMA_BUSY;
    return;
}
void SP_DMA_WRITE(void)
{
    unsigned int offC, offD; /* SP cache and dynamic DMA pointers */
    register unsigned int length;
    register unsigned int count;
    register unsigned int skip;
    unsigned int rows;

    length = (GET_RCP_REG(SP_WR_LEN_REG) & 0x00000FFFul) >>  0;
    count  = (GET_RCP_REG(SP_WR_LEN_REG) & 0x000FF000ul) >> 12;
    skip   = (GET_RCP_REG(SP_WR_LEN_REG) & 0xFFF00000ul) >> 20;

#ifdef _DEBUG
    length |= 07; /* already corrected by mtc0 */
#endif
    ++length;
    ++count;
    skip += length;
    rows = count;
    do {
        register unsigned int i;

        i = 0;
        --count;
        do {
            offC = ((count*length + *CR[0x0] + i) & 0x00000FF8ul)
                 | (*CR[0x0] & 0x00001000ul);
            offD = (count*skip + *CR[0x1] + i) & 0x00FFFFF8ul;
            i += 0x000008;
            if (offD > su_max_address)
                continue;
            memcpy(DRAM + offD, DMEM + offC, 8);
        } while (i < length);
    } while (count);
    sp_dma_done(rows, length, skip, CR[0x3]);

    GET_RCP_REG(SP_DMA_BUSY_REG)  =  0x00000000;
    GET_RCP_REG(SP_STATUS_REG)   &= ~SP_STATUS_DMA_BUSY;
    return;
}

/*** scalar, R4000 control flow manipulation ***/

PROFILE_MODE void J(u32 inst)
{
    set_PC(4 * inst);
}
PROFILE_MODE void JAL(u32 inst, u32 PC)
{
    SR[ra] = FIT_IMEM(PC + LINK_OFF);
    set_PC(4 * inst);
}

PROFILE_MODE int BEQ(u32 inst, u32 PC)
{
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    if (!(SR[rs] == SR[rt]))
        return 0;
    set_PC(PC + 4*inst + SLOT_OFF);
    return 1;
}
PROFILE_MODE int BNE(u32 inst, u32 PC)
{
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    if (!(SR[rs] != SR[rt]))
        return 0;
    set_PC(PC + 4*inst + SLOT_OFF);
    return 1;
}
PROFILE_MODE int BLEZ(u32 inst, u32 PC)
{
    const unsigned int rs = (inst >> 21) % (1 << 5);

    if (!((s32)SR[rs] <= 0))
        return 0;
    set_PC(PC + 4*inst + SLOT_OFF);
    return 1;
}
PROFILE_MODE int BGTZ(u32 inst, u32 PC)
{
    const unsigned int rs = (inst >> 21) % (1 << 5);

    if (!((s32)SR[rs] >  0))
        return 0;
    set_PC(PC + 4*inst + SLOT_OFF);
    return 1;
}

/*** scalar, R4000 bit-wise logical operations ***/

PROFILE_MODE void ANDI(u32 inst)
{
    const u16 immediate = (u16)(inst & 0x0000FFFFu);
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    SR[rt] = SR[rs] & immediate;
    SR[zero] = 0x00000000;
}
PROFILE_MODE void ORI(u32 inst)
{
    const u16 immediate = (u16)(inst & 0x0000FFFFu);
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    SR[rt] = SR[rs] | immediate;
    SR[zero] = 0x00000000;
}
PROFILE_MODE void XORI(u32 inst)
{
    const u16 immediate = (u16)(inst & 0x0000FFFFu);
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    SR[rt] = SR[rs] ^ immediate;
    SR[zero] = 0x00000000;
}
PROFILE_MODE void LUI(u32 inst)
{
    const u16 immediate = (u16)(inst & 0x0000FFFFu);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    SR[rt] = (u32)immediate << 16; /* or:  SR[rt] = 0; SR[rt]31..16 = imm; */
    SR[zero] = 0x00000000;
}

/*** scalar, R4000 arithmetic operations ***/

PROFILE_MODE void ADDIU(u32 inst)
{
    const u16 immediate = (u16)(inst & 0x0000FFFFu);
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    SR[rt] = SR[rs] + (s16)(immediate);
    SR[zero] = 0x00000000;
}
PROFILE_MODE void SLTI(u32 inst)
{
    const u16 immediate = (u16)(inst & 0x0000FFFFu);
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    SR[rt] = ((s32)(SR[rs]) < (s32)SIGNED_IMM16(immediate)) ? 1 : 0;
    SR[zero] = 0x00000000;
}
PROFILE_MODE void SLTIU(u32 inst)
{
    const u16 immediate = (u16)(inst & 0x0000FFFFu);
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    SR[rt] = ((u32)(SR[rs]) < (u32)SIGNED_IMM16(immediate)) ? 1 : 0;
    SR[zero] = 0x00000000;
}

/*** scalar, R4000 memory loads and stores ***/

PROFILE_MODE void LB(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    SR[rt] = DMEM[BES(addr) & 0x00000FFFul];
    SR[rt] = (s8)SR[rt];
    SR[zero] = 0x00000000;
}
PROFILE_MODE void LH(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    SR[rt] = 0x00000000
      | DMEM[BES(addr + 0) & 0x00000FFFul] <<  8
      | DMEM[BES(addr + 1) & 0x00000FFFul] <<  0
    ;
    SR[rt] = (s16)SR[rt];
    SR[zero] = 0x00000000;
}
PROFILE_MODE void LW(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    SR_B(rt, 0) = DMEM[BES(addr + 0) & 0x00000FFFul];
    SR_B(rt, 1) = DMEM[BES(addr + 1) & 0x00000FFFul];
    SR_B(rt, 2) = DMEM[BES(addr + 2) & 0x00000FFFul];
    SR_B(rt, 3) = DMEM[BES(addr + 3) & 0x00000FFFul];
    SR[zero] = 0x00000000;
}
PROFILE_MODE void LBU(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    SR[rt] = DMEM[BES(addr) & 0x00000FFFul];
    SR[zero] = 0x00000000;
}
PROFILE_MODE void LHU(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    SR[rt] = 0x00000000
      | DMEM[BES(addr + 0) & 0x00000FFFul] <<  8
      | DMEM[BES(addr + 1) & 0x00000FFFul] <<  0
    ;
    SR[zero] = 0x00000000;
}

PROFILE_MODE void SB(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    DMEM[BES(addr) & 0x00000FFFul] = (u8)(SR[rt] & 0xFFu);
}
PROFILE_MODE void SH(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    DMEM[BES(addr + 0) & 0x00000FFFul] = SR_B(rt, 2);
    DMEM[BES(addr + 1) & 0x00000FFFul] = SR_B(rt, 3);
}
PROFILE_MODE void SW(u32 inst)
{
    u32 addr;
    const s16 offset = (s16)(inst & 0x0000FFFFul);
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);

    addr = SR[base] + offset;
    DMEM[BES(addr + 0) & 0x00000FFFul] = SR_B(rt, 0);
    DMEM[BES(addr + 1) & 0x00000FFFul] = SR_B(rt, 1);
    DMEM[BES(addr + 2) & 0x00000FFFul] = SR_B(rt, 2);
    DMEM[BES(addr + 3) & 0x00000FFFul] = SR_B(rt, 3);
}

/*** scalar, coprocessor operations (vector unit) ***/

u16 rwR_VCE(void)
{ /* never saw a game try to read VCE out to a scalar GPR yet */
    register u16 ret_slot;

    ret_slot = 0x00 | (u16)get_VCE();
    return (ret_slot);
}
void rwW_VCE(u16 vce)
{ /* never saw a game try to write VCE using a scalar GPR yet */
    register int i;

    vce = 0x00 | (vce & 0xFF);
    for (i = 0; i < 8; i++)
        cf_vce[i] = (vce >> i) & 1;
    return;
}

static u16 (*R_VCF[4])(void) = {
    get_VCO,get_VCC,rwR_VCE,rwR_VCE,
};
static void (*W_VCF[4])(u16) = {
    set_VCO,set_VCC,rwW_VCE,rwW_VCE,
};
void MFC2(unsigned int rt, unsigned int vs, unsigned int e)
{
    SR_B(rt, 2) = VR_B(vs, e);
    e = (e + 0x1) & 0xF;
    SR_B(rt, 3) = VR_B(vs, e);
    SR[rt] = (s16)(SR[rt]);
    SR[zero] = 0x00000000;
    return;
}
void MTC2(unsigned int rt, unsigned int vd, unsigned int e)
{
    VR_B(vd, e+0x0) = SR_B(rt, 2);
    if (e != 0xF) /* UltraHLE: byte 16 would be the next register's (ares) */
        VR_B(vd, e+0x1) = SR_B(rt, 3);
    return;
}
void CFC2(unsigned int rt, unsigned int rd)
{
    SR[rt] = (s16)R_VCF[rd & 3]();
    SR[zero] = 0x00000000;
    return;
}
void CTC2(unsigned int rt, unsigned int rd)
{
    W_VCF[rd & 3](SR[rt] & 0x0000FFFF);
    return;
}

/*** scalar, coprocessor operations (vector unit, scalar cache transfers) ***/

/*
 * UltraHLE: the vector loads and stores below replace cxd4's unrolled ones.
 * They are transcribed from ares (ares/n64/rsp/interpreter-vpu.cpp, commit
 * 4cb8d92b), a byte at a time: DMEM addresses wrap at 4 KB and every
 * element/address combination is handled (n64-systemtest RSP load/store
 * tests). VR_B is ares's r128::byte(), VR[vt][n] its element(n).
 */
#define DMB(a)  DMEM[BES((u32)(a) & 0x00000FFF)]

NOINLINE void
res_lsw(unsigned vt, unsigned element, signed offset, unsigned base)
{
    message("Reserved vector unit transfer operation.");
    if (vt != element + base || offset != 0) /* unused parameters */
        return;
    return;
}

static void load_bytes(unsigned vt, int start, int end, u32 addr)
{
    int i;

    for (i = start; i < end; i++)
        VR_B(vt, i & 0xF) = DMB(addr++);
}

static void store_bytes(unsigned vt, int start, int end, u32 addr)
{
    int i;

    for (i = start; i < end; i++)
        DMB(addr++) = VR_B(vt, i & 0xF);
}

void LBV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    VR_B(vt, element) = DMB(SR[base] + offset);
}
void LSV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    const int e = element;

    load_bytes(vt, e, e + 2 < 16 ? e + 2 : 16, SR[base] + 2*offset);
}
void LLV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    const int e = element;

    load_bytes(vt, e, e + 4 < 16 ? e + 4 : 16, SR[base] + 4*offset);
}
void LDV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    const int e = element;

    load_bytes(vt, e, e + 8 < 16 ? e + 8 : 16, SR[base] + 8*offset);
}
void LQV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    const u32 addr = SR[base] + 16*offset;
    const int e = element;
    int end;

    end = 16 + e - (int)(addr & 0xF);
    load_bytes(vt, e, end < 16 ? end : 16, addr);
}
void LRV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    const u32 addr = SR[base] + 16*offset;
    const int e = element;

    load_bytes(vt, 16 - ((int)(addr & 0xF) - e), 16, addr & ~0xFu);
}
static void load_packed(unsigned vt, unsigned element, u32 addr, int step,
    int shift)
{
    const int index = (int)(addr & 7) - (int)element;
    int i;

    addr &= ~7u;
    for (i = 0; i < 8; i++)
        VR[vt][i] = (i16)(DMB(addr + ((index + i*step) & 0xF)) << shift);
}
void LPV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    load_packed(vt, element, SR[base] + 8*offset, 1, 8);
}
void LUV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    load_packed(vt, element, SR[base] + 8*offset, 1, 7);
}
void LHV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    load_packed(vt, element, SR[base] + 16*offset, 2, 7);
}
void LFV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    u32 addr = SR[base] + 16*offset;
    const int e = element;
    const int index = (int)(addr & 7) - e;
    const int end = e + 8 < 16 ? e + 8 : 16;
    unsigned char tmp[16];
    int i;

    addr &= ~7u;
    for (i = 0; i < 4; i++) {
        u16 lo = (u16)(DMB(addr + ((index + i*4 + 0) & 0xF)) << 7);
        u16 hi = (u16)(DMB(addr + ((index + i*4 + 8) & 0xF)) << 7);

        tmp[2*i + 0] = (unsigned char)(lo >> 8);
        tmp[2*i + 1] = (unsigned char)lo;
        tmp[2*i + 8] = (unsigned char)(hi >> 8);
        tmp[2*i + 9] = (unsigned char)hi;
    }
    for (i = e; i < end; i++)
        VR_B(vt, i) = tmp[i];
}
void LTV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    u32 addr = SR[base] + 16*offset;
    const u32 begin = addr & ~7u;
    const unsigned vtbase = vt & ~7u;
    unsigned vtoff = element >> 1;
    int i;

    addr = begin + ((element + (addr & 8)) & 0xF);
    for (i = 0; i < 8; i++) {
        VR_B(vtbase + vtoff, 2*i + 0) = DMB(addr++);
        if (addr == begin + 16)
            addr = begin;
        VR_B(vtbase + vtoff, 2*i + 1) = DMB(addr++);
        if (addr == begin + 16)
            addr = begin;
        vtoff = (vtoff + 1) & 7;
    }
}

void SBV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    DMB(SR[base] + offset) = VR_B(vt, element);
}
void SSV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    store_bytes(vt, element, element + 2, SR[base] + 2*offset);
}
void SLV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    store_bytes(vt, element, element + 4, SR[base] + 4*offset);
}
void SDV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    store_bytes(vt, element, element + 8, SR[base] + 8*offset);
}
void SQV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    const u32 addr = SR[base] + 16*offset;

    store_bytes(vt, element, element + (16 - (int)(addr & 0xF)), addr);
}
void SRV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    u32 addr = SR[base] + 16*offset;
    const int e = element;
    const int end = e + (int)(addr & 0xF);
    const int shift = 16 - (int)(addr & 0xF);
    int i;

    addr &= ~0xFu;
    for (i = e; i < end; i++)
        DMB(addr++) = VR_B(vt, (i + shift) & 0xF);
}
/* SPV/SUV: the upper byte (SPV) or the 7-bit-shifted value (SUV) of each
 * element, swapping roles for the second half of the element range */
static void store_packed(unsigned vt, unsigned element, u32 addr, int upper)
{
    int i;

    for (i = element; i < (int)element + 8; i++) {
        if (((i & 0xF) < 8) == upper)
            DMB(addr++) = VR_B(vt, (i & 7) << 1);
        else
            DMB(addr++) = (u8)((u16)VR[vt][i & 7] >> 7);
    }
}
void SPV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    store_packed(vt, element, SR[base] + 8*offset, 1);
}
void SUV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    store_packed(vt, element, SR[base] + 8*offset, 0);
}
void SHV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    u32 addr = SR[base] + 16*offset;
    const int index = (int)(addr & 7);
    int i;

    addr &= ~7u;
    for (i = 0; i < 8; i++) {
        const int b = element + 2*i;
        const u8 value = (u8)(VR_B(vt, b & 0xF) << 1 | VR_B(vt, (b + 1) & 0xF) >> 7);

        DMB(addr + ((index + 2*i) & 0xF)) = value;
    }
}
void SFV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    static const signed char order[16][4] = {
        { 0, 1, 2, 3}, { 6, 7, 4, 5}, {-1,-1,-1,-1}, {-1,-1,-1,-1},
        { 1, 2, 3, 0}, { 7, 4, 5, 6}, {-1,-1,-1,-1}, {-1,-1,-1,-1},
        { 4, 5, 6, 7}, {-1,-1,-1,-1}, {-1,-1,-1,-1}, { 3, 0, 1, 2},
        { 5, 6, 7, 4}, {-1,-1,-1,-1}, {-1,-1,-1,-1}, { 0, 1, 2, 3},
    };
    u32 addr = SR[base] + 16*offset;
    const int b = (int)(addr & 7);
    int i;

    addr &= ~7u;
    for (i = 0; i < 4; i++) {
        const int n = order[element & 0xF][i];

        DMB(addr + ((b + 4*i) & 0xF)) = (n < 0) ? 0 : (u8)((u16)VR[vt][n] >> 7);
    }
}
void SWV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    u32 addr = SR[base] + 16*offset;
    int b = (int)(addr & 7);
    int i;

    addr &= ~7u;
    for (i = element; i < (int)element + 16; i++)
        DMB(addr + (b++ & 0xF)) = VR_B(vt, i & 0xF);
}
void STV(unsigned vt, unsigned element, signed offset, unsigned base)
{
    u32 addr = SR[base] + 16*offset;
    const unsigned start = vt & ~7u;
    int el = 16 - (int)(element & ~1u);
    int b = (int)(addr & 7) - (int)(element & ~1u);
    unsigned r;

    addr &= ~7u;
    for (r = start; r < start + 8; r++) {
        DMB(addr + (b++ & 0xF)) = VR_B(r, el++ & 0xF);
        DMB(addr + (b++ & 0xF)) = VR_B(r, el++ & 0xF);
    }
}

#ifdef WAIT_FOR_CPU_HOST
short MFC0_count[NUMBER_OF_SCALAR_REGISTERS];
#endif

mwc2_func LWC2[2 * 8*2] = {
    LBV    ,LSV    ,LLV    ,LDV    ,LQV    ,LRV    ,LPV    ,LUV    ,
    LHV    ,LFV    ,res_lsw,LTV    ,res_lsw,res_lsw,res_lsw,res_lsw,
    res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,
    res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,
};
mwc2_func SWC2[2 * 8*2] = {
    SBV    ,SSV    ,SLV    ,SDV    ,SQV    ,SRV    ,SPV    ,SUV    ,
    SHV    ,SFV    ,SWV    ,STV    ,res_lsw,res_lsw,res_lsw,res_lsw,
    res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,
    res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,res_lsw,
};


PROFILE_MODE int SPECIAL(u32 inst, u32 PC)
{
    unsigned int rd, rs, rt;

    rd = IW_RD(inst);
    rt = (inst >> 16) % (1 << 5);

    switch (inst % 64) {
    case 000: /* SLL */
        SR[rd] = SR[rt] << MASK_SA(inst >> 6);
        SR[zero] = 0x00000000;
        break;
    case 002: /* SRL */
        SR[rd] = (u32)(SR[rt]) >> MASK_SA(inst >> 6);
        SR[zero] = 0x00000000;
        break;
    case 003: /* SRA */
        SR[rd] = (s32)(SR[rt]) >> MASK_SA(inst >> 6);
        SR[zero] = 0x00000000;
        break;
    case 004: /* SLLV */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = SR[rt] << MASK_SA(SR[rs]);
        SR[zero] = 0x00000000;
        break;
    case 006: /* SRLV */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = (u32)(SR[rt]) >> MASK_SA(SR[rs]);
        SR[zero] = 0x00000000;
        break;
    case 007: /* SRAV */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = (s32)(SR[rt]) >> MASK_SA(SR[rs]);
        SR[zero] = 0x00000000;
        break;
    case 011: /* JALR */
        rs = SPECIAL_DECODE_RS(inst);
        set_PC(SR[rs]); /* UltraHLE: the target before the link (rd == rs) */
        SR[rd] = FIT_IMEM(PC + LINK_OFF);
        SR[zero] = 0x00000000;
        return 1;
    case 010: /* JR */
        rs = SPECIAL_DECODE_RS(inst);
        set_PC(SR[rs]);
        return 1;
    case 015: /* BREAK */
        *CR[0x4] |= SP_STATUS_BROKE | SP_STATUS_HALT;
        if (*CR[0x4] & SP_STATUS_INTR_BREAK) {
            GET_RCP_REG(MI_INTR_REG) |= 0x00000001;
            GET_RSP_INFO(CheckInterrupts)();
        }
        return -1;
    case 040: /* ADD */
    case 041: /* ADDU */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = SR[rs] + SR[rt];
        SR[zero] = 0x00000000; /* needed for Rareware micro-codes */
        break;
    case 042: /* SUB */
    case 043: /* SUBU */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = SR[rs] - SR[rt];
        SR[zero] = 0x00000000;
        break;
    case 044: /* AND */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = SR[rs] & SR[rt];
        SR[zero] = 0x00000000; /* needed for Rareware micro-codes */
        break;
    case 045: /* OR */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = SR[rs] | SR[rt];
        SR[zero] = 0x00000000;
        break;
    case 046: /* XOR */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = SR[rs] ^ SR[rt];
        SR[zero] = 0x00000000;
        break;
    case 047: /* NOR */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = ~(SR[rs] | SR[rt]);
        SR[zero] = 0x00000000;
        break;
    case 052: /* SLT */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = ((s32)(SR[rs]) < (s32)(SR[rt]));
        SR[zero] = 0x00000000;
        break;
    case 053: /* SLTU */
        rs = SPECIAL_DECODE_RS(inst);
        SR[rd] = ((u32)(SR[rs]) < (u32)(SR[rt]));
        SR[zero] = 0x00000000;
        break;
    default:
        res_S();
    }
    return 0;
}

PROFILE_MODE int REGIMM(u32 inst, u32 PC)
{
    const unsigned int base = (inst >> 21) % (1 << 5);
    const unsigned int rt   = (inst >> 16) % (1 << 5);
    int taken;

    switch (rt) {
    /* UltraHLE: the AL forms test rs before the link (rs == ra) */
    case 020: /* BLTZAL */
    case 000: /* BLTZ */
        taken = (s32)SR[base] < 0;
        if (rt & 020)
            SR[ra] = FIT_IMEM(PC + LINK_OFF);
        if (!taken)
            return 0;
        set_PC(PC + 4*inst + SLOT_OFF);
        break;
    case 021: /* BGEZAL */
    case 001: /* BGEZ */
        taken = (s32)SR[base] >= 0;
        if (rt & 020)
            SR[ra] = FIT_IMEM(PC + LINK_OFF);
        if (!taken)
            return 0;
        set_PC(PC + 4*inst + SLOT_OFF);
        break;
    default:
        res_S();
    }
    return 1;
}

PROFILE_MODE void MWC2_load(u32 inst)
{
    s16 offset;
    const unsigned int base    = (inst >> 21) % (1 << 5);
    const unsigned int vt      = (inst >> 16) % (1 << 5);
    const unsigned int element = (inst >>  7) % (1 << 4);

#if defined(ARCH_MIN_SSE2) && !defined(SSE2NEON)
    offset   = (s16)inst;
    offset <<= 5 + 4; /* safe on x86, skips 5-bit rd, 4-bit element */
    offset >>= 5 + 4;
#else
    offset = (inst & 64) ? -(s16)(~inst%64 + 1) : (s16)(inst % 64);
#endif
    LWC2[IW_RD(inst)](vt, element, offset, base);
}
PROFILE_MODE void MWC2_store(u32 inst)
{
    s16 offset;
    const unsigned int base    = (inst >> 21) % (1 << 5);
    const unsigned int vt      = (inst >> 16) % (1 << 5);
    const unsigned int element = (inst >>  7) % (1 << 4);

#if defined(ARCH_MIN_SSE2) && !defined(SSE2NEON)
    offset = (s16)inst;
    offset <<= 5 + 4; /* safe on x86, skips 5-bit rd, 4-bit element */
    offset >>= 5 + 4;
#else
    offset = (inst & 64) ? -(s16)(~inst%64 + 1) : (s16)(inst % 64);
#endif
    SWC2[IW_RD(inst)](vt, element, offset, base);
}

PROFILE_MODE void COP0(u32 inst)
{
    const unsigned int rd = IW_RD(inst);
    const unsigned int rs = (inst >> 21) % (1 << 5);
    const unsigned int rt = (inst >> 16) % (1 << 5);

    switch (rs) {
    case 000:
        SP_CP0_MF(rt, rd);
        break;
    case 004:
        SP_CP0_MT[rd % NUMBER_OF_CP0_REGISTERS](rt);
        break;
    default:
        res_S();
    }
}

PROFILE_MODE void COP2(u32 inst)
{
    const unsigned int op = (inst >> 21) % (1 << 5); /* inst.R.rs */
    const unsigned int vt = (inst >> 16) % (1 << 5); /* inst.R.rt */
    const unsigned int vs = IW_RD(inst);
    const unsigned int vd = (inst >>  6) % (1 << 5); /* inst.R.sa */
    const unsigned int func = inst % (1 << 6);
#ifndef ARCH_MIN_SSE2
    const unsigned int e  = op & 0xF; /* With Intel, LEA offsets beat ANDing. */
#endif

    switch (op) {
        static ALIGNED i16 shuffle_temporary[N];
#ifdef ARCH_MIN_SSE2
        v16 target;
#else
        register unsigned int i;
#endif

    case 000:
        MFC2(vt, vs, vd >> 1);
        break;
    case 002:
        CFC2(vt, vs);
        break;
    case 004:
        MTC2(vt, vs, vd >> 1);
        break;
    case 006:
        CTC2(vt, vs);
        break;
    case 020:
    case 021:
#ifdef ARCH_MIN_SSE2
        *(v16 *)(VR[vd]) = COP2_C2[func](*(v16 *)VR[vs], *(v16 *)VR[vt]);
#else
        COP2_C2[func](&VR[vs][0], &VR[vt][0]);
        vector_copy(&VR[vd][0], &V_result[0]);
#endif
        break;
    case 022:
    case 023:
#ifdef ARCH_MIN_SSE2
#ifdef __ARM_NEON__
        target = (v16)vld1q_u16(&VR[vt][0 + op - 0x12]);
        target = (v16)vshlq_n_u32((uint32x4_t)target, 16);
        target = (v16)vorrq_u16((uint16x8_t)target,
                                (uint16x8_t)vshrq_n_u32((uint32x4_t)target, 16));
#else
        shuffle_temporary[0] = VR[vt][0 + op - 0x12];
        shuffle_temporary[2] = VR[vt][2 + op - 0x12];
        shuffle_temporary[4] = VR[vt][4 + op - 0x12];
        shuffle_temporary[6] = VR[vt][6 + op - 0x12];
        target = *(v16 *)(&shuffle_temporary[0]);
        target = _mm_shufflehi_epi16(target, _MM_SHUFFLE(2, 2, 0, 0));
        target = _mm_shufflelo_epi16(target, _MM_SHUFFLE(2, 2, 0, 0));
#endif
        *(v16 *)(VR[vd]) = COP2_C2[func](*(v16 *)VR[vs], target);
#else
        for (i = 0; i < N; i++)
            shuffle_temporary[i] = VR[vt][(i & 0xE) + (e & 0x1)];
        COP2_C2[func](&VR[vs][0], &shuffle_temporary[0]);
        vector_copy(&VR[vd][0], &V_result[0]);
#endif
        break;
    case 024:
    case 025:
    case 026:
    case 027:
#ifdef ARCH_MIN_SSE2
#ifdef __ARM_NEON__
        target = (v16)vcombine_s16(vdup_n_s16(VR[vt][0 + op - 0x14]),
                                   vdup_n_s16(VR[vt][4 + op - 0x14]));
#else
        target = _mm_setzero_si128();
        target = _mm_insert_epi16(target, VR[vt][0 + op - 0x14], 0);
        target = _mm_insert_epi16(target, VR[vt][4 + op - 0x14], 4);
        target = _mm_shufflehi_epi16(target, _MM_SHUFFLE(0, 0, 0, 0));
        target = _mm_shufflelo_epi16(target, _MM_SHUFFLE(0, 0, 0, 0));
#endif
        *(v16 *)(VR[vd]) = COP2_C2[func](*(v16 *)VR[vs], target);
#else
        for (i = 0; i < N; i++)
            shuffle_temporary[i] = VR[vt][(i & 0xC) + (e & 0x3)];
        COP2_C2[func](&VR[vs][0], &shuffle_temporary[0]);
        vector_copy(&VR[vd][0], &V_result[0]);
#endif
        break;
    case 030:
    case 031:
    case 032:
    case 033:
    case 034:
    case 035:
    case 036:
    case 037:
#ifdef ARCH_MIN_SSE2
        *(v16 *)(VR[vd]) = COP2_C2[func](
            *(v16 *)VR[vs],
            _mm_set1_epi16(VR[vt][op - 0x18])
        );
#else
        for (i = 0; i < N; i++)
            shuffle_temporary[i] = VR[vt][e % N];
        COP2_C2[func](&VR[vs][0], &shuffle_temporary[0]);
        vector_copy(&VR[vd][0], &V_result[0]);
#endif
        break;
    default:
        res_S();
    }
}

/* UltraHLE: instructions one run_task call may execute (0: no limit). A
 * run that uses them up returns with the RSP still running (cxd4_budgetout)
 * so it can go on beside the CPU (see rsp_cxd4/UPSTREAM). */
int cxd4_budget;
int cxd4_budgetout;

NOINLINE void run_task(void)
{
    register u32 PC;
    int left = cxd4_budget;

    cxd4_budgetout = 0;
    PC = FIT_IMEM(GET_RCP_REG(SP_PC_REG));
    for (;;) {
#ifndef EMULATE_STATIC_PC
        if (cxd4_budget != 0 && stage == 0 && --left < 0) {
#else
     /* static PC: delay slots run inline (goto EX), never from here */
        if (cxd4_budget != 0 && --left < 0) {
#endif
            cxd4_budgetout = 1;
            GET_RCP_REG(SP_PC_REG) = 0x04001000 | FIT_IMEM(PC);
            return;
        }
        inst_word = *(pi32)(IMEM + FIT_IMEM(PC));
#ifdef EMULATE_STATIC_PC
        PC = (PC + 0x004);
EX:
#endif
#ifdef SP_EXECUTE_LOG
        step_SP_commands(inst_word);
#endif

#if (0 != 0)
        if (GET_RCP_REG(SP_STATUS_REG) & SP_STATUS_HALT)
            goto RSP_halted_CPU_exit_point; /* Only BREAK and COP0 set this. */
        SR[zero] = 0x00000000; /* already handled on per-instruction basis */
#endif
        switch (inst_word >> 26) {
        case 000: /* SPECIAL */
            switch (SPECIAL(inst_word, PC)) {
            case -1: /* BREAK */
                goto RSP_halted_CPU_exit_point;
            case +1: /* JR and JALR */
                JUMP;
            }
            break;
        case 001: /* REGIMM */
            if (REGIMM(inst_word, PC) != 0)
                JUMP;
            break;
        case 002:
            J(inst_word);
            JUMP;
        case 003:
            JAL(inst_word, PC);
            JUMP;
        case 004:
            if (BEQ(inst_word, PC) != 0)
                JUMP;
            break;
        case 005:
            if (BNE(inst_word, PC) != 0)
                JUMP;
            break;
        case 006:
            if (BLEZ(inst_word, PC) != 0)
                JUMP;
            break;
        case 007:
            if (BGTZ(inst_word, PC) != 0)
                JUMP;
            break;
        case 010: /* ADDI:  Traps don't exist on the RCP. */
        case 011:
            ADDIU(inst_word);
            break;
        case 012:
            SLTI(inst_word);
            break;
        case 013:
            SLTIU(inst_word);
            break;
        case 014:
            ANDI(inst_word);
            break;
        case 015:
            ORI(inst_word);
            break;
        case 016:
            XORI(inst_word);
            break;
        case 017:
            LUI(inst_word);
            break;
        case 020:
            COP0(inst_word);
            if (GET_RCP_REG(SP_STATUS_REG) & SP_STATUS_HALT)
                goto RSP_halted_CPU_exit_point;
            break;
        case 022:
            COP2(inst_word);
            break;
        case 040:
            LB(inst_word);
            break;
        case 041:
            LH(inst_word);
            break;
        case 043:
        case 047: /* LWU: UltraHLE, the same as LW with 32-bit registers */
            LW(inst_word);
            break;
        case 044:
            LBU(inst_word);
            break;
        case 045:
            LHU(inst_word);
            break;
        case 050:
            SB(inst_word);
            break;
        case 051:
            SH(inst_word);
            break;
        case 053:
            SW(inst_word);
            break;
        case 062: /* LWC2 */
            MWC2_load(inst_word);
            break;
        case 072: /* SWC2 */
            MWC2_store(inst_word);
            break;
        default:
            res_S();
        }

#ifndef EMULATE_STATIC_PC
        if (stage == 2) { /* branch phase of scheduler */
            stage = 0*stage;
            PC = FIT_IMEM(temp_PC);
            GET_RCP_REG(SP_PC_REG) = temp_PC;
        } else {
            stage = 2*stage; /* next IW in branch delay slot? */
            PC = FIT_IMEM(PC + 0x004);
            GET_RCP_REG(SP_PC_REG) = 0x04001000 + PC;
        }
#else
        continue;
set_branch_delay:
        inst_word = *(pi32)(IMEM + FIT_IMEM(PC));
        PC = FIT_IMEM(temp_PC);
        goto EX;
#endif
    }
RSP_halted_CPU_exit_point:
#ifndef EMULATE_STATIC_PC
 /* UltraHLE: halted in a branch delay slot (BREAK there): the RSP resumes
  * at the branch target, not in the slot (see rsp_cxd4/UPSTREAM). */
    if (stage == 2) {
        PC = FIT_IMEM(temp_PC);
        stage = 0;
    }
#endif
    GET_RCP_REG(SP_PC_REG) = 0x04001000 | FIT_IMEM(PC);

    return;
}
