// Software RDP: raw RDP command lists drawn into the RDRAM color image, for
// LLE games whose own RSP microcode feeds the RDP (libdragon's rdpq). The VI
// then shows that framebuffer (rdp_showvi). Implemented: fill and texture
// rectangles, triangles with shade, texture and z, the depth image (compare,
// update, primitive depth), TMEM loads (tile, block, TLUT), RGBA/CI/IA/I
// textures, point or 3-point filtered sampling, fill, copy, 1- and 2-cycle
// modes with the color combiner (key and K4/K5 inputs), blender and alpha
// compare. Not done: coverage (every pixel is fully covered), LOD/mipmaps,
// chroma keying and YUV conversion. Command formats and pipeline follow
// angrylion's RDP (n64video.c) and GLideN64 (RDP.cpp).

#include "ultra.h"
#include "rdp_soft.h"

typedef struct { int r,g,b,a; } Color;

typedef struct
{
    int fmt,size,line,tmem,pal;
    int ct,mt,maskt,shiftt,cs,ms,masks,shifts;
    int sl,tl,sh,th; // 10.2
} Tile;

typedef struct
{
    int suba_rgb,subb_rgb,mul_rgb,add_rgb;
    int suba_a,subb_a,mul_a,add_a;
} Combine;

static struct
{
    int     active;
    dword   ciaddr; int cifmt,cisize,ciwidth;
    dword   tiaddr; int tifmt,tisize,tiwidth;
    Tile    tile[8];
    dword   omhi,omlo;
    Combine cc[2];
    dword   fill;
    Color   fog,blend,prim,env;
    int     primlodfrac;
    int     sxh,syh,sxl,syl; // scissor, 10.2
    byte    tmem[4096];      // N64 byte order
    dword   zaddr;           // SET_Z_IMAGE: 16-bit depth image, color width
    int     primz;           // SET_PRIM_DEPTH z, 18-bit like interpolated z
    int     k[6];            // SET_CONVERT K0..K5 (K4/K5 reach the combiner)
    int     keyc[3],keys[3]; // SET_KEY_R/GB: key center and scale, r g b
} rs;

// other modes
#define CYCLETYPE   ((rs.omhi>>20)&3)
#define PERSP       ((rs.omhi>>19)&1)
#define SAMPLE_TYPE ((rs.omhi>>13)&1) // 1 = filtered (3-point), 0 = point
#define EN_TLUT     ((rs.omhi>>15)&1)
#define TLUT_TYPE   ((rs.omhi>>14)&1)
#define FORCE_BLEND ((rs.omlo>>14)&1)
#define ZMODE       ((rs.omlo>>10)&3) // 3 = decal
#define IMAGE_READ  ((rs.omlo>>6)&1)
#define Z_UPDATE    ((rs.omlo>>5)&1)
#define Z_COMPARE   ((rs.omlo>>4)&1)
#define Z_SOURCE    ((rs.omlo>>2)&1)  // 1 = primitive depth
#define ALPHA_CMP   (rs.omlo&1)
#define DITHER_ALPHA ((rs.omlo>>1)&1)

enum { CYC_1, CYC_2, CYC_COPY, CYC_FILL };

// the software RDP owns the picture only while it is the selected renderer
// and has drawn since the last reset
int softrdp_active(void)
{
    return(rs.active && inifile_softrdp());
}

void softrdp_reset(void)
{
    memset(&rs,0,sizeof(rs));
}

/****************************************************************************
** RDRAM access (mem.ram holds host-order 32-bit words)
*/

static int rd8(dword a)
{
    a&=0xffffff;
    if(a>=(dword)mem.ramsize) return(0);
    return(mem.ram[a^3]);
}

static int rd16(dword a)
{
    a&=0xfffffe;
    if(a>=(dword)mem.ramsize) return(0);
    return(*(word *)(mem.ram+(a^2)));
}

static dword rd32(dword a)
{
    a&=0xfffffc;
    if(a>=(dword)mem.ramsize) return(0);
    return(*(dword *)(mem.ram+a));
}

static void wr16(dword a,int v)
{
    a&=0xfffffe;
    if(a>=(dword)mem.ramsize) return;
    *(word *)(mem.ram+(a^2))=(word)v;
}

static void wr32(dword a,dword v)
{
    a&=0xfffffc;
    if(a>=(dword)mem.ramsize) return;
    *(dword *)(mem.ram+a)=v;
}

/****************************************************************************
** colors
*/

static Color color32(dword c)
{
    Color k;
    k.r=(c>>24)&255; k.g=(c>>16)&255; k.b=(c>>8)&255; k.a=c&255;
    return(k);
}

static Color color16(int c)
{
    Color k;
    k.r=((c>>11)&31)*255/31;
    k.g=((c>>6)&31)*255/31;
    k.b=((c>>1)&31)*255/31;
    k.a=(c&1)?255:0;
    return(k);
}

