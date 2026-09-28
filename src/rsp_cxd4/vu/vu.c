/******************************************************************************\
* Project:  MSP Emulation Layer for Vector Unit Computational Operations       *
* Authors:  Iconoclast                                                         *
* Release:  2018.03.18                                                         *
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

#include "vu.h"

#include "multiply.h"
#include "add.h"
#include "select.h"
#include "logical.h"
#include "divide.h"
#if 0
#include "pack.h"
#endif

ALIGNED i16 VR[32][N << VR_STATIC_WRAPAROUND];
ALIGNED i16 VACC[3][N];
#ifndef ARCH_MIN_SSE2
ALIGNED i16 V_result[N];
#endif

/*
 * These normally should have type `int` because they are Boolean T/F arrays.
 * However, since SSE2 uses 128-bit XMM's, and Win32 `int` storage is 32-bit,
 * we have the problem of 32*8 > 128 bits, so we use `short` to reduce packs.
 */
ALIGNED i16 cf_ne[N]; /* $vco:  high "NOTEQUAL" */
ALIGNED i16 cf_co[N]; /* $vco:  low "carry/borrow in/out" */
ALIGNED i16 cf_clip[N]; /* $vcc:  high (clip tests:  VCL, VCH, VCR) */
ALIGNED i16 cf_comp[N]; /* $vcc:  low (VEQ, VNE, VLT, VGE, VCL, VCH, VCR) */
ALIGNED i16 cf_vce[N]; /* $vce:  vector compare extension register */

VECTOR_OPERATION res_V(v16 vs, v16 vt)
{
    vt = vs; /* unused */
    message("C2\nRESERVED"); /* uncertain how to handle reserved, untested */
#ifdef ARCH_MIN_SSE2
    vs = _mm_setzero_si128();
    return (vt = vs); /* -Wunused-but-set-parameter */
#else
    vector_wipe(V_result);
    if (vt == vs)
        return; /* -Wunused-but-set-parameter */
    return;
#endif
}
VECTOR_OPERATION res_M(v16 vs, v16 vt)
{ /* Ultra64 OS did have these, so one could implement this ext. */
    message("VMUL IQ");
#ifdef ARCH_MIN_SSE2
    vs = res_V(vs, vt);
    return (vs);
#else
    res_V(vs, vt);
    return;
#endif
}

/*
 * UltraHLE: ops transcribed from ares (ares/n64/rsp/interpreter-vpu.cpp,
 * commit 4cb8d92b, the SISD versions) for n64-systemtest: VABS, VCH, VCR
 * and VMRG replace cxd4's; VMULQ, VMACQ, VRNDP and VRNDN were reserved; the
 * undocumented ops (VADDB..VSUM, VEXTx, VINSx, V30...) are ares's VZERO;
 * VNULL is VNOP. Flags: VCOH cf_ne, VCOL cf_co, VCCH cf_clip, VCCL cf_comp.
 */
#ifdef ARCH_MIN_SSE2
#define UH_ARGS(vs, vt, S, T) \
    ALIGNED i16 S[N], T[N]; *(v16 *)S = vs; *(v16 *)T = vt;
#define UH_RETURN(VD)   return (*(v16 *)(VD))
#else
#define UH_ARGS(vs, vt, S, T) \
    i16 *S = vs, *T = vt;
#define UH_RETURN(VD)   { vector_copy(V_result, VD); return; }
#endif

static s64 uh_accget(int n)
{
    return (s64)((u64)(s64)VACC_H[n] << 32
        | (u64)(u16)VACC_M[n] << 16 | (u16)VACC_L[n]);
}
static void uh_accset(int n, s64 acc)
{
    VACC_H[n] = (i16)(acc >> 32);
    VACC_M[n] = (i16)(acc >> 16);
    VACC_L[n] = (i16)acc;
}
static i16 uh_clamp16(s64 x)
{
    return (i16)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
}

VECTOR_OPERATION uh_VABS(v16 vs, v16 vt)
{
    ALIGNED i16 VD[N];
    int n;
    UH_ARGS(vs, vt, S, T)

    for (n = 0; n < N; n++) {
        if (S[n] < 0) {
            VACC_L[n] = (i16)-T[n];                 /* -(-32768): -32768 */
            VD[n] = (T[n] == -32768) ? 32767 : (i16)-T[n];
        } else if (S[n] > 0) {
            VACC_L[n] = VD[n] = T[n];
        } else {
            VACC_L[n] = VD[n] = 0;
        }
    }
    UH_RETURN(VD);
}

