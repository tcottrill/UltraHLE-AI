/****************************************************************************
** MusyX audio microcode (Factor 5), high level.
**
** Ported from mupen64plus-rsp-hle's musyx.c (Copyright (C) 2013 Bobby
** Smiles, GPL v2 or later) with the helpers it uses from audio.c and
** memory.c. v1: Rogue Squadron, Resident Evil 2, The World Is Not Enough,
** Gauntlet Legends, Rush 2049, Hydro Thunder, ... v2: Indiana Jones, Battle
** for Naboo.
**
** A task mixes up to 32 voices into 192-sample subframes and writes them to
** RDRAM, where the game queues them on the AI itself: the output is played
** from the AI buffers (slist_aiplay), not from here.
**
** RDRAM is held as native 32-bit words, so on this little endian host a
** byte is at address^3 and a halfword at address^2, as in Mupen64Plus.
*/

#include "ultra.h"
#include <stdint.h>
#include <string.h>

/* various constants */
enum { SUBFRAME_SIZE = 192 };
enum { MAX_VOICES = 32 };

enum { SAMPLE_BUFFER_SIZE = 0x200 };

enum {
    SFD_VOICE_COUNT     = 0x0,
    SFD_SFX_INDEX       = 0x2,
    SFD_VOICE_BITMASK   = 0x4,
    SFD_STATE_PTR       = 0x8,
    SFD_SFX_PTR         = 0xc,
    SFD_VOICES          = 0x10,

    /* v2 only */
    SFD2_10_PTR         = 0x10,
    SFD2_14_BITMASK     = 0x14,
    SFD2_15_BITMASK     = 0x15,
    SFD2_16_BITMASK     = 0x16,
    SFD2_18_PTR         = 0x18,
    SFD2_1C_PTR         = 0x1c,
    SFD2_20_PTR         = 0x20,
    SFD2_24_PTR         = 0x24,
    SFD2_VOICES         = 0x28
};

enum {
    VOICE_ENV_BEGIN         = 0x00,
    VOICE_ENV_STEP          = 0x10,
    VOICE_PITCH_Q16         = 0x20,
    VOICE_PITCH_SHIFT       = 0x22,
    VOICE_CATSRC_0          = 0x24,
    VOICE_CATSRC_1          = 0x30,
    VOICE_ADPCM_FRAMES      = 0x3c,
    VOICE_SKIP_SAMPLES      = 0x3e,

    /* for PCM16 */
    VOICE_U16_40            = 0x40,
    VOICE_U16_42            = 0x42,

    /* for ADPCM */
    VOICE_ADPCM_TABLE_PTR   = 0x40,

    VOICE_INTERLEAVED_PTR   = 0x44,
    VOICE_END_POINT         = 0x48,
    VOICE_RESTART_POINT     = 0x4a,
    VOICE_U16_4C            = 0x4c,
    VOICE_U16_4E            = 0x4e,

    VOICE_SIZE              = 0x50
};

enum {
    CATSRC_PTR1     = 0x00,
    CATSRC_PTR2     = 0x04,
    CATSRC_SIZE1    = 0x08,
    CATSRC_SIZE2    = 0x0a
};

enum {
    STATE_LAST_SAMPLE   = 0x0,
    STATE_BASE_VOL      = 0x100,
    STATE_CC0           = 0x110,
    STATE_740_LAST4_V1  = 0x290,

    STATE_740_LAST4_V2  = 0x110
};

enum {
    SFX_CBUFFER_PTR     = 0x00,
    SFX_CBUFFER_LENGTH  = 0x04,
    SFX_TAP_COUNT       = 0x08,
    SFX_FIR4_HGAIN      = 0x0a,
    SFX_TAP_DELAYS      = 0x0c,
    SFX_TAP_GAINS       = 0x2c,
    SFX_U16_3C          = 0x3c,
    SFX_U16_3E          = 0x3e,
    SFX_FIR4_HCOEFFS    = 0x40
};

typedef struct {
    /* internal subframes */
    int16_t left[SUBFRAME_SIZE];
    int16_t right[SUBFRAME_SIZE];
    int16_t cc0[SUBFRAME_SIZE];
    int16_t e50[SUBFRAME_SIZE];

    /* internal subframes base volumes */
    int32_t base_vol[4];

    int16_t subframe_740_last4[4];
} musyx_t;

typedef void (*mix_sfx_with_main_subframes_t)(musyx_t *musyx, const int16_t *subframe,
                                              const uint16_t* gains);

/****************************************************************************
** RDRAM access (Mupen64Plus memory.h), addresses masked to the RDRAM size
*/

static unsigned dram_mask(uint32_t address)
{
    return(address&(mem.ramsize-1));
}

static uint8_t *dram_u8(uint32_t address)
{
    return((uint8_t *)(mem.ram+(dram_mask(address)^3)));
}

static uint16_t *dram_u16(uint32_t address)
{
    return((uint16_t *)(mem.ram+(dram_mask(address)^2)));
}

static uint32_t *dram_u32(uint32_t address)
{
    return((uint32_t *)(mem.ram+(dram_mask(address)&~3u)));
}

static void dram_load_u8(uint8_t *dst,uint32_t address,size_t count)
{
    while(count--) *(dst++)=*dram_u8(address++);
}

static void dram_load_u16(uint16_t *dst,uint32_t address,size_t count)
{
    while(count--) { *(dst++)=*dram_u16(address); address+=2; }
}

static void dram_load_u32(uint32_t *dst,uint32_t address,size_t count)
{
    while(count--) { *(dst++)=*dram_u32(address); address+=4; }
}

static void dram_store_u16(const uint16_t *src,uint32_t address,size_t count)
{
    while(count--) { *dram_u16(address)=*(src++); address+=2; }
}