static Color colorIA16(int c)
{
    Color k;
    k.r=k.g=k.b=(c>>8)&255;
    k.a=c&255;
    return(k);
}

static int pack16(const Color *c)
{
    return(((c->r>>3)<<11)|((c->g>>3)<<6)|((c->b>>3)<<1)|(c->a>=128?1:0));
}

static int clamp255(int v)
{
    return(v<0?0:(v>255?255:v));
}

/****************************************************************************
** framebuffer
*/

static int inscissor(int x,int y)
{
    return(x>=(rs.sxh>>2) && x<(rs.sxl+3)>>2 && y>=(rs.syh>>2) && y<(rs.syl+3)>>2 &&
           x>=0 && x<rs.ciwidth);
}

static Color fbread(int x,int y)
{
    dword a=rs.ciaddr+(dword)(y*rs.ciwidth+x)*(rs.cisize==3?4:2);
    if(rs.cisize==3) return(color32(rd32(a)));
    return(color16(rd16(a)));
}

static void fbwrite(int x,int y,const Color *c)
{
    dword a=rs.ciaddr+(dword)(y*rs.ciwidth+x)*(rs.cisize==3?4:2);
    if(rs.cisize==3) wr32(a,((dword)c->r<<24)|(c->g<<16)|(c->b<<8)|c->a);
    else             wr16(a,pack16(c));
}

static void fbfill(int x,int y)
{
    dword a=rs.ciaddr+(dword)(y*rs.ciwidth+x)*(rs.cisize==3?4:2);
    if(rs.cisize==3) wr32(a,rs.fill);
    else             wr16(a,(x&1)?rs.fill&0xffff:rs.fill>>16);
}

/****************************************************************************
** depth buffer: 16 bits per pixel, a 14-bit floating-point z (3-bit
** exponent = leading ones of the 18-bit z, 11-bit mantissa) and 2 bits of
** dz. Formats as angrylion's z_com_table / z_dec_table; games clear it with
** fill color 0xFFFC (the farthest z).
*/

static const struct { int shift,add; } zdec[8]={
    {6,0x00000},{5,0x20000},{4,0x30000},{3,0x38000},
    {2,0x3c000},{1,0x3e000},{0,0x3f000},{0,0x3f800}
};

static int zcompress(int z) // 18-bit z -> 16-bit depth word (dz 0)
{
    int e=0;
    if(z<0) z=0;
    if(z>0x3ffff) z=0x3ffff;
    while(e<7 && (z&(1<<(17-e)))) e++;
    return(((e<<11)|((z>>zdec[e].shift)&0x7ff))<<2);
}

static int zdecompress(int word) // 16-bit depth word -> 18-bit z
{
    int z=(word>>2)&0x3fff,e=z>>11;
    return(((z&0x7ff)<<zdec[e].shift)+zdec[e].add);
}

static dword zaddress(int x,int y)
{
    return(rs.zaddr+(dword)(y*rs.ciwidth+x)*2);
}

// z test (z<0: the primitive has no depth): nearer (smaller) passes; decal
// mode also passes an equal depth, for surfaces drawn over themselves
static int ztest(int x,int y,int z)
{
    int old;
    if(z<0 || !Z_COMPARE) return(1);
    old=zdecompress(rd16(zaddress(x,y)));
    return(ZMODE==3?z<=old:z<old);
}

static void zwrite(int x,int y,int z)
{
    if(z>=0 && Z_UPDATE) wr16(zaddress(x,y),zcompress(z));
}

/****************************************************************************
** TMEM and texels
*/

static int tmem8(int a)  { return(rs.tmem[a&0xfff]); }
static int tmem16(int a) { a&=0xffe; return((rs.tmem[a]<<8)|rs.tmem[a+1]); }

// a texture coordinate (1/32 texel) shifted and made relative to the
// tile's origin (lo is 10.2): texel index in bits 5.., fraction in 0-4
static int texshift(int s,int shift,int lo)
{
    if(shift<11) s>>=shift;
    else         s<<=16-shift;
    return(s-(lo<<3));
}

// texel index v on the tile, after clamp, mirror and mask
static int texwrap(int v,int lo,int hi,int clamp,int mirror,int mask)
{
    if(clamp || !mask)
    {
        int max=(hi-lo)>>2;
        if(v<0) v=0;
        if(v>max) v=max;
    }
    if(mask)
    {
        if(mirror && (v&(1<<mask))) v=~v;
        v&=(1<<mask)-1;
    }
    return(v);
}

static Color tlut(int index)
{
    int c=tmem16(0x800+index*8);
    return(TLUT_TYPE?colorIA16(c):color16(c));
}