VECTOR_OPERATION uh_VCH(v16 vs, v16 vt)
{
    int n;
    UH_ARGS(vs, vt, S, T)

    for (n = 0; n < N; n++) {
        const int ne = (u16)S[n] != (u16)(T[n] ^ 0xFFFF);

        if ((S[n] ^ T[n]) < 0) {
            const i16 result = (i16)(S[n] + T[n]);

            VACC_L[n] = (result <= 0) ? (i16)-T[n] : S[n];
            cf_comp[n] = result <= 0;
            cf_clip[n] = T[n] < 0;
            cf_co[n] = 1;
            cf_ne[n] = result != 0 && ne;
            cf_vce[n] = result == -1;
        } else {
            const i16 result = (i16)(S[n] - T[n]);

            VACC_L[n] = (result >= 0) ? T[n] : S[n];
            cf_comp[n] = T[n] < 0;
            cf_clip[n] = result >= 0;
            cf_co[n] = 0;
            cf_ne[n] = result != 0 && ne;
            cf_vce[n] = 0;
        }
    }
    UH_RETURN(VACC_L);
}

VECTOR_OPERATION uh_VCR(v16 vs, v16 vt)
{
    int n;
    UH_ARGS(vs, vt, S, T)

    for (n = 0; n < N; n++) {
        if ((S[n] ^ T[n]) < 0) {
            cf_clip[n] = T[n] < 0;
            cf_comp[n] = S[n] + T[n] + 1 <= 0;
            VACC_L[n] = cf_comp[n] ? (i16)~T[n] : S[n];
        } else {
            cf_comp[n] = T[n] < 0;
            cf_clip[n] = S[n] - T[n] >= 0;
            VACC_L[n] = cf_clip[n] ? T[n] : S[n];
        }
        cf_co[n] = cf_ne[n] = cf_vce[n] = 0;
    }
    UH_RETURN(VACC_L);
}

VECTOR_OPERATION uh_VMRG(v16 vs, v16 vt)
{
    int n;
    UH_ARGS(vs, vt, S, T)

    for (n = 0; n < N; n++) {
        VACC_L[n] = cf_comp[n] ? S[n] : T[n];
        cf_co[n] = cf_ne[n] = 0;
    }
    UH_RETURN(VACC_L);
}

VECTOR_OPERATION uh_VMULQ(v16 vs, v16 vt)
{
    ALIGNED i16 VD[N];
    int n;
    UH_ARGS(vs, vt, S, T)

    for (n = 0; n < N; n++) {
        s32 product = (s32)S[n] * T[n];

        if (product < 0)
            product += 31; /* round */
        VACC_H[n] = (i16)(product >> 16);
        VACC_M[n] = (i16)product;
        VACC_L[n] = 0;
        VD[n] = (i16)(uh_clamp16(product >> 1) & ~15);
    }
    UH_RETURN(VD);
}

VECTOR_OPERATION uh_VMACQ(v16 vs, v16 vt)
{
    ALIGNED i16 VD[N];
    int n;
    UH_ARGS(vs, vt, S, T)

    (void)S; (void)T;
    for (n = 0; n < N; n++) {
        s32 product = (s32)((u32)(u16)VACC_H[n] << 16 | (u16)VACC_M[n]);

        if (product < 0 && !(product & 1 << 5))
            product += 32;
        else if (product >= 32 && !(product & 1 << 5))
            product -= 32;
        VACC_H[n] = (i16)(product >> 16);
        VACC_M[n] = (i16)product;
        VD[n] = (i16)(uh_clamp16(product >> 1) & ~15);
    }
    UH_RETURN(VD);
}

/* VRNDP (up=1) adds vt to a non-negative accumulator, VRNDN to a negative
 * one; bit 0 of the vs field shifts vt up 16 bits */