static unsigned int align(unsigned int x,unsigned amount)
{
    --amount;
    return((x+amount)&~amount);
}

/****************************************************************************
** Mupen64Plus arithmetics.h / audio.c
*/

static int16_t clamp_s16(int32_t x)
{
    x=(x<INT16_MIN)?INT16_MIN:x;
    x=(x>INT16_MAX)?INT16_MAX:x;
    return((int16_t)x);
}

static const int16_t RESAMPLE_LUT[64*4] = {
    (int16_t)0x0c39, (int16_t)0x66ad, (int16_t)0x0d46, (int16_t)0xffdf,
    (int16_t)0x0b39, (int16_t)0x6696, (int16_t)0x0e5f, (int16_t)0xffd8,
    (int16_t)0x0a44, (int16_t)0x6669, (int16_t)0x0f83, (int16_t)0xffd0,
    (int16_t)0x095a, (int16_t)0x6626, (int16_t)0x10b4, (int16_t)0xffc8,
    (int16_t)0x087d, (int16_t)0x65cd, (int16_t)0x11f0, (int16_t)0xffbf,
    (int16_t)0x07ab, (int16_t)0x655e, (int16_t)0x1338, (int16_t)0xffb6,
    (int16_t)0x06e4, (int16_t)0x64d9, (int16_t)0x148c, (int16_t)0xffac,
    (int16_t)0x0628, (int16_t)0x643f, (int16_t)0x15eb, (int16_t)0xffa1,
    (int16_t)0x0577, (int16_t)0x638f, (int16_t)0x1756, (int16_t)0xff96,
    (int16_t)0x04d1, (int16_t)0x62cb, (int16_t)0x18cb, (int16_t)0xff8a,
    (int16_t)0x0435, (int16_t)0x61f3, (int16_t)0x1a4c, (int16_t)0xff7e,
    (int16_t)0x03a4, (int16_t)0x6106, (int16_t)0x1bd7, (int16_t)0xff71,
    (int16_t)0x031c, (int16_t)0x6007, (int16_t)0x1d6c, (int16_t)0xff64,
    (int16_t)0x029f, (int16_t)0x5ef5, (int16_t)0x1f0b, (int16_t)0xff56,
    (int16_t)0x022a, (int16_t)0x5dd0, (int16_t)0x20b3, (int16_t)0xff48,
    (int16_t)0x01be, (int16_t)0x5c9a, (int16_t)0x2264, (int16_t)0xff3a,
    (int16_t)0x015b, (int16_t)0x5b53, (int16_t)0x241e, (int16_t)0xff2c,
    (int16_t)0x0101, (int16_t)0x59fc, (int16_t)0x25e0, (int16_t)0xff1e,
    (int16_t)0x00ae, (int16_t)0x5896, (int16_t)0x27a9, (int16_t)0xff10,
    (int16_t)0x0063, (int16_t)0x5720, (int16_t)0x297a, (int16_t)0xff02,
    (int16_t)0x001f, (int16_t)0x559d, (int16_t)0x2b50, (int16_t)0xfef4,
    (int16_t)0xffe2, (int16_t)0x540d, (int16_t)0x2d2c, (int16_t)0xfee8,
    (int16_t)0xffac, (int16_t)0x5270, (int16_t)0x2f0d, (int16_t)0xfedb,
    (int16_t)0xff7c, (int16_t)0x50c7, (int16_t)0x30f3, (int16_t)0xfed0,
    (int16_t)0xff53, (int16_t)0x4f14, (int16_t)0x32dc, (int16_t)0xfec6,
    (int16_t)0xff2e, (int16_t)0x4d57, (int16_t)0x34c8, (int16_t)0xfebd,
    (int16_t)0xff0f, (int16_t)0x4b91, (int16_t)0x36b6, (int16_t)0xfeb6,
    (int16_t)0xfef5, (int16_t)0x49c2, (int16_t)0x38a5, (int16_t)0xfeb0,
    (int16_t)0xfedf, (int16_t)0x47ed, (int16_t)0x3a95, (int16_t)0xfeac,
    (int16_t)0xfece, (int16_t)0x4611, (int16_t)0x3c85, (int16_t)0xfeab,
    (int16_t)0xfec0, (int16_t)0x4430, (int16_t)0x3e74, (int16_t)0xfeac,
    (int16_t)0xfeb6, (int16_t)0x424a, (int16_t)0x4060, (int16_t)0xfeaf,
    (int16_t)0xfeaf, (int16_t)0x4060, (int16_t)0x424a, (int16_t)0xfeb6,
    (int16_t)0xfeac, (int16_t)0x3e74, (int16_t)0x4430, (int16_t)0xfec0,
    (int16_t)0xfeab, (int16_t)0x3c85, (int16_t)0x4611, (int16_t)0xfece,
    (int16_t)0xfeac, (int16_t)0x3a95, (int16_t)0x47ed, (int16_t)0xfedf,
    (int16_t)0xfeb0, (int16_t)0x38a5, (int16_t)0x49c2, (int16_t)0xfef5,
    (int16_t)0xfeb6, (int16_t)0x36b6, (int16_t)0x4b91, (int16_t)0xff0f,
    (int16_t)0xfebd, (int16_t)0x34c8, (int16_t)0x4d57, (int16_t)0xff2e,
    (int16_t)0xfec6, (int16_t)0x32dc, (int16_t)0x4f14, (int16_t)0xff53,
    (int16_t)0xfed0, (int16_t)0x30f3, (int16_t)0x50c7, (int16_t)0xff7c,
    (int16_t)0xfedb, (int16_t)0x2f0d, (int16_t)0x5270, (int16_t)0xffac,
    (int16_t)0xfee8, (int16_t)0x2d2c, (int16_t)0x540d, (int16_t)0xffe2,
    (int16_t)0xfef4, (int16_t)0x2b50, (int16_t)0x559d, (int16_t)0x001f,
    (int16_t)0xff02, (int16_t)0x297a, (int16_t)0x5720, (int16_t)0x0063,
    (int16_t)0xff10, (int16_t)0x27a9, (int16_t)0x5896, (int16_t)0x00ae,
    (int16_t)0xff1e, (int16_t)0x25e0, (int16_t)0x59fc, (int16_t)0x0101,
    (int16_t)0xff2c, (int16_t)0x241e, (int16_t)0x5b53, (int16_t)0x015b,
    (int16_t)0xff3a, (int16_t)0x2264, (int16_t)0x5c9a, (int16_t)0x01be,
    (int16_t)0xff48, (int16_t)0x20b3, (int16_t)0x5dd0, (int16_t)0x022a,
    (int16_t)0xff56, (int16_t)0x1f0b, (int16_t)0x5ef5, (int16_t)0x029f,
    (int16_t)0xff64, (int16_t)0x1d6c, (int16_t)0x6007, (int16_t)0x031c,
    (int16_t)0xff71, (int16_t)0x1bd7, (int16_t)0x6106, (int16_t)0x03a4,
    (int16_t)0xff7e, (int16_t)0x1a4c, (int16_t)0x61f3, (int16_t)0x0435,
    (int16_t)0xff8a, (int16_t)0x18cb, (int16_t)0x62cb, (int16_t)0x04d1,
    (int16_t)0xff96, (int16_t)0x1756, (int16_t)0x638f, (int16_t)0x0577,
    (int16_t)0xffa1, (int16_t)0x15eb, (int16_t)0x643f, (int16_t)0x0628,
    (int16_t)0xffac, (int16_t)0x148c, (int16_t)0x64d9, (int16_t)0x06e4,
    (int16_t)0xffb6, (int16_t)0x1338, (int16_t)0x655e, (int16_t)0x07ab,
    (int16_t)0xffbf, (int16_t)0x11f0, (int16_t)0x65cd, (int16_t)0x087d,
    (int16_t)0xffc8, (int16_t)0x10b4, (int16_t)0x6626, (int16_t)0x095a,
    (int16_t)0xffd0, (int16_t)0x0f83, (int16_t)0x6669, (int16_t)0x0a44,
    (int16_t)0xffd8, (int16_t)0x0e5f, (int16_t)0x6696, (int16_t)0x0b39,
    (int16_t)0xffdf, (int16_t)0x0d46, (int16_t)0x66ad, (int16_t)0x0c39
};