// the texel at index (x,y) of the tile (before wrapping)
static Color texelat(Tile *tp,int x,int y)
{
    Color c={0,0,0,0};
    int   base,swap,v;
    x=texwrap(x,tp->sl,tp->sh,tp->cs,tp->ms,tp->masks);
    y=texwrap(y,tp->tl,tp->th,tp->ct,tp->mt,tp->maskt);
    base=tp->tmem*8+y*tp->line*8;
    swap=(y&1)?4:0; // odd rows have their 32-bit words swapped

    switch(tp->size)
    {
    case 0: // 4-bit
        v=tmem8((base+(x>>1))^swap);
        v=(x&1)?v&15:v>>4;
        if(EN_TLUT) return(tlut((tp->pal<<4)|v));
        if(tp->fmt==3)
        { // IA4: 3-bit intensity, 1-bit alpha
            c.r=c.g=c.b=((v>>1)*255)/7;
            c.a=(v&1)?255:0;
        }
        else c.r=c.g=c.b=c.a=v*17; // I4 (and CI4 without TLUT)
        return(c);
    case 1: // 8-bit
        v=tmem8((base+x)^swap);
        if(EN_TLUT) return(tlut(v));
        if(tp->fmt==3)
        { // IA8
            c.r=c.g=c.b=(v>>4)*17;
            c.a=(v&15)*17;
        }
        else c.r=c.g=c.b=c.a=v;
        return(c);
    case 2: // 16-bit
        v=tmem16((base+x*2)^swap);
        if(EN_TLUT) return(tlut(v>>8));
        if(tp->fmt==3) return(colorIA16(v));
        return(color16(v));
    case 3: // 32-bit: red/green in low TMEM, blue/alpha in high
        {
            int a=((base+x*2)^swap)&0x7fe;
            int hi=tmem16(a),lo=tmem16(a|0x800);
            c.r=hi>>8; c.g=hi&255; c.b=lo>>8; c.a=lo&255;
            return(c);
        }
    }
    return(c);
}

static int lerp3(int c00,int c10,int c01,int c11,int fs,int ft)
{
    if(fs+ft<32) return(c00+((fs*(c10-c00)+ft*(c01-c00)+16)>>5));
    return(c11+(((32-fs)*(c01-c11)+(32-ft)*(c10-c11)+16)>>5));
}

// a sample of tile ti at s,t (1/32 texel). Filtered mode (other modes
// SAMPLE_TYPE, not in copy mode) blends three of the four neighbours by the
// fractions, as the RDP's triangle filter does; point mode takes one texel.
static Color texel(int ti,int s,int t)
{
    Tile *tp=&rs.tile[ti&7];
    int   ss=texshift(s,tp->shifts,tp->sl),st=texshift(t,tp->shiftt,tp->tl);
    int   x=ss>>5,y=st>>5,fs=ss&31,ft=st&31;
    Color c00,c10,c01,c11,r;

    c00=texelat(tp,x,y);
    if(!SAMPLE_TYPE || CYCLETYPE==CYC_COPY || (!fs && !ft)) return(c00);
    c10=texelat(tp,x+1,y);
    c01=texelat(tp,x,y+1);
    c11=texelat(tp,x+1,y+1);
    r.r=lerp3(c00.r,c10.r,c01.r,c11.r,fs,ft);
    r.g=lerp3(c00.g,c10.g,c01.g,c11.g,fs,ft);
    r.b=lerp3(c00.b,c10.b,c01.b,c11.b,fs,ft);
    r.a=lerp3(c00.a,c10.a,c01.a,c11.a,fs,ft);
    return(r);
}

/****************************************************************************
** combiner and blender
*/

typedef struct
{
    Color comb,t0,t1,shade;
} Inputs;

static int cc_rgb(int sel,int kind,const Inputs *in,int ch)
{
    const Color *c=NULL;
    switch(sel)
    {
    case 0: c=&in->comb; break;
    case 1: c=&in->t0; break;
    case 2: c=&in->t1; break;
    case 3: c=&rs.prim; break;
    case 4: c=&in->shade; break;
    case 5: c=&rs.env; break;
    }
    if(c) return(ch==0?c->r:(ch==1?c->g:c->b));
    switch(kind)
    {
    case 0: // sub a
        if(sel==6) return(255);
        if(sel==7) return(rand()&255); // noise
        return(0);
    case 1: // sub b: key center, K4
        if(sel==6) return(rs.keyc[ch]);
        if(sel==7) return(rs.k[4]);
        return(0);
    case 2: // mul
        switch(sel)
        {
        case 6:  return(rs.keys[ch]);   // key scale
        case 15: return(rs.k[5]);
        case 7:  return(in->comb.a);
        case 8:  return(in->t0.a);
        case 9:  return(in->t1.a);
        case 10: return(rs.prim.a);
        case 11: return(in->shade.a);
        case 12: return(rs.env.a);
        case 14: return(rs.primlodfrac);
        }
        return(0);
    default: // add
        return(sel==6?255:0);
    }
}