static void uh_vrnd(i16 *VD, const i16 *T, int up)
{
    const int high = (inst_word >> 11) & 1;
    int n;

    for (n = 0; n < N; n++) {
        s64 product = T[n];
        s64 acc = uh_accget(n);

        if (high)
            product *= 65536;
        if ((up && acc >= 0) || (!up && acc < 0)) {
            acc += product;
            acc = (s64)((u64)acc << 16) >> 16; /* 48 bits, sign-extended */
        }
        uh_accset(n, acc);
        VD[n] = uh_clamp16(acc >> 16);
    }
}
VECTOR_OPERATION uh_VRNDP(v16 vs, v16 vt)
{
    ALIGNED i16 VD[N];
    UH_ARGS(vs, vt, S, T)

    (void)S;
    uh_vrnd(VD, T, 1);
    UH_RETURN(VD);
}
VECTOR_OPERATION uh_VRNDN(v16 vs, v16 vt)
{
    ALIGNED i16 VD[N];
    UH_ARGS(vs, vt, S, T)

    (void)S;
    uh_vrnd(VD, T, 0);
    UH_RETURN(VD);
}

VECTOR_OPERATION uh_VZERO(v16 vs, v16 vt)
{
    ALIGNED i16 VD[N];
    int n;
    UH_ARGS(vs, vt, S, T)

    for (n = 0; n < N; n++) {
        VACC_L[n] = (i16)(S[n] + T[n]);
        VD[n] = 0;
    }
    UH_RETURN(VD);
}

/*
 * Op-code-accurate matrix of all the known RSP vector operations.
 * To do:  Either remove VMACQ, or add VRNDP, VRNDN, and VMULQ.
 *
 * Note that these are not our literal function names, just macro names.
 */
VECTOR_OPERATION (*COP2_C2[8 * 8])(v16, v16) = {
    VMULF  ,VMULU  ,uh_VRNDP,uh_VMULQ,VMUDL ,VMUDM  ,VMUDN  ,VMUDH  , /* 000 */
    VMACF  ,VMACU  ,uh_VRNDN,uh_VMACQ,VMADL ,VMADM  ,VMADN  ,VMADH  , /* 001 */
    VADD   ,VSUB   ,uh_VZERO,uh_VABS,VADDC  ,VSUBC  ,uh_VZERO,uh_VZERO, /* 010 */
    uh_VZERO,uh_VZERO,uh_VZERO,uh_VZERO,uh_VZERO,VSAW ,uh_VZERO,uh_VZERO, /* 011 */
    VLT    ,VEQ    ,VNE    ,VGE    ,VCL    ,uh_VCH ,uh_VCR ,uh_VMRG, /* 100 */
    VAND   ,VNAND  ,VOR    ,VNOR   ,VXOR   ,VNXOR  ,uh_VZERO,uh_VZERO, /* 101 */
    VRCP   ,VRCPL  ,VRCPH  ,VMOV   ,VRSQ   ,VRSQL  ,VRSQH  ,VNOP   , /* 110 */
    uh_VZERO,uh_VZERO,uh_VZERO,uh_VZERO,uh_VZERO,uh_VZERO,uh_VZERO,VNOP, /* 111 */
}; /* 000     001     010     011     100     101     110     111 */