static int32_t rdot(size_t n,const int16_t *x,const int16_t *y)
{
    int32_t accu=0;
    y+=n;
    while(n!=0)
    {
        accu+=*(x++)**(--y);
        --n;
    }
    return(accu);
}

static void adpcm_compute_residuals(int16_t *dst,const int16_t *src,
        const int16_t *cb_entry,const int16_t *last_samples,size_t count)
{
    const int16_t *const book1=cb_entry;
    const int16_t *const book2=cb_entry+8;
    const int16_t l1=last_samples[0];
    const int16_t l2=last_samples[1];
    size_t i;

    for(i=0;i<count;++i)
    {
        int32_t accu=(int32_t)src[i]<<11;
        accu+=book1[i]*l1+book2[i]*l2+rdot(i,book2,src);
        dst[i]=clamp_s16(accu>>11);
    }
}

static int16_t adpcm_predict_sample(uint8_t byte,uint8_t mask,
        unsigned lshift,unsigned rshift)
{
    int16_t sample=(int16_t)((uint16_t)(byte&mask)<<lshift);
    sample>>=rshift; /* signed */
    return(sample);
}

/****************************************************************************
** musyx.c
*/

static void load_base_vol(int32_t *base_vol, uint32_t address);
static void save_base_vol(const int32_t *base_vol, uint32_t address);
static void update_base_vol(int32_t *base_vol,
                            uint32_t voice_mask, uint32_t last_sample_ptr,
                            uint8_t mask_15, uint32_t ptr_24);

static void init_subframes_v1(musyx_t *musyx);
static void init_subframes_v2(musyx_t *musyx);

static uint32_t voice_stage(musyx_t *musyx,
                            uint32_t voice_ptr, uint32_t last_sample_ptr);

static void dma_cat8(uint8_t *dst, uint32_t catsrc_ptr);
static void dma_cat16(uint16_t *dst, uint32_t catsrc_ptr);

static void load_samples_PCM16(uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset);
static void load_samples_ADPCM(uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset);

static void adpcm_decode_frames(int16_t *dst, const uint8_t *src,
                                const int16_t *table, uint8_t count,
                                uint8_t skip_samples);

static void adpcm_predict_frame(int16_t *dst, const uint8_t *src,
                                const uint8_t *nibbles,
                                unsigned int rshift);

static void mix_voice_samples(musyx_t *musyx,
                              uint32_t voice_ptr, const int16_t *samples,
                              unsigned segbase, unsigned offset, uint32_t last_sample_ptr);

static void sfx_stage(mix_sfx_with_main_subframes_t mix_sfx_with_main_subframes,
                      musyx_t *musyx, uint32_t sfx_ptr, uint16_t idx);

static void mix_sfx_with_main_subframes_v1(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* gains);
static void mix_sfx_with_main_subframes_v2(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* gains);

static void mix_samples(int16_t *y, int16_t x, int16_t hgain);
static void mix_subframes(int16_t *y, const int16_t *x, int16_t hgain);
static void mix_fir4(int16_t *y, const int16_t *x, int16_t hgain, const int16_t *hcoeffs);

static void interleave_stage_v1(musyx_t *musyx, uint32_t output_ptr);