static int cc_alpha(int sel,int mul,const Inputs *in)
{
    switch(sel)
    {
    case 0: return(mul?0:in->comb.a); // mul: LOD fraction
    case 1: return(in->t0.a);
    case 2: return(in->t1.a);
    case 3: return(rs.prim.a);
    case 4: return(in->shade.a);
    case 5: return(rs.env.a);
    case 6: return(mul?rs.primlodfrac:255);
    }
    return(0);
}

static Color combine(const Combine *k,const Inputs *in)
{
    Color r;
    int   ch,v[3];
    for(ch=0;ch<3;ch++)
    {
        int a=cc_rgb(k->suba_rgb,0,in,ch);
        int b=cc_rgb(k->subb_rgb,1,in,ch);
        int m=cc_rgb(k->mul_rgb,2,in,ch);
        int d=cc_rgb(k->add_rgb,3,in,ch);
        v[ch]=clamp255((((a-b)*m+0x80)>>8)+d);
    }
    r.r=v[0]; r.g=v[1]; r.b=v[2];
    {
        int a=cc_alpha(k->suba_a,0,in);
        int b=cc_alpha(k->subb_a,0,in);
        int m=cc_alpha(k->mul_a,1,in);
        int d=cc_alpha(k->add_a,0,in);
        r.a=clamp255((((a-b)*m+0x80)>>8)+d);
    }
    return(r);
}

// blender inputs of one cycle: P*A + M*B
static Color blendin(int sel,const Color *pix,const Color *mem)
{
    switch(sel)
    {
    case 0: return(*pix);
    case 1: return(*mem);
    case 2: return(rs.blend);
    default: return(rs.fog);
    }
}

static Color blender(int cyc,const Color *pix,const Color *mem,int shadea,int last)
{
    int   m1a=(rs.omlo>>(30-2*cyc))&3;
    int   m1b=(rs.omlo>>(26-2*cyc))&3;
    int   m2a=(rs.omlo>>(22-2*cyc))&3;
    int   m2b=(rs.omlo>>(18-2*cyc))&3;
    Color p=blendin(m1a,pix,mem),m=blendin(m2a,pix,mem),r;
    int   a,b,div;

    switch(m1b)
    {
    case 0:  a=pix->a; break;
    case 1:  a=rs.fog.a; break;
    case 2:  a=shadea; break;
    default: a=0; break;
    }
    switch(m2b)
    {
    case 0:  b=255-a; break;
    case 1:  b=mem->a; break;
    case 2:  b=255; break;
    default: b=0; break;
    }
    // without force blend a fully covered pixel takes the first input
    if(last && !FORCE_BLEND)
    {
        p.a=pix->a;
        return(p);
    }
    div=(last && !FORCE_BLEND)?a+b:255;
    if(div<=0) div=255;
    r.r=clamp255((p.r*a+m.r*b)/div);
    r.g=clamp255((p.g*a+m.g*b)/div);
    r.b=clamp255((p.b*a+m.b*b)/div);
    r.a=pix->a;
    return(r);
}

// one pixel through the 1/2-cycle pipeline; s,t in 1/32 texel, sn,tn the
// next pixel's coordinates; z 18-bit (<0: no depth)
static void shadepixel(int x,int y,const Color *shade,int s,int t,int sn,int tn,
                       int tile,int z)
{
    Inputs in;
    Color  c,mem;
    int    two=(CYCLETYPE==CYC_2);

    if(!inscissor(x,y)) return;
    if(!ztest(x,y,z)) return;
    in.shade=*shade;
    in.t0=texel(tile,s,t);
    in.t1=texel(tile+1,s,t);
    in.comb.r=in.comb.g=in.comb.b=in.comb.a=0;

    if(two)
    {
        in.comb=combine(&rs.cc[0],&in);
        // the texture pipeline advances for the second cycle: TEXEL0 is
        // the first cycle's TEXEL1, TEXEL1 the next pixel's first texel
        // (angrylion combiner.c)
        in.t0=in.t1;
        in.t1=texel(tile,sn,tn);
        c=combine(&rs.cc[1],&in);
    }
    else c=combine(&rs.cc[1],&in);

    if(ALPHA_CMP)
    {
        int threshold=DITHER_ALPHA?(rand()&255):rs.blend.a;
        if(c.a<threshold) return;
    }

    if(IMAGE_READ) mem=fbread(x,y);
    else           mem.r=mem.g=mem.b=mem.a=0;
    if(two)
    {
        Color first=blender(0,&c,&mem,shade->a,0);
        first.a=c.a;
        c=blender(1,&first,&mem,shade->a,1);
    }
    else c=blender(0,&c,&mem,shade->a,1);
    c.a=255; // full coverage
    fbwrite(x,y,&c);
    zwrite(x,y,z);
}