#ifndef ARCH_MIN_SSE2
u16 get_VCO(void)
{
    register u16 vco;

    vco = 0x0000
      | (cf_ne[0xF % 8] << 0xF)
      | (cf_ne[0xE % 8] << 0xE)
      | (cf_ne[0xD % 8] << 0xD)
      | (cf_ne[0xC % 8] << 0xC)
      | (cf_ne[0xB % 8] << 0xB)
      | (cf_ne[0xA % 8] << 0xA)
      | (cf_ne[0x9 % 8] << 0x9)
      | (cf_ne[0x8 % 8] << 0x8)
      | (cf_co[0x7 % 8] << 0x7)
      | (cf_co[0x6 % 8] << 0x6)
      | (cf_co[0x5 % 8] << 0x5)
      | (cf_co[0x4 % 8] << 0x4)
      | (cf_co[0x3 % 8] << 0x3)
      | (cf_co[0x2 % 8] << 0x2)
      | (cf_co[0x1 % 8] << 0x1)
      | (cf_co[0x0 % 8] << 0x0);
    return (vco); /* Big endian becomes little. */
}
u16 get_VCC(void)
{
    register u16 vcc;

    vcc = 0x0000
      | (cf_clip[0xF % 8] << 0xF)
      | (cf_clip[0xE % 8] << 0xE)
      | (cf_clip[0xD % 8] << 0xD)
      | (cf_clip[0xC % 8] << 0xC)
      | (cf_clip[0xB % 8] << 0xB)
      | (cf_clip[0xA % 8] << 0xA)
      | (cf_clip[0x9 % 8] << 0x9)
      | (cf_clip[0x8 % 8] << 0x8)
      | (cf_comp[0x7 % 8] << 0x7)
      | (cf_comp[0x6 % 8] << 0x6)
      | (cf_comp[0x5 % 8] << 0x5)
      | (cf_comp[0x4 % 8] << 0x4)
      | (cf_comp[0x3 % 8] << 0x3)
      | (cf_comp[0x2 % 8] << 0x2)
      | (cf_comp[0x1 % 8] << 0x1)
      | (cf_comp[0x0 % 8] << 0x0);
    return (vcc); /* Big endian becomes little. */
}
u8 get_VCE(void)
{
    unsigned int result;
    register u8 vce;

    result = 0x00
      | (cf_vce[0x7] << 0x7)
      | (cf_vce[0x6] << 0x6)
      | (cf_vce[0x5] << 0x5)
      | (cf_vce[0x4] << 0x4)
      | (cf_vce[0x3] << 0x3)
      | (cf_vce[0x2] << 0x2)
      | (cf_vce[0x1] << 0x1)
      | (cf_vce[0x0] << 0x0)
    ;
    vce = (u8)(result & 0xFF);
    return (vce); /* Big endian becomes little. */
}
#else
u16 get_VCO(void)
{
    v16 xmm, hi, lo;
    register u16 vco;

    hi = _mm_load_si128((v16 *)cf_ne);
    lo = _mm_load_si128((v16 *)cf_co);

/*
 * Rotate Boolean storage from LSB to MSB.
 */
    hi = _mm_slli_epi16(hi, 15);
    lo = _mm_slli_epi16(lo, 15);

    xmm = _mm_packs_epi16(lo, hi); /* Decompress INT16 Booleans to INT8 ones. */
    vco = _mm_movemask_epi8(xmm) & 0x0000FFFF; /* PMOVMSKB combines each MSB. */
    return (vco);
}
u16 get_VCC(void)
{
    v16 xmm, hi, lo;
    register u16 vcc;

    hi = _mm_load_si128((v16 *)cf_clip);
    lo = _mm_load_si128((v16 *)cf_comp);

/*
 * Rotate Boolean storage from LSB to MSB.
 */
    hi = _mm_slli_epi16(hi, 15);
    lo = _mm_slli_epi16(lo, 15);

    xmm = _mm_packs_epi16(lo, hi); /* Decompress INT16 Booleans to INT8 ones. */
    vcc = _mm_movemask_epi8(xmm) & 0x0000FFFF; /* PMOVMSKB combines each MSB. */
    return (vcc);
}
u8 get_VCE(void)
{
    v16 xmm, hi, lo;
    register u8 vce;

    hi = _mm_setzero_si128();
    lo = _mm_load_si128((v16 *)cf_vce);

    lo = _mm_slli_epi16(lo, 15); /* Rotate Boolean storage from LSB to MSB. */

    xmm = _mm_packs_epi16(lo, hi); /* Decompress INT16 Booleans to INT8 ones. */
    vce = _mm_movemask_epi8(xmm) & 0x000000FF; /* PMOVMSKB combines each MSB. */
    return (vce);
}
#endif

/*
 * CTC2 resources
 * not sure how to vectorize going the other direction into SSE2
 */
void set_VCO(u16 vco)
{
    register unsigned int i;

    for (i = 0; i < N; i++)
        cf_co[i] = (vco >> (i + 0x0)) & 1;
    for (i = 0; i < N; i++)
        cf_ne[i] = (vco >> (i + 0x8)) & 1;
    return; /* Little endian becomes big. */
}
void set_VCC(u16 vcc)
{
    register unsigned int i;

    for (i = 0; i < N; i++)
        cf_comp[i] = (vcc >> (i + 0x0)) & 1;
    for (i = 0; i < N; i++)
        cf_clip[i] = (vcc >> (i + 0x8)) & 1;
    return; /* Little endian becomes big. */
}
void set_VCE(u8 vce)
{
    register unsigned int i;

    for (i = 0; i < N; i++)
        cf_vce[i] = (vce >> i) & 1;
    return; /* Little endian becomes big. */
}