static void interleave_stage_v2(musyx_t *musyx,
                                uint16_t mask_16, uint32_t ptr_18,
                                uint32_t ptr_1c, uint32_t output_ptr);

static int32_t dot4(const int16_t *x, const int16_t *y)
{
    size_t i;
    int32_t accu = 0;

    for (i = 0; i < 4; ++i)
        accu = clamp_s16(accu + (((int32_t)x[i] * (int32_t)y[i]) >> 15));

    return accu;
}

/* MusyX v1: the task's data pointer and size (the SFD count) */
void musyx_v1_task(dword data_ptr,dword data_size)
{
    uint32_t sfd_ptr   = data_ptr;
    uint32_t sfd_count = data_size;
    uint32_t state_ptr;
    musyx_t musyx;

    if (sfd_count == 0)
        return;

    state_ptr = *dram_u32(sfd_ptr + SFD_STATE_PTR);

    /* load initial state */
    load_base_vol(musyx.base_vol, state_ptr + STATE_BASE_VOL);
    dram_load_u16((uint16_t *)musyx.cc0, state_ptr + STATE_CC0, SUBFRAME_SIZE);
    dram_load_u16((uint16_t *)musyx.subframe_740_last4, state_ptr + STATE_740_LAST4_V1, 4);

    for (;;) {
        /* parse SFD structure */
        uint16_t sfx_index   = *dram_u16(sfd_ptr + SFD_SFX_INDEX);
        uint32_t voice_mask  = *dram_u32(sfd_ptr + SFD_VOICE_BITMASK);
        uint32_t sfx_ptr     = *dram_u32(sfd_ptr + SFD_SFX_PTR);
        uint32_t voice_ptr       = sfd_ptr + SFD_VOICES;
        uint32_t last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;
        uint32_t output_ptr;

        /* initialize internal subframes using updated base volumes */
        update_base_vol(musyx.base_vol, voice_mask, last_sample_ptr, 0, 0);
        init_subframes_v1(&musyx);

        /* active voices get mixed into L,R,cc0,e50 subframes (optional) */
        output_ptr = voice_stage(&musyx, voice_ptr, last_sample_ptr);

        /* apply delay-based effects (optional) */
        sfx_stage(mix_sfx_with_main_subframes_v1, &musyx, sfx_ptr, sfx_index);

        /* emit interleaved L,R subframes */
        interleave_stage_v1(&musyx, output_ptr);

        --sfd_count;
        if (sfd_count == 0)
            break;

        sfd_ptr += SFD_VOICES + MAX_VOICES * VOICE_SIZE;
        state_ptr = *dram_u32(sfd_ptr + SFD_STATE_PTR);
    }

    /* writeback updated state */
    save_base_vol(musyx.base_vol, state_ptr + STATE_BASE_VOL);
    dram_store_u16((uint16_t *)musyx.cc0, state_ptr + STATE_CC0, SUBFRAME_SIZE);
    dram_store_u16((uint16_t *)musyx.subframe_740_last4, state_ptr + STATE_740_LAST4_V1, 4);
}

/* MusyX v2 */
void musyx_v2_task(dword data_ptr,dword data_size)
{
    uint32_t sfd_ptr   = data_ptr;
    uint32_t sfd_count = data_size;
    musyx_t musyx;

    if (sfd_count == 0)
        return;

    for (;;) {
        /* parse SFD structure */
        uint16_t sfx_index       = *dram_u16(sfd_ptr + SFD_SFX_INDEX);
        uint32_t voice_mask      = *dram_u32(sfd_ptr + SFD_VOICE_BITMASK);
        uint32_t state_ptr       = *dram_u32(sfd_ptr + SFD_STATE_PTR);
        uint32_t sfx_ptr         = *dram_u32(sfd_ptr + SFD_SFX_PTR);
        uint32_t voice_ptr       = sfd_ptr + SFD2_VOICES;

        uint8_t  mask_15         = *dram_u8 (sfd_ptr + SFD2_15_BITMASK);
        uint16_t mask_16         = *dram_u16(sfd_ptr + SFD2_16_BITMASK);
        uint32_t ptr_18          = *dram_u32(sfd_ptr + SFD2_18_PTR);
        uint32_t ptr_1c          = *dram_u32(sfd_ptr + SFD2_1C_PTR);
        uint32_t ptr_20          = *dram_u32(sfd_ptr + SFD2_20_PTR);
        uint32_t ptr_24          = *dram_u32(sfd_ptr + SFD2_24_PTR);

        uint32_t last_sample_ptr = state_ptr + STATE_LAST_SAMPLE;
        uint32_t output_ptr;

        /* load state */
        load_base_vol(musyx.base_vol, state_ptr + STATE_BASE_VOL);
        dram_load_u16((uint16_t *)musyx.subframe_740_last4,
                state_ptr + STATE_740_LAST4_V2, 4);

        /* initialize internal subframes using updated base volumes */
        update_base_vol(musyx.base_vol, voice_mask, last_sample_ptr, mask_15, ptr_24);
        init_subframes_v2(&musyx);

        /* ptr_10 / mask_14: not handled by Mupen64Plus either */

        /* active voices get mixed into L,R,cc0,e50 subframes (optional) */
        output_ptr = voice_stage(&musyx, voice_ptr, last_sample_ptr);

        /* apply delay-based effects (optional) */
        sfx_stage(mix_sfx_with_main_subframes_v2, &musyx, sfx_ptr, sfx_index);

        dram_store_u16((uint16_t*)musyx.left,  output_ptr                  , SUBFRAME_SIZE);
        dram_store_u16((uint16_t*)musyx.right, output_ptr + 2*SUBFRAME_SIZE, SUBFRAME_SIZE);
        dram_store_u16((uint16_t*)musyx.cc0,   output_ptr + 4*SUBFRAME_SIZE, SUBFRAME_SIZE);

        /* store state */
        save_base_vol(musyx.base_vol, state_ptr + STATE_BASE_VOL);
        dram_store_u16((uint16_t*)musyx.subframe_740_last4,
                state_ptr + STATE_740_LAST4_V2, 4);

        if (mask_16)
            interleave_stage_v2(&musyx, mask_16, ptr_18, ptr_1c, ptr_20);

        --sfd_count;
        if (sfd_count == 0)
            break;

        sfd_ptr += SFD2_VOICES + MAX_VOICES * VOICE_SIZE;
    }
}