// rectangles have only the primitive depth
static int rectz(void)
{
    return(Z_SOURCE?rs.primz:-1);
}

/****************************************************************************
** rectangles
*/

static void fillrect(const dword *w)
{
    int xl=(w[0]>>12)&0xfff,yl=w[0]&0xfff;
    int xh=(w[1]>>12)&0xfff,yh=w[1]&0xfff;
    int x,y,x0,x1,y0,y1;
    Color shade={0,0,0,0};

    if(CYCLETYPE>=CYC_COPY)
    { // fill and copy modes include the lower right edge
        x0=xh>>2; x1=(xl>>2)+1;
        y0=yh>>2; y1=(yl>>2)+1;
    }
    else
    {
        x0=(xh+3)>>2; x1=(xl+3)>>2;
        y0=(yh+3)>>2; y1=(yl+3)>>2;
    }
    for(y=y0;y<y1;y++) for(x=x0;x<x1;x++)
    {
        if(CYCLETYPE==CYC_FILL)
        {
            if(inscissor(x,y)) fbfill(x,y);
        }
        else if(CYCLETYPE!=CYC_COPY) shadepixel(x,y,&shade,0,0,0,0,0,rectz());
    }
}

static void texrect(const dword *w,int flip)
{
    int xl=(w[0]>>12)&0xfff,yl=w[0]&0xfff;
    int tile=(w[1]>>24)&7;
    int xh=(w[1]>>12)&0xfff,yh=w[1]&0xfff;
    int s0=(short)(w[2]>>16),t0=(short)(w[2]&0xffff);   // s10.5
    int dsdx=(short)(w[3]>>16),dtdy=(short)(w[3]&0xffff); // s5.10
    int x,y,x0,x1,y0,y1,copy=(CYCLETYPE==CYC_COPY);
    Color shade={0,0,0,0};

    if(CYCLETYPE==CYC_FILL) return;
    if(copy)
    {
        x0=xh>>2; x1=(xl>>2)+1;
        y0=yh>>2; y1=(yl>>2)+1;
        dsdx>>=2; // copy mode writes 4 pixels per step of dsdx
    }
    else
    {
        x0=(xh+3)>>2; x1=(xl+3)>>2;
        y0=(yh+3)>>2; y1=(yl+3)>>2;
    }
    for(y=y0;y<y1;y++)
    {
        for(x=x0;x<x1;x++)
        {
            // position from the rectangle's corner, in 1/1024 texel
            int dx=x-(xh>>2),dy=y-(yh>>2);
            int s,t,sn,tn;
            if(flip)
            {
                s=s0*32+dy*dsdx;
                t=t0*32+dx*dtdy;
                sn=s; tn=t+dtdy; // the next pixel (x+1)
            }
            else
            {
                s=s0*32+dx*dsdx;
                t=t0*32+dy*dtdy;
                sn=s+dsdx; tn=t;
            }
            s>>=5; t>>=5; sn>>=5; tn>>=5; // back to 1/32 texel
            if(copy)
            {
                Color c;
                if(!inscissor(x,y)) continue;
                c=texel(tile,s,t);
                if(ALPHA_CMP && c.a==0) continue;
                c.a=255;
                fbwrite(x,y,&c);
            }
            else shadepixel(x,y,&shade,s,t,sn,tn,tile,rectz());
        }
    }
}

/****************************************************************************
** triangles
*/

static int sext(dword v,int bits)
{
    int s=32-bits;
    return(((int)(v<<s))>>s);
}

// attribute n of a coefficient block: int16 parts at words 0-1, fractions
// at 4-5; d/dx at 2-3 and 6-7; d/de at 8-9 and 12-13; d/dy at 10-11, 14-15
static double coef(const dword *g,int word,int n)
{
    int  w=word+(n>>1);
    int  hi=!(n&1);
    int  i=hi?(short)(g[w]>>16):(short)(g[w]&0xffff);
    int  f=hi?(g[w+4]>>16):(g[w+4]&0xffff);
    return((double)(int)(((dword)i<<16)|(dword)f)/65536.0);
}

// texture coordinates (1/32 texel) at dy,dx from the start point: s,t,w
// and their slopes; perspective divides per pixel (w is 1.0 at 0x7fff)
static void texst(const double *t,const double *tdx,const double *tde,
                  double dy,double dx,int *s,int *tt)
{
    double ss=t[0]+tde[0]*dy+tdx[0]*dx;
    double st=t[1]+tde[1]*dy+tdx[1]*dx;
    if(PERSP)
    {
        double ww=t[2]+tde[2]*dy+tdx[2]*dx;
        if(ww>0.0001) { ss=ss/ww*32768.0; st=st/ww*32768.0; }
    }
    *s=(int)floor(ss);
    *tt=(int)floor(st);
}

