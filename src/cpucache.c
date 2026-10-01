/****************************************************************************
** The VR4300's primary caches (LLE mode), transcribed from ares
** (ares/n64/cpu/dcache.cpp, cpu.hpp InstructionCache, interpreter-ipu.cpp
** CPU::CACHE, commit 4cb8d92b) for n64-systemtest's cache tests:
** - D-cache: 8 KB, 512 lines of 16 bytes, write-back, direct mapped by
**   virtual address bits 12..4, physical tags
** - I-cache: 16 KB, 512 lines of 32 bytes, bits 13..5
** A tag word is the physical page (bits 31..12) | valid (bit 0); a D-cache
** line keeps a mask of its dirty bytes. Line data are the big-endian words
** memory holds (mem_read32 values).
**
** Only cached CPU accesses to RDRAM go through the caches: KSEG0, TLB pages
** whose EntryLo C isn't 2, cached XKPHYS (op_checkaddr hands those in as
** KSEG0 addresses). Hardware registers and the rest stay uncached. DMA and
** UltraHLE's own accesses see RDRAM directly, as the RCP does on hardware.
*/

#include "ultra.h"

typedef struct
{
    dword tag;   // physical page | valid
    word  dirty; // dirty bytes
    dword w[4];
} DLine;

typedef struct
{
    dword tag;
    dword w[8];
} ILine;

static DLine dcache[512];
static ILine icache[512];

// memoized TLB page lookups (lle_tlbgen invalidates them): one for
// instruction fetches, and a table by page number for data. With a single
// one, a game running TLB-mapped (Factor 5's at 0x40000000) lost it at every
// data access and searched the TLB again for the next fetch.
typedef struct
{
    dword page,phys; // page 1: empty
    int   cached;
} Memo;

#define MEMO_DATA 64
static Memo  memo_fetch,memo_data[MEMO_DATA];
static dword memo_gen;
static int   memo_ready;

#define DATAMEMO(a) (&memo_data[((a)>>12)&(MEMO_DATA-1)])

static void memo_clear(void)
{
    int i;
    memo_fetch.page=1;
    for(i=0;i<MEMO_DATA;i++) memo_data[i].page=1;
    memo_gen=lle_tlbgen;
    memo_ready=1;
}

// a cached access to RDRAM? *pa its physical address
static int cache_classify(dword a,dword *pa,Memo *m)
{
    dword seg=a&0xE0000000;
    if(seg==0x80000000)
    {
        *pa=a&0x1fffffff;
        return(*pa<(dword)mem.ramsize);
    }
    if(seg==0xA0000000) return(0);
    if(memo_gen!=lle_tlbgen || !memo_ready) memo_clear();
    if((a&~0xfffu)!=m->page)
    {
        dword phys;
        int   cached;
        if(!lle_tlbphys(a,&phys,&cached)) return(0);
        m->page=a&~0xfffu;
        m->phys=phys&~0xfffu;
        m->cached=cached;
    }
    *pa=m->phys|(a&0xfff);
    return(m->cached && *pa<(dword)mem.ramsize);
}

static dword ram_read(dword pa)          { return(mem_read32(0xA0000000|pa)); }
static void  ram_write(dword pa,dword v) { mem_write32(0xA0000000|pa,v); }

/****************************************************************************
** D-cache
*/

static DLine *dline(dword vaddr)
{
    return(&dcache[(vaddr>>4)&0x1ff]);
}

// the physical address a line covers (tag page | index bits 11..4)
static dword dline_addr(DLine *l)
{
    return((l->tag&~0xfffu)|((dword)((l-dcache)<<4)&0xff0));
}

static int dline_hit(DLine *l,dword pa)
{
    return((l->tag&1) && (l->tag&~1u)==(pa&~0xfffu));
}

static void dline_writeback(DLine *l)
{
    dword base=dline_addr(l);
    int   i;
    for(i=0;i<4;i++) ram_write(base+i*4,l->w[i]);
}

static void dline_fill(DLine *l,dword pa)
{
    dword base;
    int   i;
    l->dirty=0;
    l->tag=(pa&~0xfffu)|1;
    base=dline_addr(l);
    for(i=0;i<4;i++) l->w[i]=ram_read(base+i*4);
}