static void load_base_vol(int32_t *base_vol, uint32_t address)
{
    base_vol[0] = ((uint32_t)(*dram_u16(address))     << 16) | (*dram_u16(address +  8));
    base_vol[1] = ((uint32_t)(*dram_u16(address + 2)) << 16) | (*dram_u16(address + 10));
    base_vol[2] = ((uint32_t)(*dram_u16(address + 4)) << 16) | (*dram_u16(address + 12));
    base_vol[3] = ((uint32_t)(*dram_u16(address + 6)) << 16) | (*dram_u16(address + 14));
}

static void save_base_vol(const int32_t *base_vol, uint32_t address)
{
    unsigned k;

    for (k = 0; k < 4; ++k) {
        *dram_u16(address) = (uint16_t)(base_vol[k] >> 16);
        address += 2;
    }

    for (k = 0; k < 4; ++k) {
        *dram_u16(address) = (uint16_t)(base_vol[k]);
        address += 2;
    }
}

static void update_base_vol(int32_t *base_vol,
                            uint32_t voice_mask, uint32_t last_sample_ptr,
                            uint8_t mask_15, uint32_t ptr_24)
{
    unsigned i, k;
    uint32_t mask;

    /* optim: skip voices contributions entirely if voice_mask is empty */
    if (voice_mask != 0) {
        for (i = 0, mask = 1; i < MAX_VOICES;
             ++i, mask <<= 1, last_sample_ptr += 8) {
            if ((voice_mask & mask) == 0)
                continue;

            for (k = 0; k < 4; ++k)
                base_vol[k] += (int16_t)*dram_u16(last_sample_ptr + k * 2);
        }
    }

    /* optim: skip contributions entirely if mask_15 is empty */
    if (mask_15 != 0) {
        for(i = 0, mask = 1; i < 4;
                ++i, mask <<= 1, ptr_24 += 8) {
            if ((mask_15 & mask) == 0)
                continue;

            for(k = 0; k < 4; ++k)
                base_vol[k] += (int16_t)*dram_u16(ptr_24 + k * 2);
        }
    }

    /* apply 3% decay */
    for (k = 0; k < 4; ++k)
        base_vol[k] = (base_vol[k] * 0x0000f850) >> 16;
}

static void init_subframes_v1(musyx_t *musyx)
{
    unsigned i;

    int16_t base_cc0 = clamp_s16(musyx->base_vol[2]);
    int16_t base_e50 = clamp_s16(musyx->base_vol[3]);

    int16_t *left  = musyx->left;
    int16_t *right = musyx->right;
    int16_t *cc0   = musyx->cc0;
    int16_t *e50   = musyx->e50;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        *(e50++)    = base_e50;
        *(left++)   = clamp_s16(*cc0 + base_cc0);
        *(right++)  = clamp_s16(-*cc0 - base_cc0);
        *(cc0++)    = 0;
    }
}

static void init_subframes_v2(musyx_t *musyx)
{
    unsigned i,k;
    int16_t values[4];
    int16_t* subframes[4];

    for(k = 0; k < 4; ++k)
        values[k] = clamp_s16(musyx->base_vol[k]);

    subframes[0] = musyx->left;
    subframes[1] = musyx->right;
    subframes[2] = musyx->cc0;
    subframes[3] = musyx->e50;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {

        for(k = 0; k < 4; ++k)
            *(subframes[k]++) = values[k];
    }
}

/* Process voices, and returns interleaved subframe destination address */
static uint32_t voice_stage(musyx_t *musyx,
                            uint32_t voice_ptr, uint32_t last_sample_ptr)
{
    uint32_t output_ptr;
    int i = 0;

    /* voice stage can be skipped if first voice has no samples */
    if (*dram_u16(voice_ptr + VOICE_CATSRC_0 + CATSRC_SIZE1) == 0) {
        output_ptr = *dram_u32(voice_ptr + VOICE_INTERLEAVED_PTR);
    } else {
        /* otherwise process voices until a non null output_ptr is encountered
           (no more than MAX_VOICES: a runaway list in bad RAM won't hang) */
        for (;;) {
            /* load voice samples (PCM16 or APDCM) */
            int16_t samples[SAMPLE_BUFFER_SIZE];
            unsigned segbase;
            unsigned offset;

            if (*dram_u8(voice_ptr + VOICE_ADPCM_FRAMES) == 0)
                load_samples_PCM16(voice_ptr, samples, &segbase, &offset);
            else
                load_samples_ADPCM(voice_ptr, samples, &segbase, &offset);

            /* mix them with each internal subframes */
            mix_voice_samples(musyx, voice_ptr, samples, segbase, offset,
                              last_sample_ptr + i * 8);

            /* check break condition */
            output_ptr = *dram_u32(voice_ptr + VOICE_INTERLEAVED_PTR);
            if (output_ptr != 0 || i >= MAX_VOICES - 1)
                break;

            /* next voice */
            ++i;
            voice_ptr += VOICE_SIZE;
        }
    }

    return output_ptr;
}