static void triangle(const dword *w)
{
    int    op=(w[0]>>24)&0x3f;
    int    lft=(w[0]>>23)&1;
    int    tile=(w[0]>>16)&7;
    double yl=sext(w[0],14)/4.0;
    double ym=sext(w[1]>>16,14)/4.0;
    double yh=sext(w[1],14)/4.0;
    double xl=(int)w[2]/65536.0,dxl=(int)w[3]/65536.0;
    double xh=(int)w[4]/65536.0,dxh=(int)w[5]/65536.0;
    double xm=(int)w[6]/65536.0,dxm=(int)w[7]/65536.0;
    const dword *sh=NULL,*tx=NULL,*zb=NULL;
    double c[4]={0,0,0,0},cdx[4]={0,0,0,0},cde[4]={0,0,0,0};
    double t[3]={0,0,0},tdx[3]={0,0,0},tde[3]={0,0,0};
    double z=0,zdx=0,zde=0;
    double ys=floor(yh);
    int    py,i,n=8;

    if(op&4) { sh=w+n; n+=16; }
    if(op&2) { tx=w+n; n+=16; }
    if(op&1)
    { // depth: z, dz/dx, dz/de, dz/dy as s15.16
        zb=w+n;
        z=(int)zb[0]/65536.0; zdx=(int)zb[1]/65536.0; zde=(int)zb[2]/65536.0;
    }
    if(sh) for(i=0;i<4;i++)
    {
        c[i]=coef(sh,0,i); cdx[i]=coef(sh,2,i); cde[i]=coef(sh,8,i);
    }
    if(tx) for(i=0;i<3;i++)
    {
        t[i]=coef(tx,0,i); tdx[i]=coef(tx,2,i); tde[i]=coef(tx,8,i);
    }

    for(py=(int)ys;py<yl;py++)
    {
        double y=py+0.5,major,minor,left,right,dy=py-ys;
        int    px,x0,x1;
        if(y<yh || y>=yl) continue;
        major=xh+dxh*dy;
        minor=(y<ym)?xm+dxm*dy:xl+dxl*(py-ym);
        if(lft) { left=major; right=minor; }
        else    { left=minor; right=major; }
        x0=(int)ceil(left-0.5);
        x1=(int)ceil(right-0.5);
        for(px=x0;px<x1;px++)
        {
            double dx=px+0.5-major;
            Color  shade;
            int    s=0,tt=0,sn=0,tn=0,pz=-1;
            shade.r=clamp255((int)(c[0]+cde[0]*dy+cdx[0]*dx));
            shade.g=clamp255((int)(c[1]+cde[1]*dy+cdx[1]*dx));
            shade.b=clamp255((int)(c[2]+cde[2]*dy+cdx[2]*dx));
            shade.a=clamp255((int)(c[3]+cde[3]*dy+cdx[3]*dx));
            if(!sh) shade.r=shade.g=shade.b=shade.a=0;
            if(tx)
            {
                texst(t,tdx,tde,dy,dx,&s,&tt);
                texst(t,tdx,tde,dy,dx+1,&sn,&tn);
            }
            // 18-bit z (the s15.16 value's top bits), or the primitive's
            if(Z_SOURCE) pz=rs.primz;
            else if(zb)
            {
                double v=(z+zde*dy+zdx*dx)*8.0;
                pz=v<0?0:(v>0x3ffff?0x3ffff:(int)v);
            }
            if(CYCLETYPE==CYC_FILL)
            {
                if(inscissor(px,py)) fbfill(px,py);
            }
            else shadepixel(px,py,&shade,s,tt,sn,tn,tile,pz);
        }
    }
}

/****************************************************************************
** texture loads
*/