// the line for an access, filled (after writing back a dirty victim)
static DLine *dline_get(dword vaddr,dword pa)
{
    DLine *l=dline(vaddr);
    if(!dline_hit(l,pa))
    {
        if((l->tag&1) && l->dirty) dline_writeback(l);
        dline_fill(l,pa);
    }
    return(l);
}

dword cache_read32(dword a)
{
    dword pa;
    if(!cache_classify(a,&pa,DATAMEMO(a))) return(mem_vread32(a));
    return(dline_get(a,pa)->w[(pa>>2)&3]);
}

dword cache_read16(dword a)
{
    dword pa;
    if(!cache_classify(a,&pa,DATAMEMO(a))) return(mem_vread16(a));
    return((dline_get(a,pa)->w[(pa>>2)&3]>>(16-8*(pa&2)))&0xffff);
}

dword cache_read8(dword a)
{
    dword pa;
    if(!cache_classify(a,&pa,DATAMEMO(a))) return(mem_vread8(a));
    return((dline_get(a,pa)->w[(pa>>2)&3]>>(24-8*(pa&3)))&0xff);
}

void cache_write32(dword a,dword v)
{
    dword  pa;
    DLine *l;
    if(!cache_classify(a,&pa,DATAMEMO(a))) { mem_vwrite32(a,v); return; }
    l=dline_get(a,pa);
    l->w[(pa>>2)&3]=v;
    l->dirty|=(word)(0xf<<(pa&0xc));
}

void cache_write16(dword a,dword v)
{
    dword  pa,sh,*w;
    DLine *l;
    if(!cache_classify(a,&pa,DATAMEMO(a))) { mem_vwrite16(a,v); return; }
    l=dline_get(a,pa);
    w=&l->w[(pa>>2)&3];
    sh=16-8*(pa&2);
    *w=(*w&~(0xffffu<<sh))|((v&0xffff)<<sh);
    l->dirty|=(word)(3<<(pa&0xe));
}

void cache_write8(dword a,dword v)
{
    dword  pa,sh,*w;
    DLine *l;
    if(!cache_classify(a,&pa,DATAMEMO(a))) { mem_vwrite8(a,v); return; }
    l=dline_get(a,pa);
    w=&l->w[(pa>>2)&3];
    sh=24-8*(pa&3);
    *w=(*w&~(0xffu<<sh))|((v&0xff)<<sh);
    l->dirty|=(word)(1<<(pa&0xf));
}

/****************************************************************************
** I-cache
*/

static ILine *iline(dword vaddr)
{
    return(&icache[(vaddr>>5)&0x1ff]);
}

static dword iline_addr(ILine *l)
{
    return((l->tag&~0xfffu)|((dword)((l-icache)<<5)&0xfe0));
}

static int iline_hit(ILine *l,dword pa)
{
    return((l->tag&1) && (l->tag&~1u)==(pa&~0xfffu));
}

static void iline_fill(ILine *l,dword pa)
{
    dword base;
    int   i;
    l->tag=(pa&~0xfffu)|1;
    base=iline_addr(l);
    for(i=0;i<8;i++) l->w[i]=ram_read(base+i*4);
}

// The line of the last fetch: fetches that follow in it skip the lookup. A
// line's words change only by a fill (a fetch at its index, which moves
// fetch_line), a CACHE instruction or a reset; its translation by a TLB
// write (lle_tlbgen). c_exec reads fetch_words itself while the PC stays in
// the line and Status is still fetch_status, the value it had when the
// line's fetch passed the mode, TLB and alignment checks (cpuc.c).
static ILine *fetch_line;
dword        *fetch_words;
dword         fetch_base=1,fetch_gen,fetch_status; // fetch_base: the line's virtual address; 1: none

// c_exec's fast path for a KSEG0 fetch in kernel mode (no TLB, no mode
// checks): the word when the I-cache line holds it, else 0 and the full path
int cache_fetchk0(dword a,dword *op)
{
    ILine *l=iline(a);
    dword  pa=a&0x1fffffff;
    if(l->tag!=((pa&~0xfffu)|1) || pa>=(dword)mem.ramsize) return(0);
    *op=l->w[(a>>2)&7];
    return(1);
}

dword cache_fetch(dword a)
{
    dword  pa;
    ILine *l;
    if((a&~31u)==fetch_base && fetch_gen==lle_tlbgen) return(fetch_line->w[(a>>2)&7]);
    if(!cache_classify(a,&pa,&memo_fetch)) return(mem_readop(a));
    l=iline(a);
    if(!iline_hit(l,pa)) iline_fill(l,pa);
    fetch_line=l;
    fetch_words=l->w;
    fetch_base=a&~31u;
    fetch_gen=lle_tlbgen;
    fetch_status=st.mmu[12].d;
    return(l->w[(pa>>2)&7]);
}