static void dma_cat8(uint8_t *dst, uint32_t catsrc_ptr)
{
    uint32_t ptr1  = *dram_u32(catsrc_ptr + CATSRC_PTR1);
    uint32_t ptr2  = *dram_u32(catsrc_ptr + CATSRC_PTR2);
    uint16_t size1 = *dram_u16(catsrc_ptr + CATSRC_SIZE1);
    uint16_t size2 = *dram_u16(catsrc_ptr + CATSRC_SIZE2);

    size_t count1 = size1;
    size_t count2 = size2;

    /* the caller's buffer holds 0x140 bytes */
    if (count1 > 0x140) count1 = 0x140;
    if (count1 + count2 > 0x140) count2 = 0x140 - count1;

    dram_load_u8(dst, ptr1, count1);

    if (size2 == 0)
        return;

    dram_load_u8(dst + count1, ptr2, count2);
}

static void dma_cat16(uint16_t *dst, uint32_t catsrc_ptr)
{
    uint32_t ptr1  = *dram_u32(catsrc_ptr + CATSRC_PTR1);
    uint32_t ptr2  = *dram_u32(catsrc_ptr + CATSRC_PTR2);
    uint16_t size1 = *dram_u16(catsrc_ptr + CATSRC_SIZE1);
    uint16_t size2 = *dram_u16(catsrc_ptr + CATSRC_SIZE2);

    size_t count1 = size1 >> 1;
    size_t count2 = size2 >> 1;

    dram_load_u16(dst, ptr1, count1);

    if (size2 == 0)
        return;

    dram_load_u16(dst + count1, ptr2, count2);
}

static void load_samples_PCM16(uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset)
{
    uint8_t  u8_3e  = *dram_u8(voice_ptr + VOICE_SKIP_SAMPLES);
    uint16_t u16_40 = *dram_u16(voice_ptr + VOICE_U16_40);
    uint16_t u16_42 = *dram_u16(voice_ptr + VOICE_U16_42);

    unsigned count = align(u16_40 + u8_3e, 4);

    if (count > SAMPLE_BUFFER_SIZE) count = SAMPLE_BUFFER_SIZE;

    *segbase = SAMPLE_BUFFER_SIZE - count;
    *offset  = u8_3e;

    dma_cat16((uint16_t *)samples + *segbase, voice_ptr + VOICE_CATSRC_0);

    if (u16_42 != 0)
        dma_cat16((uint16_t *)samples, voice_ptr + VOICE_CATSRC_1);
}

static void load_samples_ADPCM(uint32_t voice_ptr, int16_t *samples,
                               unsigned *segbase, unsigned *offset)
{
    /* decompressed samples cannot exceed 0x400 bytes;
     * ADPCM has a compression ratio of 5/16 */
    uint8_t buffer[SAMPLE_BUFFER_SIZE * 2 * 5 / 16];
    int16_t adpcm_table[128];

    uint8_t u8_3c = *dram_u8(voice_ptr + VOICE_ADPCM_FRAMES    );
    uint8_t u8_3d = *dram_u8(voice_ptr + VOICE_ADPCM_FRAMES + 1);
    uint8_t u8_3e = *dram_u8(voice_ptr + VOICE_SKIP_SAMPLES    );
    uint8_t u8_3f = *dram_u8(voice_ptr + VOICE_SKIP_SAMPLES + 1);
    uint32_t adpcm_table_ptr = *dram_u32(voice_ptr + VOICE_ADPCM_TABLE_PTR);
    unsigned count;

    /* 16 frames of 32 samples fill the buffer */
    if (u8_3c > 16) u8_3c = 16;
    if (u8_3d > 16 - u8_3c) u8_3d = 16 - u8_3c;

    dram_load_u16((uint16_t *)adpcm_table, adpcm_table_ptr, 128);

    count = u8_3c << 5;

    *segbase = SAMPLE_BUFFER_SIZE - count;
    *offset  = u8_3e & 0x1f;

    dma_cat8(buffer, voice_ptr + VOICE_CATSRC_0);
    adpcm_decode_frames(samples + *segbase, buffer, adpcm_table, u8_3c, u8_3e);

    if (u8_3d != 0) {
        dma_cat8(buffer, voice_ptr + VOICE_CATSRC_1);
        adpcm_decode_frames(samples, buffer, adpcm_table, u8_3d, u8_3f);
    }
}

static void adpcm_decode_frames(int16_t *dst, const uint8_t *src,
                                const int16_t *table, uint8_t count,
                                uint8_t skip_samples)
{
    int16_t frame[32];
    const uint8_t *nibbles = src + 8;
    unsigned i;
    int jump_gap = 0;

    if (skip_samples >= 32) {
        jump_gap = 1;
        nibbles += 16;
        src += 4;
    }

    for (i = 0; i < count; ++i) {
        uint8_t c2 = nibbles[0];

        const int16_t *book = (c2 & 0xf0) + table;
        unsigned int rshift = (c2 & 0x0f);

        adpcm_predict_frame(frame, src, nibbles, rshift);

        memcpy(dst, frame, 2 * sizeof(frame[0]));
        adpcm_compute_residuals(dst +  2, frame +  2, book, dst     , 6);
        adpcm_compute_residuals(dst +  8, frame +  8, book, dst +  6, 8);
        adpcm_compute_residuals(dst + 16, frame + 16, book, dst + 14, 8);
        adpcm_compute_residuals(dst + 24, frame + 24, book, dst + 22, 8);

        if (jump_gap) {
            nibbles += 8;
            src += 32;
        }

        jump_gap = !jump_gap;
        nibbles += 16;
        src += 4;
        dst += 32;
    }
}