// texel of the texture image, as the bytes TMEM gets
static void loadtile(const dword *w)
{
    Tile *tp=&rs.tile[(w[1]>>24)&7];
    int  sl=((w[0]>>12)&0xfff)>>2,tl=(w[0]&0xfff)>>2;
    int  sh=((w[1]>>12)&0xfff)>>2,th=(w[1]&0xfff)>>2;
    int  s,t;

    tp->sl=(w[0]>>12)&0xfff; tp->tl=w[0]&0xfff;
    tp->sh=(w[1]>>12)&0xfff; tp->th=w[1]&0xfff;
    for(t=tl;t<=th;t++)
    {
        int row=tp->tmem*8+(t-tl)*tp->line*8;
        int swap=((t-tl)&1)?4:0;
        for(s=sl;s<=sh;s++)
        {
            dword src;
            int   i=s-sl;
            switch(rs.tisize)
            {
            case 0: // 4-bit: two texels per byte
                if(i&1) break;
                src=rs.tiaddr+((dword)(t*rs.tiwidth+s)>>1);
                rs.tmem[((row+(i>>1))^swap)&0xfff]=(byte)rd8(src);
                break;
            case 1:
                src=rs.tiaddr+(dword)(t*rs.tiwidth+s);
                rs.tmem[((row+i)^swap)&0xfff]=(byte)rd8(src);
                break;
            case 2:
                {
                    int a=((row+i*2)^swap)&0xffe,v;
                    src=rs.tiaddr+(dword)(t*rs.tiwidth+s)*2;
                    v=rd16(src);
                    rs.tmem[a]=(byte)(v>>8); rs.tmem[a+1]=(byte)v;
                }
                break;
            case 3:
                {
                    int   a=((row+i*2)^swap)&0x7fe;
                    dword v;
                    src=rs.tiaddr+(dword)(t*rs.tiwidth+s)*4;
                    v=rd32(src);
                    rs.tmem[a]=(byte)(v>>24); rs.tmem[a+1]=(byte)(v>>16);
                    rs.tmem[a|0x800]=(byte)(v>>8); rs.tmem[(a|0x800)+1]=(byte)v;
                }
                break;
            }
        }
    }
}

static void loadblock(const dword *w)
{
    Tile *tp=&rs.tile[(w[1]>>24)&7];
    int  sl=(w[0]>>12)&0xfff,tl=w[0]&0xfff;
    int  sh=(w[1]>>12)&0xfff,dxt=w[1]&0xfff;
    int  texels=sh-sl+1,bytes,i;
    dword src;
    int  tmembase=tp->tmem*8;
    int  linecount=0;

    tp->sl=sl<<2; tp->tl=tl<<2; tp->sh=sh<<2; tp->th=tl<<2;
    bytes=(rs.tisize==0)?(texels+1)>>1:texels<<(rs.tisize-1);
    src=rs.tiaddr+(dword)(((tl*rs.tiwidth+sl)<<rs.tisize)>>1);
    // 64-bit words; dxt advances the line counter per word (1.11), words
    // on odd lines are stored with their 32-bit halves swapped
    for(i=0;i<bytes;i+=8)
    {
        int  swap=((linecount>>11)&1)?4:0;
        int  j;
        for(j=0;j<8;j++)
        {
            int v=rd8(src+i+j);
            if(rs.tisize==3)
            { // 32-bit: 4 bytes per texel split into two banks
                int texel=(i+j)>>2,part=(i+j)&3;
                int a=((tmembase+texel*2)^swap)&0x7fe;
                if(part<2) rs.tmem[a+part]=(byte)v;
                else       rs.tmem[(a|0x800)+part-2]=(byte)v;
            }
            else rs.tmem[((tmembase+i+j)^swap)&0xfff]=(byte)v;
        }
        linecount+=dxt;
    }
}

static void loadtlut(const dword *w)
{
    Tile *tp=&rs.tile[(w[1]>>24)&7];
    int  sl=((w[0]>>12)&0xfff)>>2,tl=(w[0]&0xfff)>>2;
    int  sh=((w[1]>>12)&0xfff)>>2;
    int  i;
    dword src=rs.tiaddr+(dword)(tl*rs.tiwidth+sl)*2;
    // each 16-bit entry is stored four times, one per TMEM bank
    for(i=0;i<=sh-sl;i++)
    {
        int v=rd16(src+i*2),k;
        int a=(tp->tmem*8+i*8)&0xff8;
        for(k=0;k<4;k++)
        {
            rs.tmem[a+k*2]=(byte)(v>>8);
            rs.tmem[a+k*2+1]=(byte)v;
        }
    }
}

/****************************************************************************
** commands
*/

int softrdp_cmdwords(dword w0)
{
    int op=(w0>>24)&0x3f;
    if(op>=0x08 && op<=0x0f)
    {
        int n=8;
        if(op&4) n+=16;
        if(op&2) n+=16;
        if(op&1) n+=4;
        return(n);
    }
    if(op==0x24 || op==0x25) return(4);
    return(2);
}