/****************************************************************************
** CACHE (ares CPU::CACHE); a is the checked, mapped address. TagLo is
** COP0 register 28: PTagLo (bits 27..8) = physical address 31..12,
** PState (bits 7..6).
*/

#define TAGLO st.mmu[28].d

static dword taglo_make(dword tag,dword state)
{
    return((state<<6)|(((tag&~0xfffu)>>12)<<8));
}

void cache_op(int op,dword a)
{
    dword pa;
    fetch_base=1;
    if(!cache_classify(a,&pa,DATAMEMO(a)))
    { // uncached addresses still select a line and give a tag
        if((a&0xE0000000)==0xA0000000 || (a&0xE0000000)==0x80000000) pa=a&0x1fffffff;
        else
        {
            int cached;
            if(!lle_tlbphys(a,&pa,&cached)) pa=a&0x1fffffff;
        }
    }
    switch(op)
    {
    case 0x00: // I: index invalidate
        {
            ILine *l=iline(a);
            l->tag=pa&~0xfffu;
        }
        break;
    case 0x04: // I: index load tag
        {
            ILine *l=iline(a);
            TAGLO=taglo_make(l->tag,(l->tag&1)?2:0);
        }
        break;
    case 0x08: // I: index store tag
        {
            ILine *l=iline(a);
            l->tag=(((TAGLO>>8)&0xfffff)<<12)|((TAGLO>>7)&1);
        }
        break;
    case 0x10: // I: hit invalidate
        {
            ILine *l=iline(a);
            if(iline_hit(l,pa)) l->tag&=~1u;
        }
        break;
    case 0x14: // I: fill
        iline_fill(iline(a),pa);
        break;
    case 0x18: // I: hit write back
        {
            ILine *l=iline(a);
            if(iline_hit(l,pa))
            {
                dword base=iline_addr(l);
                int   i;
                for(i=0;i<8;i++) ram_write(base+i*4,l->w[i]);
            }
        }
        break;
    case 0x01: // D: index write back invalidate
        {
            DLine *l=dline(a);
            int    valid=l->tag&1;
            if(valid && l->dirty) dline_writeback(l);
            if(valid) l->tag=pa&~0xfffu;
            l->tag&=~1u;
        }
        break;
    case 0x05: // D: index load tag
        {
            DLine *l=dline(a);
            TAGLO=taglo_make(l->tag,(l->tag&1)?3:0);
        }
        break;
    case 0x09: // D: index store tag
        {
            DLine *l=dline(a);
            l->tag=(((TAGLO>>8)&0xfffff)<<12)|((TAGLO>>7)&1);
        }
        break;
    case 0x0d: // D: create dirty exclusive
        {
            DLine *l=dline(a);
            if(!dline_hit(l,pa))
            {
                if((l->tag&1) && l->dirty) dline_writeback(l);
                l->dirty=0;
            }
            l->tag=(pa&~0xfffu)|1;
        }
        break;
    case 0x11: // D: hit invalidate
        {
            DLine *l=dline(a);
            if(dline_hit(l,pa)) l->tag&=~1u;
        }
        break;
    case 0x15: // D: hit write back invalidate
        {
            DLine *l=dline(a);
            if(dline_hit(l,pa))
            {
                if(l->dirty) dline_writeback(l);
                l->tag&=~1u;
            }
        }
        break;
    case 0x19: // D: hit write back
        {
            DLine *l=dline(a);
            if(dline_hit(l,pa) && l->dirty)
            {
                dline_writeback(l);
                l->dirty=0;
            }
        }
        break;
    }
}

/****************************************************************************
** save states and reset
*/

// before a state is saved: RAM gets the dirty lines (the state doesn't
// hold the caches)
void cache_flushall(void)
{
    int i;
    for(i=0;i<512;i++)
    {
        DLine *l=&dcache[i];
        if((l->tag&1) && l->dirty)
        {
            dline_writeback(l);
            l->dirty=0;
        }
    }
}

void cache_reset(void)
{
    memset(dcache,0,sizeof(dcache));
    memset(icache,0,sizeof(icache));
    memo_clear();
    fetch_base=1;
}