static void adpcm_predict_frame(int16_t *dst, const uint8_t *src,
                                const uint8_t *nibbles,
                                unsigned int rshift)
{
    unsigned int i;

    *(dst++) = (int16_t)((src[0] << 8) | src[1]);
    *(dst++) = (int16_t)((src[2] << 8) | src[3]);

    for (i = 1; i < 16; ++i) {
        uint8_t byte = nibbles[i];

        *(dst++) = adpcm_predict_sample(byte, 0xf0,  8, rshift);
        *(dst++) = adpcm_predict_sample(byte, 0x0f, 12, rshift);
    }
}

static void mix_voice_samples(musyx_t *musyx,
                              uint32_t voice_ptr, const int16_t *samples,
                              unsigned segbase, unsigned offset, uint32_t last_sample_ptr)
{
    int i, k;

    /* parse VOICE structure */
    const uint16_t pitch_q16   = *dram_u16(voice_ptr + VOICE_PITCH_Q16);
    const uint16_t pitch_shift = *dram_u16(voice_ptr + VOICE_PITCH_SHIFT); /* Q4.12 */

    const uint16_t end_point     = *dram_u16(voice_ptr + VOICE_END_POINT);
    const uint16_t restart_point = *dram_u16(voice_ptr + VOICE_RESTART_POINT);

    const uint16_t u16_4e = *dram_u16(voice_ptr + VOICE_U16_4E);

    /* init values and pointers */
    const int16_t       *sample         = samples + segbase + offset + u16_4e;
    const int16_t *const sample_end     = samples + segbase + end_point;
    const int16_t *const sample_restart = samples + (restart_point & 0x7fff) +
                                          (((restart_point & 0x8000) != 0) ? 0x000 : segbase);

    uint32_t pitch_accu = pitch_q16;
    uint32_t pitch_step = pitch_shift << 4;

    int32_t  v4_env[4];
    int32_t  v4_env_step[4];
    int16_t *v4_dst[4];
    int16_t  v4[4];

    dram_load_u32((uint32_t *)v4_env,      voice_ptr + VOICE_ENV_BEGIN, 4);
    dram_load_u32((uint32_t *)v4_env_step, voice_ptr + VOICE_ENV_STEP,  4);

    v4_dst[0] = musyx->left;
    v4_dst[1] = musyx->right;
    v4_dst[2] = musyx->cc0;
    v4_dst[3] = musyx->e50;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        /* update sample and lut pointers and then pitch_accu */
        const int16_t *lut = (RESAMPLE_LUT + ((pitch_accu & 0xfc00) >> 8));
        int dist;
        int16_t v;

        sample += (pitch_accu >> 16);
        pitch_accu &= 0xffff;
        pitch_accu += pitch_step;

        /* handle end/restart points */
        dist = (int)(sample - sample_end);
        if (dist >= 0)
            sample = sample_restart + dist;

        /* a bad restart point or pitch leaves the buffer: silence, as
           reading past it would crash (the RSP reads DMEM, which wraps) */
        if (sample < samples || sample + 4 > samples + SAMPLE_BUFFER_SIZE)
            v = 0;
        else
            /* apply resample filter */
            v = clamp_s16(dot4(sample, lut));

        for (k = 0; k < 4; ++k) {
            /* envmix */
            int32_t accu = (v * (v4_env[k] >> 16)) >> 15;
            v4[k] = clamp_s16(accu);
            *(v4_dst[k]) = clamp_s16(accu + *(v4_dst[k]));

            /* update envelopes and dst pointers */
            ++(v4_dst[k]);
            v4_env[k] += v4_env_step[k];
        }
    }

    /* save last resampled sample */
    dram_store_u16((uint16_t *)v4, last_sample_ptr, 4);
}

static void sfx_stage(mix_sfx_with_main_subframes_t mix_sfx_with_main_subframes,
                      musyx_t *musyx, uint32_t sfx_ptr, uint16_t idx)
{
    unsigned int i;

    int16_t buffer[SUBFRAME_SIZE + 4];
    int16_t *subframe = buffer + 4;

    uint32_t tap_delays[8];
    int16_t tap_gains[8];
    int16_t fir4_hcoeffs[4];

    int16_t delayed[SUBFRAME_SIZE];
    int dpos, dlength;

    const uint32_t pos = idx * SUBFRAME_SIZE;

    uint32_t cbuffer_ptr;
    uint32_t cbuffer_length;
    uint16_t tap_count;
    int16_t fir4_hgain;
    uint16_t sfx_gains[2];

    if (sfx_ptr == 0)
        return;

    /* load sfx  parameters */
    cbuffer_ptr    = *dram_u32(sfx_ptr + SFX_CBUFFER_PTR);
    cbuffer_length = *dram_u32(sfx_ptr + SFX_CBUFFER_LENGTH);

    tap_count      = *dram_u16(sfx_ptr + SFX_TAP_COUNT);
    if (tap_count > 8) tap_count = 8;

    dram_load_u32(tap_delays, sfx_ptr + SFX_TAP_DELAYS, 8);
    dram_load_u16((uint16_t *)tap_gains,  sfx_ptr + SFX_TAP_GAINS,  8);

    fir4_hgain     = *dram_u16(sfx_ptr + SFX_FIR4_HGAIN);
    dram_load_u16((uint16_t *)fir4_hcoeffs, sfx_ptr + SFX_FIR4_HCOEFFS, 4);

    sfx_gains[0]   = *dram_u16(sfx_ptr + SFX_U16_3C);
    sfx_gains[1]   = *dram_u16(sfx_ptr + SFX_U16_3E);

    /* mix up to 8 delayed subframes */
    memset(subframe, 0, SUBFRAME_SIZE * sizeof(subframe[0]));
    for (i = 0; i < tap_count; ++i) {

        dpos = pos - tap_delays[i];
        if (dpos <= 0)
            dpos += cbuffer_length;
        dlength = SUBFRAME_SIZE;

        if ((uint32_t)(dpos + SUBFRAME_SIZE) > cbuffer_length) {
            dlength = cbuffer_length - dpos;
            if (dlength < 0) dlength = 0;
            if (dlength > SUBFRAME_SIZE) dlength = SUBFRAME_SIZE;
            dram_load_u16((uint16_t *)delayed + dlength, cbuffer_ptr, SUBFRAME_SIZE - dlength);
        }

        dram_load_u16((uint16_t *)delayed, cbuffer_ptr + dpos * 2, dlength);

        mix_subframes(subframe, delayed, tap_gains[i]);
    }

    /* add resulting subframe to main subframes */
    mix_sfx_with_main_subframes(musyx, subframe, sfx_gains);

    /* apply FIR4 filter and writeback filtered result */
    memcpy(buffer, musyx->subframe_740_last4, 4 * sizeof(int16_t));
    memcpy(musyx->subframe_740_last4, subframe + SUBFRAME_SIZE - 4, 4 * sizeof(int16_t));
    mix_fir4(musyx->e50, buffer + 1, fir4_hgain, fir4_hcoeffs);
    dram_store_u16((uint16_t *)musyx->e50, cbuffer_ptr + pos * 2, SUBFRAME_SIZE);
}