void softrdp_cmd(const dword *w,int words)
{
    int op=(w[0]>>24)&0x3f;

    if(!rs.active)
    {
        rs.active=1;
        rs.sxl=rs.syl=0xfff;
        print("rdp: drawing RDP command lists in software\n");
    }
    if(words<softrdp_cmdwords(w[0])) return;

    switch(op)
    {
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        triangle(w);
        break;
    case 0x24: texrect(w,0); break;
    case 0x25: texrect(w,1); break;
    case 0x2a: // SET_KEY_GB: widths unused (keying in the blender isn't done)
        rs.keyc[1]=(w[1]>>24)&255; rs.keys[1]=(w[1]>>16)&255;
        rs.keyc[2]=(w[1]>>8)&255;  rs.keys[2]=w[1]&255;
        break;
    case 0x2b: // SET_KEY_R
        rs.keyc[0]=(w[1]>>8)&255; rs.keys[0]=w[1]&255;
        break;
    case 0x2c: // SET_CONVERT: K0..K5, 9 bits each
        rs.k[0]=(w[0]>>13)&0x1ff; rs.k[1]=(w[0]>>4)&0x1ff;
        rs.k[2]=((w[0]&15)<<5)|(w[1]>>27);
        rs.k[3]=(w[1]>>18)&0x1ff; rs.k[4]=(w[1]>>9)&0x1ff; rs.k[5]=w[1]&0x1ff;
        break;
    case 0x2e: // SET_PRIM_DEPTH: 15-bit z (and dz, unused)
        rs.primz=((w[1]>>16)&0x7fff)<<3;
        break;
    case 0x2d: // SET_SCISSOR
        rs.sxh=(w[0]>>12)&0xfff; rs.syh=w[0]&0xfff;
        rs.sxl=(w[1]>>12)&0xfff; rs.syl=w[1]&0xfff;
        break;
    case 0x2f: rs.omhi=w[0]; rs.omlo=w[1]; break;
    case 0x30: loadtlut(w); break;
    case 0x32: // SET_TILE_SIZE
        {
            Tile *tp=&rs.tile[(w[1]>>24)&7];
            tp->sl=(w[0]>>12)&0xfff; tp->tl=w[0]&0xfff;
            tp->sh=(w[1]>>12)&0xfff; tp->th=w[1]&0xfff;
        }
        break;
    case 0x33: loadblock(w); break;
    case 0x34: loadtile(w); break;
    case 0x35: // SET_TILE
        {
            Tile *tp=&rs.tile[(w[1]>>24)&7];
            tp->fmt=(w[0]>>21)&7; tp->size=(w[0]>>19)&3;
            tp->line=(w[0]>>9)&0x1ff; tp->tmem=w[0]&0x1ff;
            tp->pal=(w[1]>>20)&15;
            tp->ct=(w[1]>>19)&1; tp->mt=(w[1]>>18)&1;
            tp->maskt=(w[1]>>14)&15; tp->shiftt=(w[1]>>10)&15;
            tp->cs=(w[1]>>9)&1; tp->ms=(w[1]>>8)&1;
            tp->masks=(w[1]>>4)&15; tp->shifts=w[1]&15;
            if(tp->masks>10) tp->masks=10;
            if(tp->maskt>10) tp->maskt=10;
        }
        break;
    case 0x36: fillrect(w); break;
    case 0x37: rs.fill=w[1]; break;
    case 0x38: rs.fog=color32(w[1]); break;
    case 0x39: rs.blend=color32(w[1]); break;
    case 0x3a: rs.prim=color32(w[1]); rs.primlodfrac=w[0]&255; break;
    case 0x3b: rs.env=color32(w[1]); break;
    case 0x3c: // SET_COMBINE
        rs.cc[0].suba_rgb=(w[0]>>20)&15; rs.cc[0].mul_rgb=(w[0]>>15)&31;
        rs.cc[0].suba_a  =(w[0]>>12)&7;  rs.cc[0].mul_a  =(w[0]>>9)&7;
        rs.cc[1].suba_rgb=(w[0]>>5)&15;  rs.cc[1].mul_rgb=w[0]&31;
        rs.cc[0].subb_rgb=(w[1]>>28)&15; rs.cc[1].subb_rgb=(w[1]>>24)&15;
        rs.cc[1].suba_a  =(w[1]>>21)&7;  rs.cc[1].mul_a  =(w[1]>>18)&7;
        rs.cc[0].add_rgb =(w[1]>>15)&7;  rs.cc[0].subb_a =(w[1]>>12)&7;
        rs.cc[0].add_a   =(w[1]>>9)&7;   rs.cc[1].add_rgb=(w[1]>>6)&7;
        rs.cc[1].subb_a  =(w[1]>>3)&7;   rs.cc[1].add_a  =w[1]&7;
        break;
    case 0x3d: // SET_TEXTURE_IMAGE
        rs.tifmt=(w[0]>>21)&7; rs.tisize=(w[0]>>19)&3;
        rs.tiwidth=(w[0]&0x3ff)+1; rs.tiaddr=w[1]&0x3ffffff;
        break;
    case 0x3e: // SET_Z_IMAGE
        rs.zaddr=w[1]&0x3ffffff;
        break;
    case 0x3f: // SET_COLOR_IMAGE
        rs.cifmt=(w[0]>>21)&7; rs.cisize=(w[0]>>19)&3;
        rs.ciwidth=(w[0]&0x3ff)+1; rs.ciaddr=w[1]&0x3ffffff;
        break;
    }
}