static void mix_sfx_with_main_subframes_v1(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* gains)
{
    unsigned i;

    (void)gains;
    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        int16_t v = subframe[i];
        musyx->left[i]  = clamp_s16(musyx->left[i]  + v);
        musyx->right[i] = clamp_s16(musyx->right[i] + v);
    }
}

static void mix_sfx_with_main_subframes_v2(musyx_t *musyx, const int16_t *subframe,
                                           const uint16_t* gains)
{
    unsigned i;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        int16_t v = subframe[i];
        int16_t v1 = (int16_t)((int32_t)(v * gains[0]) >> 16);
        int16_t v2 = (int16_t)((int32_t)(v * gains[1]) >> 16);

        musyx->left[i]  = clamp_s16(musyx->left[i]  + v1);
        musyx->right[i] = clamp_s16(musyx->right[i] + v1);
        musyx->cc0[i]   = clamp_s16(musyx->cc0[i]   + v2);
    }
}

static void mix_samples(int16_t *y, int16_t x, int16_t hgain)
{
    *y = clamp_s16(*y + ((x * hgain + 0x4000) >> 15));
}

static void mix_subframes(int16_t *y, const int16_t *x, int16_t hgain)
{
    unsigned int i;

    for (i = 0; i < SUBFRAME_SIZE; ++i)
        mix_samples(&y[i], x[i], hgain);
}

static void mix_fir4(int16_t *y, const int16_t *x, int16_t hgain, const int16_t *hcoeffs)
{
    unsigned int i;
    int32_t h[4];

    h[0] = (hgain * hcoeffs[0]) >> 15;
    h[1] = (hgain * hcoeffs[1]) >> 15;
    h[2] = (hgain * hcoeffs[2]) >> 15;
    h[3] = (hgain * hcoeffs[3]) >> 15;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        int32_t v = (h[0] * x[i] + h[1] * x[i + 1] + h[2] * x[i + 2] + h[3] * x[i + 3]) >> 15;
        y[i] = clamp_s16(y[i] + v);
    }
}

static void interleave_stage_v1(musyx_t *musyx, uint32_t output_ptr)
{
    size_t i;

    int16_t base_left;
    int16_t base_right;

    int16_t *left;
    int16_t *right;

    base_left  = clamp_s16(musyx->base_vol[0]);
    base_right = clamp_s16(musyx->base_vol[1]);

    left  = musyx->left;
    right = musyx->right;

    for (i = 0; i < SUBFRAME_SIZE; ++i) {
        uint16_t l = clamp_s16(*(left++)  + base_left);
        uint16_t r = clamp_s16(*(right++) + base_right);

        *dram_u32(output_ptr + i * 4) = ((uint32_t)l << 16) | r;
    }
}

static void interleave_stage_v2(musyx_t *musyx,
                                uint16_t mask_16, uint32_t ptr_18,
                                uint32_t ptr_1c, uint32_t output_ptr)
{
    unsigned i, k;
    int16_t subframe[SUBFRAME_SIZE];
    uint16_t mask;

    /* compute L_total, R_total and update subframe @ptr_1c */
    memset(subframe, 0, SUBFRAME_SIZE*sizeof(subframe[0]));

    for(i = 0; i < SUBFRAME_SIZE; ++i) {
        int16_t v = *dram_u16(ptr_1c + i*2);
        musyx->left[i] = v;
        musyx->right[i] = clamp_s16(-v);
    }

    for (k = 0, mask = 1; k < 8; ++k, mask <<= 1, ptr_18 += 8) {
        int16_t hgain;
        uint32_t address;

        if ((mask_16 & mask) == 0)
            continue;

        address = *dram_u32(ptr_18);
        hgain   = *dram_u16(ptr_18 + 4);

        for(i = 0; i < SUBFRAME_SIZE; ++i, address += 2) {
            mix_samples(&musyx->left[i],  *dram_u16(address), hgain);
            mix_samples(&musyx->right[i], *dram_u16(address + 2*SUBFRAME_SIZE), hgain);
            mix_samples(&subframe[i],     *dram_u16(address + 4*SUBFRAME_SIZE), hgain);
        }
    }

    /* interleave L_total and R_total */
    for(i = 0; i < SUBFRAME_SIZE; ++i) {
        uint16_t l = musyx->left[i];
        uint16_t r = musyx->right[i];
        *dram_u32(output_ptr + i * 4) = ((uint32_t)l << 16) | r;
    }

    /* writeback subframe @ptr_1c */
    dram_store_u16((uint16_t*)subframe, ptr_1c, SUBFRAME_SIZE);
}
