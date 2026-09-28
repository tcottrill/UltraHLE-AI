#include "ultra.h"
#include <time.h>
#include <direct.h>
#include "zip/miniz.h"

// whole frame collected, drawn in bursts
#define MAXVX       65536 // Destruction Derby races load over 20000
#define MAXPR       8192
#define MAXTXT      1000  // max textures in a frame
#define MAXLOAD     2048  // max textures loaded
#define MAXCOMB     256   // max different combinemodes

#define GEOMFLAGS   0 //X_DUMPDATA

//#define VERTEXARRAY

// globals
int     showwire;
int     showinfo;
int     showtest;
int     showtest2;

const float colscale=(1.0/256.0);

static char *fmt[]={"RGBA","YUV","CI","IA","I","?5","?6","?7"};
static char *bpp[]={"4b","8b","16b","32b"};
static char *cm []={"WRAP","MIRROR","CLAMP","CLAMP3"};

typedef struct
{
    int     tmembase;
    int     tmemrl;
    int     fmt;
    int     bpp;
    int     xs,ys;
    int     cmt,maskt,shiftt;
    int     cms,masks,shifts;
    int     cmsset,cmtset; // cms/cmt as set, before txt_masksize
    int     x0,y0;
    int     x1,y1;
    int     x0full,y0full;
    int     palette;
    // from preceding settimg
    dword   membase;
    int     memrl;
    dword   memx0;
    dword   memy0;
    dword   memfmt;
    dword   membpp;
    dword   crc;
    // mapped to where
    int     texture;
    int     fromfb;
    //
    int     settilemark;
} Tile;

typedef struct
{
    // info on what loaded
    int     fromfb;
    dword   membase;
    dword   memx0;
    dword   memy0;
    dword   mempal;
    dword   memfmt;
    dword   membpp;
    dword   memcrc;
    int     memxs,memys;
    int     tilefmt,tilebpp,memrl,tmemrl,tluttype;
    // when loaded/used
    int     creation_vidframe;
    int     used_vidframe;
    int     txtslot; // rst.loadtxt slot
    // handle
    int     xhandle;
    int     xs,ys; // scaled size
    int     cms,cmt;
    int     masks,maskt;
} Texture;

// bufs
#define RDP_BUF_TXT 0
#define RDP_BUF_Z   1
#define RDP_BUF_C   2

// bpps
#define RDP_BPP_4   0
#define RDP_BPP_8   1
#define RDP_BPP_16  2
#define RDP_BPP_32  3

// formats
#define RDP_FMT_RGBA 0
#define RDP_FMT_YUV  1
#define RDP_FMT_CI   2
#define RDP_FMT_IA   3
#define RDP_FMT_I    4

// blending
#define RDP_BLEND_DUNNO      0
#define RDP_BLEND_MULALPHA   1 // 0055
#define RDP_BLEND_AA_OPAQUE  2 // 0044
#define RDP_BLEND_NORMAL     3 // 0F0A or 0A0F or 0000
#define RDP_BLEND_ALPHAMIX   4 // 0050
#define RDP_BLEND_ALPHAMIX2  5 // 0040
#define RDP_BLEND_DARKEN     6 // 4C40

// combine indices
#define RDP_X     0
#define RDP_Y     1
#define RDP_M     2
#define RDP_A     3
#define RDP_TYPE  4

static char *cname[32]={
"0","1","SHADE","PRIM","ENV",
"SHAA","PRIA","ENVA","PLODF",
"BLEND","FOG","FILL",
"?","?","?","?",
"TEX0","TEX1","TEX0A","TEX1A",
"COMB","COMBA","LODF","NOISE","K4","K5","CENTER","SCALE","DUNNO",
"?","?","?"};

// colors
#define C_ZERO   0
#define C_ONE    1
#define C_SHADE  2
#define C_PRIM   3
#define C_ENV    4
// color alphas
#define C_SHAA   5
#define C_PRIA   6
#define C_ENVA   7
#define C_PLODF  8
// nonblendable colors
#define C_BLEND  9
#define C_FOG    10
#define C_FILL   11
// texture sources (not usable as colors really)
#define C_TEX0   16 // gray in rst.col
#define C_TEX1   17
#define C_TEX0A  18
#define C_TEX1A  19
// misc sources (not usable as colors really)
#define C_COMB   20 // combined cycle0
#define C_COMBA  21 // combined cycle0
#define C_LODF   22 // combined cycle0
#define C_NOISE  23 // combined cycle0
#define C_K4     24 // combined cycle0
#define C_K5     25 // combined cycle0
#define C_CENTER 26 // combined cycle0
#define C_SCALE  27 // combined cycle0
#define C_DUNNO  28 // combined cycle0
// overlayed on unused elements
#define C_ALP50  24 // 255,255,255,128
#define C_ALL50  25 // 128,128,128,128
#define C_ALL25  26 //  64, 64, 64, 64
#define C_COL50  27 // 128,128,128,255
#define C_ALP75  28 // 255,255,255,192
// total num
#define C_FULL   31 // special handlin
#define C_NUM    32
#define C_INDEX  0x7f
#define C_NEGATE 0x80

#define C_SPECIALCASE (65536*2)
#define C_MULCASE     (65536*1) // x*y
#define C_SUBCASE     (65536*4) // x-y

#define ISCOLOR(x)    (!((x)&16))
#define C_MUL(x,y)    ((x)+((y)<<8)+C_MULCASE) // NOTE: C_NEGATE only allowed for x
#define C_SUB(x,y)    ((x)+((y)<<8)+C_SUBCASE) // NOTE: C_NEGATE only allowed for x

char *stylename[8]={"Ignore","Const","Add","Mul","MulAdd","Blend","Full","Comb"};

// combine styles
#define STYLE_IGNORE   0
#define STYLE_CONST    1  // A
#define STYLE_ADD      2  // X+A
#define STYLE_MUL      3  // X*M
#define STYLE_MULADD   4  // X*M+A
#define STYLE_BLEND    5  // Y->X,M
#define STYLE_FULL     6  // (X-Y)*M+A
#define STYLE_COMB     7  // CONST, but also COMB

// Combine mode drawing method
typedef struct
{
    // original mode for this description
    dword   combine0,combine1;
    dword   other0,other1;
    int     dualtxt;     // second X_MODE, txt[1] used.
    // parsed combine state
    int     passes;      // 1 or 2
    int     texenable;   // 0=notxt, +1=text0used, +2=text1used
    int     env;         // envcolor C_*
    int     forcezcmp[4]; // >0=force, cmp and update forced then too
    int     forcezupd[4];
    int     forceatst[4]; // -1=force off, +1=force 0.05, +2=0.25, +3=0.5
    // parsed combine gouraurd color mixing [cycle][color,alpha]
    int     blend1[4];    // x_blend(1,_)
    int     blend2[4];    // x_blend(_,2)
    int     txt   [4];    // which tile to use for texture on passes
    int     col   [4][2]; // source + mul*256 + negate *65536
    int     com   [4][2]; // x_combine()
} Combine;

typedef struct
{
    // large internal tables (at start to preserve alignment better)
    Vertex    vxtab[MAXVX];
    int       vxtabinited[MAXVX];
    Primitive prtab[MAXPR];
    xt_pos    vxpos[MAXVX];
    dword     tmemsrc[4096/8*2]; // where textures loaded to tmem
    dword     tmemrl [4096/8*2]; // where textures loaded to tmem
    dword     tmemx0 [4096/8*2]; // where textures loaded to tmem
    dword     tmemy0 [4096/8*2]; // where textures loaded to tmem
    // render to texture: an offscreen color buffer drawn into an OpenGL FBO
    // (x_rtt_*) and written back to RDRAM when the game moves on
    int       rtt_on,rtt_w,rtt_bpp,rtt_maxy;
    dword     rtt_addr;
    int       rtt_saved[4]; // init.gfxwid/gfxhig/viewportwid/viewporthig
    float     gamevp[4];    // the display list's viewport (N64 pixels)
    int       gamevpset;
    int       tmemlen [4096/8*2]; // bytes of the load starting here (0: none)
    int       tmemline[4096/8*2]; // its TMEM line (LOADTILE), 0 for LOADBLOCK
    int       txtload[MAXLOAD];
    Texture   txt[MAXTXT]; // 0 not used
    Tile      tile[8];

    int       vxtabinitedcnt;

    int       lastloadb;

    int       dualtmu;

    int       nexttexturetile;
    int       texturetile;
    int       tritile;      // G_TEXTURE's tile, for HLE triangles

    int       rectmode;
    dword     rawfillcolor;

    // initdone?
    int       opened;
    int       fullscreen;

    // current frame
    int       myframe;
    int       txtrefreshcnt;
    int       starttimeus;
    int       fillrectcnt;

    int       swapflag;
    int       swapcnt;

    int       testcnt;

    int       tris;

    int       firstfillrect;

    int       geyemode;

    // big data
    ushort    palette[256];
    int       txtloads;

    // tables for whole frame
    int       vxtabi;
    int       prtabi;
    // fog settings
    float     fogmin,fogmax;
    int       fogenable;
    int       fogcolor;
    int       foglasttype;

    // framebuffer texturing detection
    dword     lastcbufs[8];      // recent color image addresses (ring)
    int       lastcbufwid[8];    // their widths in pixels
    int       lastcbufbytes[8];  // their bytes per pixel
    int       lastcbufhig[8];    // their heights: lowest scissor line used (0 = unknown)
    int       lastcbufi;
    int       scissorhig;        // current scissor's lower edge in lines
    char     *framegrab;         // RGBA copy of the last presented frame
    int       grabw,grabh;       // its size (init.gfxwid x init.gfxhig)
    int       grabframe;         // frontframe it was taken from
    dword     frontcbuf;         // color image of the last presented frame
    int       frontcbufwid;
    int       frontcbufbytes;
    int       frontcbufhig;
    int       frontframe;        // counts presented frames
    // a finished task's picture waits until the game shows its color
    // image, since a frame can take several tasks (Paper Mario draws its
    // background and its scene as two tasks into one buffer)
    int       presentpending;
    dword     pendingcbuf;
    int       pendingcbufwid;
    int       pendingcbufbytes;
    int       pendingcbufhig;
    int       pendingage;        // retraces the frame has waited

    // viewport
    int       view_x0;
    int       view_x1;
    int       view_y0;
    int       view_y1;

    // active colors
    byte      col[C_NUM][4];
    float     colf[C_NUM][4];
    // memory segmenets
    dword     segment[16];
    // buffers [txt,z,c]
    dword     bufbase[3];
    dword     buffmt[3];
    dword     bufbpp[3];
    dword     bufwid[3];
    // last tlut load
    dword     tlut_base;
    int       tlut_palbase;
    int       tlut_num;
    dword     tlut_lastbase;
    int       tlut_lastpalbase;
    int       tlut_lastnum;

    // combined (and actually used) s,t transform
    float     txt_uadd;
    float     txt_vadd;
    float     txt_uscale;
    float     txt_vscale;
    // for second texture in dual mode
    float     txt_uadd2;
    float     txt_vadd2;
    float     txt_uscale2;
    float     txt_vscale2;

    // mode (active texture considered part of mode)
    // [active,last,backup used by fillrect]
    int       modechange;             // 1=set changed, 2=set all
    int       firstmodechange;
    int       txtchange;
    int       lastusedtile;
    int       last_prtabi;            // the last mode spans from this to current tritabi
    dword     texture1[3];
    dword     texture2[3];
    dword     other0[3];
    dword     other1[3];
    dword     combine0[3];
    dword     combine1[3];

    int       flat; // from dlist.c
    int       setflat; // from dlist.c

    // temporary stuff for determining combine modes
    // [c0_color,c0_alpha,c1_color,c1_alpha] [x,m,a,y] -> (x-y)*m+a
    int       s_combx[4][4];
    int       s_combxbak[4][4]; // for dumping only
    int       s_combinecycles;
    // basic results
    int       s_combinestyle[4];
    int       s_combinetex[4];    // texture present (+1,+2)
    int       s_combinetexboth;   // texture present (+1,+2)
    int       s_combinetexbothbak;   // texture present (+1,+2)

    // parsed state
    int       s_cycles;
    int       s_txtfilt; // 0=pointsample
    int       s_zcmp;
    int       s_zupd;
    int       s_noz;
    int       s_zmode; // decal/overlay modes
    int       s_cvgmode; // decal/overlay modes
    int       s_tluttype;
    int       s_zsrc;
    float     k4,k5;   // SETCONVERT's combiner constants (0..1)
    // raw RDP lists: SET_PRIM_DEPTH (0..1), and whether new vertices take
    // it (rectangles with primitive depth selected); see rdp_rawcmd
    float     primz;
    int       rectzon;
    int       s_alphatst;
    int       s_forcebl;
    int       s_blendbits;
    int       s_blend1,s_blend2;
    Combine  *s_c;
    int       lastalphatst;
    int       lastzmode;
    // combine cache (also takes other into account)
    Combine   combcache[MAXCOMB];
    Combine   combdummy;
    int       combcacheused;
    // temp parameters for texrect
    TexRect   texrect;
    // multiword commands (texrect)
    int       wordcmd;
    int       wordsleft;

    // debugging
    int       debugwirecolor; // 0=don't draw wire
    // debugging counts
    int       cnt_setting;
    int       cnt_texture;
    int       cnt_texturegen;

    int       frameopen;
} RendState;

static Vertex rdpdummyvx[MAXRDPVX];

RendState rst;

// publics (used by dlist.c)
Vertex   *rdpvx[MAXRDPVX]; // ptrs to active vertices
char      rdpvxflag[MAXRDPVX];

#define COM (*rst.s_c)

void newmode(void);
void newtextures(void);
void flushprims(void);
static void drawprims_n64(int i0,int i1);
static void drawprims_dump(int i0,int i1);

// the N64 combiner (n64_combine): draw state (texenable) and decoded inputs
static Combine comn64;
int     rdp_n64combiner=1;
static int n64cc[2][4],n64ac[2][4],n64cycles;

/****************************************************************************
/* snapshot (F10/F12, 'screen [file]')
**
** A request is served on the thread that renders, right after a frame is
** finished (rdp_present), or at the next retrace for a screen the game
** doesn't redraw (rdp_snapshotpoll). main_command runs 'screen' on the thread that
** calls it, and the UI thread (the menu, F10/F12) has no GL context, so
** reading the picture there never worked. When the caller does own the
** context (the emulator thread's startup commands, 'sgo ; screen') it is
** served at once, from the last finished frame.
**
** snap_write takes a tightly packed, top-row-first RGBA8 buffer (x_readfb
** already flips GL's bottom-left rows) and writes the PNG.
*/

static volatile int snaprequest;
static char         snapname[MAXFILE];

// <name> (relative to the exe folder), or snap\<rom title>_<time>.png
static void snap_write(const unsigned char *rgba,int w,int h,const char *name0)
{
    char   name[MAXFILE],title[33];
    const char *base;
    size_t len=0;
    void  *png;
    FILE  *f;
    int    i;

    if(name0 && *name0)
    {
        if(name0[0]=='\\' || strchr(name0,':')) strcpy(name,name0);
        else sprintf(name,"%s%s",init.rootpath,name0);
        base=strrchr(name,'\\');
        if(!strchr(base?base:name,'.')) strcat(name,".png");
    }
    else
    {
        time_t    now=time(NULL);
        struct tm t;
        char      stamp[MAXFILE];

        // title as in pif_savename: trailing spaces off, bad characters '_'
        memcpy(title,cart.title,32);
        title[32]=0;
        for(i=31;i>=0 && (title[i]==' ' || !title[i]);i--) title[i]=0;
        for(i=0;title[i];i++) if(strchr("\\/:*?\"<>|",title[i]) || title[i]<32) title[i]='_';
        if(!title[0]) strcpy(title,"unknown");

        localtime_s(&t,&now);
        sprintf(stamp,"%ssnap",init.rootpath);
        _mkdir(stamp);
        sprintf(stamp,"%ssnap\\%s_%04i%02i%02i%02i%02i%02i",init.rootpath,title,
            t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec);
        // more than one in a second: _2, _3, ...
        sprintf(name,"%s.png",stamp);
        for(i=2;i<100 && (f=fopen(name,"rb"))!=NULL;i++)
        {
            fclose(f);
            sprintf(name,"%s_%i.png",stamp,i);
        }
    }

    png=tdefl_write_image_to_png_file_in_memory_ex(rgba,w,h,4,&len,6,MZ_FALSE);
    if(!png)
    {
        print("snapshot: PNG encoding failed\n");
        return;
    }
    f=fopen(name,"wb");
    if(!f)
    {
        print("snapshot: can't write %s\n",name);
        mz_free(png);
        return;
    }
    fwrite(png,1,len,f);
    fclose(f);
    mz_free(png);
    print("snapshot saved: %s (%ix%i)\n",name,w,h);
}

// the last finished frame; call on the rendering thread
static void snap_take(void)
{
    int w=init.gfxwid,h=init.gfxhig;
    unsigned char *buf=malloc((size_t)w*h*4);

    snaprequest=0;
    if(!buf) return;
    if(x_readfb(X_FB_FRONT|X_FB_RGBA8888,0,0,w,h,(char *)buf,w*4))
        print("snapshot: no picture to read\n");
    else snap_write(buf,w,h,snapname);
    free(buf);
}

// once per retrace (emulator thread): a still screen presents no new frames
void rdp_snapshotpoll(void)
{
    if(snaprequest && x_hascontext()) snap_take();
}

void rdp_screenshot(char *name)
{
    if(!rst.opened)
    {
        print("snapshot: no game picture yet\n");
        return;
    }
    snapname[0]=0;
    if(name) strncpy(snapname,name,sizeof(snapname)-5);
    snaprequest=1;
    if(x_hascontext()) snap_take();
    else print("snapshot: taken at the next retrace\n");
}

/****************************************************************************
/* Helper routines for setting state
*/

static __inline dword address(dword address)
{ // segment convert to physical address (as dlist.c: low 4 bits, 24-bit sum)
    int seg=(address>>24)&0x0f;
    return( ((address&0xffffff) + rst.segment[seg]) & 0xffffff );
}

static void setbuffer(int i,dword c0,dword c1)
{
    dword addr=address(c1);

    rst.bufbase[i]=addr;
    rst.buffmt [i]=FIELD(c0,21,3);
    rst.bufbpp [i]=FIELD(c0,19,2);
    // the width is 10 bits; libdragon's rdpq keeps other data in bits 10-11
    // (a 320 wide buffer read as 3392 covered its textures: CBUFSOURCE)
    rst.bufwid [i]=FIELD(c0,0,10)+1;
    if(st.dumpgfx) logd("\n+buffer %c %08X fmt=%i bpp=%i wid=%i",i==RDP_BUF_C?'C':'Z',
        addr,rst.buffmt[i],rst.bufbpp[i],rst.bufwid[i]);
    if(i==RDP_BUF_C)
    {
        // Remember recent color buffers so textures read from them can be
        // recognized (this used to write a 0xefefffef marker into the
        // game's own framebuffer memory).
        int bytes=(1<<rst.bufbpp[i])>>1;
        if(bytes<1) bytes=1;
        if(addr!=rst.lastcbufs[rst.lastcbufi])
        {
            rst.lastcbufi=(rst.lastcbufi+1)&7;
            rst.lastcbufhig[rst.lastcbufi]=0;
        }
        rst.lastcbufs    [rst.lastcbufi]=addr;
        rst.lastcbufwid  [rst.lastcbufi]=rst.bufwid[i];
        rst.lastcbufbytes[rst.lastcbufi]=bytes;
        if(rst.scissorhig>rst.lastcbufhig[rst.lastcbufi])
            rst.lastcbufhig[rst.lastcbufi]=rst.scissorhig;
    }
}

// Is addr inside a color buffer? A buffer is as high as the lowest scissor
// line drawn into it, else taken to be w*3/4 lines (320x240, 640x480).
// Xena's 640x240 buffer read as 480 lines covered its font textures, which
// then came from the (black) rendered picture. Returns 1 for the last
// presented frame's buffer (a framegrab can supply it), 2 for another
// recent color buffer, 0 otherwise.
static int cbuf_contains(dword base,int wid,int bytes,int hig,dword addr)
{
    base&=0x1fffffff;
    addr&=0x1fffffff;
    if(hig<=0) hig=wid*3/4;
    return(wid>0 && addr>=base && addr<base+(dword)(wid*bytes*hig));
}

static int cbufsource(dword addr)
{
    int j;
    if(cbuf_contains(rst.frontcbuf,rst.frontcbufwid,rst.frontcbufbytes,rst.frontcbufhig,addr)) return(1);
    for(j=0;j<8;j++)
    {
        if(cbuf_contains(rst.lastcbufs[j],rst.lastcbufwid[j],rst.lastcbufbytes[j],rst.lastcbufhig[j],addr)) return(2);
    }
    return(0);
}

Vertex *newvx(void)
{
    int i;
    if(rst.vxtabi>=MAXVX)
    {
        error("rdp: too many vertices in frame (max %i)",MAXVX);
        i=MAXVX-1; // reuse the last slot rather than write past the table
    }
    else i=rst.vxtabi++;
    rst.vxtab[i].zs=rst.rectzon?rst.primz:-1.0f; // depth from pos[2] (w)
    rst.vxtab[i].col[0]=1.0f;
    rst.vxtab[i].col[1]=0.0f;
    rst.vxtab[i].col[2]=1.0f;
    rst.vxtab[i].col[3]=1.0f;
    return(rst.vxtab+i);
}

Primitive *newpr(void)
{
    int i;
    if(rst.prtabi>=MAXPR)
    {
        // full: draw the queued ones (all complete and in the current mode,
        // since a mode change flushes) and start the table again. The clamp
        // here used to be overwritten by the increment below, so a frame
        // with more than MAXPR primitives wrote past the table.
        static int told;
        if(!told++) print("rdp: over %i primitives in a frame, drawing in parts\n",MAXPR);
        flushprims();
        rst.prtabi=0;
        rst.last_prtabi=0;
    }
    i=rst.prtabi++;
    return(rst.prtab+i);
}

void setintensityfromalpha(int di,int si)
{
    byte *d=rst.col[di];
    byte *s=rst.col[si];
    float *df=rst.colf[di];
    float *sf=rst.colf[si];
    d[0]=s[3];
    d[1]=s[3];
    d[2]=s[3];
    d[3]=s[3];
    df[0]=sf[3];
    df[1]=sf[3];
    df[2]=sf[3];
    df[3]=sf[3];
}

static __inline void setcolor(int ci,dword a)
{
    byte *col=rst.col[ci];
    float *colf=rst.colf[ci];
    col[3]=a; a>>=8;
    col[2]=a; a>>=8;
    col[1]=a; a>>=8;
    col[0]=a;
    colf[0]=colscale*col[0];
    colf[1]=colscale*col[1];
    colf[2]=colscale*col[2];
    colf[3]=colscale*col[3];
}

static void setfillcolor(int ci,dword c)
{
    byte col[4];
    int a;
    a=FIELD(c,11,5); col[3]=8*a+(a>>2);
    a=FIELD(c, 6,5); col[2]=8*a+(a>>2);
    a=FIELD(c, 1,5); col[1]=8*a+(a>>2);
    a=FIELD(c, 0,1); col[0]=255;
    setcolor(ci,*(dword *)col);
}

static void setcolorintensity(int ci,int a)
{
    byte col[4];
    col[0]=a;
    col[1]=a;
    col[2]=a;
    col[3]=a;
    setcolor(ci,*(dword *)col);
}

void rdp_freetexmem(void)
{
    int i;
    for(i=1;i<MAXTXT;i++)
    {
        if(rst.txt[i].xhandle)
        {
            x_freetexture(rst.txt[i].xhandle);
        }
        memset(&rst.txt[i],0,sizeof(Texture));
    }
    x_cleartexmem();
}

/****************************************************************************
/* DEBUG Info overlays and other debug stuff
*/

struct
{
    int x,y;
} testdot[1024];

static float dxm=+1.0/318;
static float dym=-1.0/238;
static float dxa=-1.0;
static float dya=+1.0;

static float oxm=+2.0/320;
static float oym=-2.0/240;
static float oxa=-1.0;
static float oya=+1.0;

void rdp_addtestdot(int y)
{
    static int i=0;
    testdot[i].x=639;
    testdot[i].y=y;
    i++;
    i&=1023;
}

void viewport(int rectmode)
{
    float x0,y0,x1,y1;

    if(rst.rectmode==rectmode) return;
    rst.rectmode=rectmode;

    if(rst.prtabi!=rst.last_prtabi)
    {
        // flush prims before viewport changes
        flushprims();
    }

    if(rectmode)
    {
        x0=0;
        y0=0;
        x1=init.gfxwid-1;
        y1=init.gfxhig-1;
    }
    else
    {
        x0=rst.view_x0;
        y0=rst.view_y0;
        x1=rst.view_x1;
        y1=rst.view_y1;
    }

    if(showinfo)
    {
        x0=x0*0.48+320*0.01;
        y0=y0*0.48+240*1.01;
        x1=x1*0.48+320*0.01;
        y1=y1*0.48+240*1.01;
    }
    x_viewport(x0,y0,x1,y1);

    // add to wirelist
    {
        Primitive *p;
        Vertex    *v;
        p=newpr();
        p->c[0]=NULL;
        p->c[1]=NULL;
        p->c[2]=v=newvx();
        v->pos[0]=x0;
        v->pos[1]=y0;
        v->tex[0]=x1;
        v->tex[1]=y1;
        // skip this in drawing
        rst.last_prtabi=rst.prtabi;
    }
}

static void realdrawmode(void)
{
    x_projection(90,0.9,32768.0);
    viewport(0);
    x_projmatrix(NULL);
    x_matrix(NULL);
    x_reset();
    x_geometry(GEOMFLAGS);
    x_flush();
}

static void debugdrawmode(void)
{
    x_projection(90,0.9,32768.0);
    x_viewport(0,0,init.gfxwid-1,init.gfxhig-1);
    x_projmatrix(NULL);
    x_matrix(NULL);
    x_reset();
    x_geometry(GEOMFLAGS);
    x_flush();
}

__inline static void orthovxpos(float x,float y)
{
    x_vxpos(x*dxm+dxa,y*dym+dya,1.0);
}

static void drawdot(int x,int y)
{
    x_vxcolor(0.9,0.5,0.1);
    x_begin(X_QUADS);
    orthovxpos(x+0,y+0);
    orthovxpos(x+0,y+2);
    orthovxpos(x+2,y+2);
    orthovxpos(x+2,y+0);
    x_end();
}

void debugdrawtexture(int tilepos,int txti,int size)
{
    int xhandle=rst.txt[txti].xhandle;
    int xn,s1,s2,x,y;
    float a,b;

    xn=640/size;
    xn&=~1;
    s1=1;
    s2=size-1;

    x=size*(tilepos%xn);
    y=size*(tilepos/xn);

    a=0.5/rst.txt[txti].xs;
    b=0.5/rst.txt[txti].ys;

    x_texture(rst.txt[txti].xhandle);

    x_begin(X_QUADS);
    x_vxtex(0.0+a,0.0+b); orthovxpos(x+s1,y+s1);
    x_vxtex(0.0+a,1.0-b); orthovxpos(x+s1,y+s2);
    x_vxtex(1.0-a,1.0-b); orthovxpos(x+s2,y+s2);
    x_vxtex(1.0-a,0.0+b); orthovxpos(x+s2,y+s1);
    x_end();
}

static void drawmarkers(void)
{ // framecount marker
    int i;

    for(i=0;i<1024;i++)
    {
        if(testdot[i].x>0)
        {
            drawdot(testdot[i].x,478-testdot[i].y);
            testdot[i].x-=2;
        }
    }
}

void drawtextures(void)
{
    int i,size;
    if(rst.txtloads<=0) return;

    for(size=64;size>8;size-=4)
    {
        if((rst.txtloads/(640/size)+2)*size<=240) break;
    }

    x_blend(X_ALPHA,X_INVOTHERALPHA);
    x_combine(X_TEXTURE);
    for(i=0;i<rst.txtloads;i++)
    {
        debugdrawtexture(i,rst.txtload[i],size);
    }
}

char *colortext(byte *c)
{
    static char buf[16];
    sprintf(buf,"%02X%02X%02X%02X",c[0],c[1],c[2],c[3]);
    return(buf);
}

/****************************************************************************
** Textures
*/

static byte buf1[256*256*4];
static byte buf2[256*256*4];
static byte mbuf[1024*4];

int txt_findstart(Tile *t)
{
    dword i;
    i=(((t->membase+(t->y0<<8))&0xffffff)/17)%(MAXTXT-4)+1;
    return((int)i);
}

int txt_findempty(Tile *t)
{
    int i;
    int j,besti,bestframe;

    besti=i=txt_findstart(t);
    bestframe=0x7fffffff;

    // find oldest nearby texture
    for(j=0;j<MAXTXT/8;j++)
    {
        i++; if(i>=MAXTXT) i=1;
        if(rst.txt[i].used_vidframe<bestframe)
        {
            bestframe=rst.txt[i].used_vidframe;
            besti=i;
        }
    }

    return(besti);
}

// texture cache key for the TLUT mode (othermode tlut type 0-3), and
// TLUT_RAWINDEX for CI texels drawn with no TLUT into an 8-bit color image
#define TLUT_RAWINDEX 4
static int txt_tlutkey(void)
{
    if(rst.rtt_on && rst.rtt_bpp==1 && !rst.s_tluttype) return(TLUT_RAWINDEX);
    return(rst.s_tluttype);
}

int txt_findmatch(Tile *t)
{
    Texture *txt;
    int i,j;

    i=txt_findstart(t);

    // find texture matching parameters
    for(j=0;j<MAXTXT/8;j++)
    {
        i++; if(i>=MAXTXT) i=1;
        txt=rst.txt+i;
        if(txt->membase==t->membase &&
           txt->memy0  ==t->memy0   &&
           txt->memx0  ==t->memx0   &&
           txt->cms    ==t->cms     &&
           txt->cmt    ==t->cmt     &&
           txt->masks  ==t->masks   &&
           txt->maskt  ==t->maskt   &&
           txt->memxs  ==t->xs      &&
           txt->memys  ==t->ys      &&
           txt->mempal ==t->palette &&
           txt->membpp ==t->membpp  &&
           txt->memfmt ==t->memfmt  &&
           txt->tilefmt==t->fmt     &&
           txt->tilebpp==t->bpp     &&
           txt->memrl  ==t->memrl   &&
           txt->tmemrl ==t->tmemrl  &&
           txt->tluttype==txt_tlutkey() && i)
        {
            if(txt->memcrc==t->crc)
            {
                return(i);
            }
            else
            {
                return(-i);
            }
        }
    }
    return(0);
}

void txt_scale(byte *dst,int dx,int dy,int drl,
               byte *src,int sx,int sy,int srl)
{
    int x,y,x1,y1;
    int xmul=16384*sx/dx;
    int ymul=16384*sy/dy;
    dword *dw=(dword *)dst;
    dword *sw=(dword *)src;

    if(st.dumpgfx) logd("\n+tile scale (%i,%i,%i)->(%i,%i,%i) mul (%04X,%04X) "
        ,sx,sy,srl,dx,dy,drl,xmul,ymul);

    if(sx<1 || sy<1 || sx>512 || sy>256)
    {
        logd("\n+tile INVALID SRC SIZE!");
        memset(dst,0,dy*drl*4);
        return;
    }
    if(dx<1 || dy<1 || dx>512 || dy>256)
    {
        logd("\n+tile INVALID DST SIZE!");
        memset(dst,0,dy*drl*4);
        return;
    }

    for(y=0;y<dy;y++) for(x=0;x<dx;x++)
    {
        x1=(x*xmul) >> 14;
        y1=(y*ymul) >> 14;
        dw[x+y*drl]=sw[x1+y1*srl];
    }

    /*
    xmul=16384*dx/sx;
    ymul=16384*dy/sy;
    memset(dw,0,dy*drl*4);
    for(y=0;y<sy;y++) for(x=0;x<sx;x++)
    {
        x1=(x*xmul) >> 14;
        y1=(y*ymul) >> 14;
        dw[x1+y1*drl]=sw[x+y*srl];
    }
    */
}

void txt_fill(byte *dst,int dx,int dy,dword col)
{
    dword *dw=(dword *)dst;
    int    x,y;
    for(y=0;y<dy;y++) for(x=0;x<dx;x++)
    {
        dw[x+y*dx]=col;
    }
}

int txt_checkalpha(byte *dst,int dx,int dy)
{
    dword *dw=(dword *)dst;
    int    x,y;
    dword  a;
    for(y=0;y<dy;y++) for(x=0;x<dx;x++)
    {
        a=dw[x+y*dx];
        if(a<0xf0000000) return(1);
    }
    return(0);
}

void txt_border(byte *dst,int dx,int dy,dword col)
{
    dword *dw=(dword *)dst;
    int    x,y;
    for(y=0;y<dy;y++)
    {
        x=0;
        dw[x+y*dx]=col;
        x=dx-1;
        dw[x+y*dx]=col;
    }
    for(x=0;x<dx;x++)
    {
        y=0;
        dw[x+y*dx]=col;
        y=dy-1;
        dw[x+y*dx]=col;
    }
}

void txt_mirrorx(byte *dst,int dx,int dy,byte *src)
{
    dword *dw=(dword *)dst;
    dword *ds=(dword *)src;
    int    rl=dx*2;
    int    x,y;
    for(y=dy-1;y>=0;y--)
    {
        for(x=0;x<dx;x++) dw[x+y*rl]=ds[x+y*dx];
        for(x=0;x<dx;x++) dw[dx+x+y*rl]=ds[(dx-1-x)+y*dx];
    }
}

void txt_mirrory(byte *dst,int dx,int dy,byte *src)
{
    dword *dw=(dword *)dst;
    dword *ds=(dword *)src;
    int    y;
    for(y=0;y<dy;y++)
    {
        memcpy(dw+y*dx,ds+y*dx,dx*4);
    }
    for(y=0;y<dy;y++)
    {
        memcpy(dw+(dy+y)*dx,ds+(dy-1-y)*dx,dx*4);
    }
}

dword crcmem(dword addr,int size,int samples)
{
    int i,ia;
    dword crc=0;

    if(samples<=0) ia=4;
    else
    {
        ia=size/samples;
        ia&=~3;
        if(!ia) ia=4;
    }
    // FNV-1a over 32-bit words: every sampled word changes the result
    crc=2166136261u;
    for(i=0;i<size;i+=ia)
    {
        crc^=mem_read32p(addr+i);
        crc*=16777619u;
    }

    return(crc);
}

int txt_rl(Tile *t)
{
    int rl;
//    if(t->memrl<=1) rl=t->xs*realbpp/8;
    if(t->memrl<=1)
    {
        // LOADBLOCK: rows follow the tile's TMEM line. 32-bit texels are
        // split into RG and BA halves of TMEM, so the line covers only half
        // of a row's bytes in RDRAM.
        rl=t->tmemrl;
        if(t->bpp==3) rl*=2;
        // no line on the tile: the row length its dxt gave (-memrl)
        if(rl<=0 && t->memrl<-1) rl=-t->memrl;
    }
    else rl=t->memrl;
    return(rl);
}

dword txt_calccrc(Tile *t)
{
    int    size,rl;
    dword  crc,madd,addr;
    int    realbpp=4<<t->bpp;

    // textures read from a color buffer: the last presented frame comes from
    // the framegrab (a new key each frame, so it is reloaded when the frame
    // changes); other color buffers keep the placeholder pattern
    t->fromfb=cbufsource(t->membase);
    if(t->fromfb==1) return(0xfb000000^rst.frontframe);
    if(t->fromfb==2) return(-2);

    rl=txt_rl(t);

    madd=t->memx0;//*realbpp/8;
    addr=t->membase+t->memy0*rl+madd;
    size=t->ys*rl;

    st2.gfx_txtbytes+=size;

    // Hash every word (a texture is at most 4K of TMEM) so changes anywhere in
    // the image or palette reload it; only oversized loads fall back to
    // sampling (4096 samples).
    crc=0;
    if(t->fmt==2) crc+=crcmem(rst.tlut_base,rst.tlut_num*2,0);
    crc+=crcmem(addr,size,size>16384?4096:0);

    if(st.dumpgfx) logd("\n+tile calccrc %08X,%i -> %08X",addr,size,crc);

    return(crc);
}

byte *txt_loadline(byte *mbuf,dword addr,int y,int flip,int rl2)
{
    int x,rl;
    dword a;
    addr+=y*rl2/2;
    rl=((rl2>>1)+3)/4;
    if(!(y&1)) flip=0;
    for(x=0;x<rl;x++)
    {
        if(addr&3)
        {
            a =mem_read8(addr+3)<<24;
            a|=mem_read8(addr+2)<<16;
            a|=mem_read8(addr+1)<<8;
            a|=mem_read8(addr+0)<<0;
        }
        else
        {
            a=mem_read32p(addr);
            a=FLIP32(a);
        }
        *(dword *)(mbuf+(x^flip)*4)=a;
        addr+=4;
    }
    return(mbuf);
}

void txt_loadtlut(int pal,int num,dword addr)
{
    int i;

    pal*=16;
    logd("\n+tile loadtlut %08X pal=%i num=%i ",
        addr,pal,num);
    if(pal+num>256)
    {
        logd("!palette overflow\n");
        warning("rdp: palette tlut load overflow (pal=%i num=%i addr=%X)",pal,num,addr);
        num=256-pal;
    }
    // 16 bits at a time: a palette may start on any halfword. StarCraft
    // 64's menu palettes are at ...16; read as words from ...14, every
    // entry was its neighbour and index 0 (transparent) an opaque lavender.
    for(i=0;i<num;i++)
    {
        rst.palette[pal++]=(word)mem_read16(addr);
        addr+=2;
    }
}

// a palette load that doesn't come through RDP LOADTLUT (S2DEX
// G_OBJ_LOADTXTR): recorded the way LOADTLUT records it, since CI texture
// conversion reloads the palette from rst.tlut_*; without that Kirby 64's
// hills took the last LOADTLUT's palette (red). Loaded now as well, for
// the S2DEX backgrounds, which read rst.palette directly.
void rdp_settlut(int pal,int num,dword addr)
{
    rst.tlut_base   =addr;
    rst.tlut_palbase=pal;
    rst.tlut_num    =num;
    rst.modechange=rst.txtchange=1;
    txt_loadtlut(pal,num,addr);
}

void txt_paletteread(byte *dst,int ind)
{
    int a,c,i;

    a=2048+ind*2;
    c=rst.palette[ind];

    if(rst.s_tluttype==3)
    { // IA
        i=(c>>8);
        a=c&255;
        dst[0]=i;
        dst[1]=i;
        dst[2]=i;
        dst[3]=a;
    }
    else
    { // RGBA
        a=FIELD(c,11,5); dst[0]=8*a+(a>>2);
        a=FIELD(c, 6,5); dst[1]=8*a+(a>>2);
        a=FIELD(c, 1,5); dst[2]=8*a+(a>>2);
        a=FIELD(c, 0,1); dst[3]=255*a;
    }
}

void txt_showpal(byte *dst,int xs,int ys,int palbase,int palnum)
{
    dword *d=(dword *)dst;
    int x,y;
    if(0)
    {
        for(y=0;y<xs;y++)
        {
            for(x=0;x<ys;x++)
            {
                d[x+y*xs]=0xff0000ff;
            }
        }
    }
    if(palnum==256)
    {
        for(y=0;y<16;y++)
        {
            for(x=0;x<16;x++)
            {
                txt_paletteread((byte *)(d+x+y*xs),palbase+x+y*16);
            }
        }
    }
    else
    {
        for(y=0;y<16;y++)
        {
            for(x=0;x<16;x++)
            {
                txt_paletteread((byte *)(d+x+y*xs),palbase+(x>>2)+(y>>2)*16);
            }
        }
    }
}

void txt_convert(byte *dst0,Tile *t)
{ // convert from texmem to 32 bit RGBA
    // Only CI4 selects a 16-entry bank; CI8 addresses the entire TLUT.
    int    palbase=(t->bpp==0)?16*t->palette:0;
    int    bpp=t->bpp;
    int    fmt=t->fmt;
    int    xs=t->xs;
    int    ys=t->ys;
    int    x,y,rl2,i,j,a;
    dword  c,addr;
    byte  *m,*dst=dst0;
    int    realbpp;
    int    madd,flip;

    realbpp=4<<t->bpp;

    // LOADBLOCK data has odd rows swapped the way TMEM reads them: 32-bit
    // words, or 64-bit ones for 32-bit texels (2 bytes each per TMEM half)
    if(t->memrl==-1) flip=(t->bpp==3)?2:1; else flip=0; // dxt 0 only
    rl2=txt_rl(t)*2;

    if(rl2>512*4*2)
    {
        print("rl=%08X xs=%i realbpp=%i t->memrl=%i\n",rl2/2,xs,realbpp,t->memrl);
        exception("rdp: invalid texture memory width");
        return;
    }

    madd=t->memx0;//*realbpp/8;
    addr=t->membase+t->memy0*rl2/2;

    if(st.dumpgfx) logd("\n+tile convert from %08X bpp=%i rl=%i (%i,%i) base(%ib,%il) ",
                          addr,realbpp,rl2/2,xs,ys,t->memx0,t->memy0);

    if(0)
    {
        char name[32];
        FILE *f1;
        sprintf(name,"ti%06X.out",t->membase&0xffffff);
        f1=fopen(name,"wb");
        for(y=0;y<ys;y++)
        {
            m=txt_loadline(mbuf,addr,y,0,rl2);
            fwrite(m,1,rl2/2,f1);
        }
        fclose(f1);
    }

    //   fmt: 0    1   2   3   4
    // bits:  RGBA YUV CI  IA  I    ok=supported
    //  4bpp  ok   -   ok  ok  ok   pa=partial
    //  8bpp  ?    -   ok  ok  ok    +=seen, not supported
    // 16bpp  ok   -   -   ok  -     -=not seen
    // 32bpp  ok   -   -   -   -     -=not seen
    // RGBA 4/8-bit do not exist on the RDP: the texels index the TLUT like
    // CI (GLideN64 decodes them as CI4/CI8 too; Scooby-Doo uses RGBA 4b).
    // With TLUT on, any 4/8-bit texel is an index: BioFreaks' menu font is
    // IA4 drawn through a 16-colour TLUT (as IA it came out as noise)
    // With TLUT off, RGBA 4/8-bit texels are intensities (GLideN64's
    // "RGBA as I"): Conker's mouth and tail base are 8-bit "RGBA" drawn
    // with no TLUT, and looked up in a stale palette they came out black
    if(bpp<=1 && fmt==0 && !rst.s_tluttype) fmt=4;
    else if(bpp<=1 && (fmt==0 || rst.s_tluttype)) fmt=2;
    // no TLUT while drawing into an 8-bit color image: the texel reaches it
    // as is, so CI texels are their index (as I) and the image gets them
    if(fmt==2 && txt_tlutkey()==TLUT_RAWINDEX) fmt=4;

    if(fmt==2)
    {
        if(1)
        { // always
            txt_loadtlut(rst.tlut_palbase,rst.tlut_num,rst.tlut_base);
        }
        else
        { // if base changed (doesn't always work, crc needed)
            if(rst.tlut_base   !=rst.tlut_lastbase    ||
               rst.tlut_palbase!=rst.tlut_lastpalbase ||
               rst.tlut_num    !=rst.tlut_lastnum     )
            {
                rst.tlut_lastbase   =rst.tlut_base   ;
                rst.tlut_lastpalbase=rst.tlut_palbase;
                rst.tlut_lastnum    =rst.tlut_num    ;
                txt_loadtlut(rst.tlut_palbase,rst.tlut_num,rst.tlut_base);
            }
        }
    }

//-------------------------------- 32bpp
    if(bpp==3 && fmt==0)
    { // RGBA 32bit 8-8-8-8
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x++)
            {
                *dst++=(m[0]);
                *dst++=(m[1]);
                *dst++=(m[2]);
                *dst++=(m[3]);
                m+=4;
            }
        }
    }
//-------------------------------- 16bpp
    else if(bpp==2 && fmt==0)
    { // RGBA 16bit 1-5-5-5
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x++)
            {
                { // 5551 rgba
                    c=(m[0]<<8)+m[1];
                    a=FIELD(c,11,5); *dst++=8*a+(a>>2);
                    a=FIELD(c, 6,5); *dst++=8*a+(a>>2);
                    a=FIELD(c, 1,5); *dst++=8*a+(a>>2);
                    a=FIELD(c, 0,1); *dst++=255*a;
                    //dst[-4]=dst[-1]; // show alpha as red
                }
                m+=2;
            }
        }
    }
    else if(bpp==2 && fmt==3)
    { // IA 16bit
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x++)
            {
                int i,a;
                a=m[1];
                i=m[0];
                *dst++=i;
                *dst++=i;
                *dst++=i;
                *dst++=a;
                m+=2;
            }
        }
    }
//-------------------------------- 8bpp
    else if(bpp==1 && fmt==3)
    { // IA 8bit
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x++)
            {
                i=((m[0]>>4)&15)*17;
                a=( m[0]    &15)*17;
                *dst++=i;
                *dst++=i;
                *dst++=i;
                *dst++=a;
                m++;
            }
        }
    }
    else if(bpp==1 && fmt==0)
    { // RGBA 8bit [NEVER USER, converted to CI 8 bit currently]
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x++)
            {
                a=(m[0]>>6)&3; a|=(a<<2); a|=(a<<4); *dst++=a;
                a=(m[0]>>4)&3; a|=(a<<2); a|=(a<<4); *dst++=a;
                a=(m[0]>>2)&3; a|=(a<<2); a|=(a<<4); *dst++=a;
                a=(m[0]>>0)&3; a|=(a<<2); a|=(a<<4); *dst++=a;
                m++;
            }
        }
    }
    else if(bpp==1 && fmt==4)
    { // I 8bit
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x++)
            {
                i=m[0];
                dst[0]=i;
                dst[1]=i;
                dst[2]=i;
                dst[3]=i;
                dst+=4;
                m++;
            }
        }
    }
    else if(bpp==1 && fmt==2)
    { // CI 8bit
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x++)
            {
                i=m[0];
                txt_paletteread(dst,i+palbase);
                dst+=4;
                m++;
            }
        }
//        txt_showpal(dst0,xs,ys,palbase,256);
    }
//-------------------------------- 4bpp
    else if(bpp==0 && fmt==0)
    { // RGBA 4bit 1-1-1-1
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x+=2)
            {
                a=m[0];
                *dst++=(m[0]&0x80)?255:0;
                *dst++=(m[0]&0x40)?255:0;
                *dst++=(m[0]&0x20)?255:0;
                *dst++=(m[0]&0x10)?255:0;
                a<<=4;
                *dst++=(m[0]&0x80)?255:0;
                *dst++=(m[0]&0x40)?255:0;
                *dst++=(m[0]&0x20)?255:0;
                *dst++=(m[0]&0x10)?255:0;
                m++;
            }
        }
    }
    else if(bpp==0 && fmt==3)
    { // IA 4bit 3-1
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x+=2)
            {
                for(j=4;j>=0;j-=4)
                {
                    i=((m[0]>>(j+1))&7)*36;
                    a=((m[0]>>(j+0))&1)*255;
                    *dst++=i;
                    *dst++=i;
                    *dst++=i;
                    *dst++=a;
                }
                m++;
            }
        }
    }
    else if(bpp==0 && fmt==4)
    { // I 4bit
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x+=2)
            {
                for(j=4;j>=0;j-=4)
                {
                    i=(m[0]>>j)&15;
                    dst[0]=i*17;
                    dst[1]=i*17;
                    dst[2]=i*17;
                    dst[3]=i*17;
                    dst+=4;
                }
                m++;
            }
        }
    }
    else if(bpp==0 && fmt==2)
    { // CI 4bit
        for(y=0;y<ys;y++)
        {
            m=madd+txt_loadline(mbuf,addr,y,flip,rl2);
            for(x=0;x<xs;x+=2)
            {
                for(j=4;j>=0;j-=4)
                {
                    i=(m[0]>>j)&15;
                    txt_paletteread(dst,i+palbase);
                    dst+=4;
                }
                m++;
            }
        }
//        txt_showpal(dst0,xs,ys,palbase,16);
    }
//-------------------------------- unknown
    else
    {
        logd("\n+tile t_converttexture bpp=%i fmt=%i ???\n",bpp,fmt);
        warning("dlist: unsupported texture format bpp=%i fmt=%i",bpp,fmt);
        for(y=0;y<ys;y++)
        {
            for(x=0;x<xs;x++)
            {
                c=((x^y)<<4)^128;
                *dst++=c;
                *dst++=c;
                *dst++=c;
                *dst++=255;
            }
        }
    }

    logd("\n+tile convert ends %08X (flip=%i)",addr+ys*rl2/2,flip);

    if(0 && flip)
    {
        dword *d=(dword *)dst0,a;
        for(y=0;y<ys;y++)
        {
            if(!(y&1))
            {
                d+=xs;
                continue;
            }
            for(x=0;x<xs;x+=2)
            {
                a=d[0];
                d[0]=d[1];
                d[1]=a;
                d+=2;
            }
        }
    }

    if(0)
    {
        char name[32];
        FILE *f1;
        sprintf(name,"ti%06X.out",addr&0xffffff);
        f1=fopen(name,"wb");
        fwrite(dst0,4,xs*ys,f1);
        fclose(f1);
        print("convert %08X %ix%i\n",addr,xs,ys);
    }
}

// Grabs the last presented frame (the x_* front copy) for textures read from
// its color buffer: at most once per presented frame, and only on demand.
void rdp_grabscreen(void)
{
    int size=init.gfxwid*init.gfxhig*4;

    if(rst.framegrab && rst.grabframe==rst.frontframe &&
       rst.grabw==init.gfxwid && rst.grabh==init.gfxhig) return;

    if(!rst.framegrab || rst.grabw!=init.gfxwid || rst.grabh!=init.gfxhig)
    {
        free(rst.framegrab);
        rst.framegrab=malloc(size);
        if(!rst.framegrab) return;
        rst.grabw=init.gfxwid;
        rst.grabh=init.gfxhig;
    }
    if(x_readfb(X_FB_FRONT|X_FB_RGBA8888,0,0,rst.grabw,rst.grabh,rst.framegrab,rst.grabw*4))
    {
        memset(rst.framegrab,0,size);
    }
    rst.grabframe=rst.frontframe;
}

// Texture from the last presented frame: each texel's RDRAM address is mapped
// to a pixel of that frame's color buffer, then scaled to the grab's size.
void txt_fromfb(byte *buf,Tile *t)
{
    dword *d=(dword *)buf,*s;
    dword  base,a,off;
    int    x,y,rl,bytes,w,h,gx,gy;

    rdp_grabscreen();
    if(!rst.framegrab || rst.frontcbufwid<=0)
    {
        memset(buf,0,t->xs*t->ys*4);
        return;
    }

    s    =(dword *)rst.framegrab;
    base =rst.frontcbuf&0x1fffffff;
    w    =rst.frontcbufwid;
    h    =w*3/4;
    bytes=(1<<t->bpp)>>1;
    if(bytes<1) bytes=1;
    rl   =txt_rl(t);

    for(y=0;y<t->ys;y++)
    {
        for(x=0;x<t->xs;x++)
        {
            a  =((t->membase+(t->memy0+y)*rl+t->memx0+x*bytes)&0x1fffffff)-base;
            off=a/rst.frontcbufbytes;
            gx =(off%w)*rst.grabw/w;
            gy =(off/w)*rst.grabh/h;
            if(gy>=rst.grabh) gy=rst.grabh-1;
            d[x+y*t->xs]=s[gx+gy*rst.grabw];
        }
    }
}

// Writes the last presented frame back into its RDRAM color buffer, for games
// that read the framebuffer with the CPU (Super Smash Bros. copies it for its
// screen wipes). Same idea as Glide64's CopyFrameBuffer; enabled per game
// with fbwrite=1 in ultra.ini because it reads the frame back every frame.
void rdp_fbwrite(void)
{
    dword *s,base,c,a,p,*d;
    int    x,y,w,h,gx,gy,bytes;

    w    =rst.frontcbufwid;
    h    =w*3/4;
    bytes=rst.frontcbufbytes;
    base =rst.frontcbuf&0x1fffffff;
    if(w<=0 || w>1024 || (bytes!=2 && bytes!=4)) return;
    if(base+(dword)(w*h*bytes)>(dword)mem.ramsize) return;

    rdp_grabscreen();
    if(!rst.framegrab) return;
    s=(dword *)rst.framegrab;

    for(y=0;y<h;y++)
    {
        gy=y*rst.grabh/h;
        for(x=0;x<w;x++)
        {
            gx=x*rst.grabw/w;
            c =s[gx+gy*rst.grabw]; // bytes R,G,B,A
            a =base+(x+y*w)*bytes;
            d =(dword *)(mem.ram+(a&~3));
            if(bytes==4)
            {
                *d=((c&255)<<24)|(((c>>8)&255)<<16)|(((c>>16)&255)<<8)|255;
            }
            else
            {
                // RGBA5551; the pixel at the lower address is the high half
                p=((c&0xf8)<<8)|((c>>5)&0x7c0)|((c>>18)&0x3e)|1;
                if(a&2) *d=(*d&0xffff0000)|p;
                else    *d=(*d&0x0000ffff)|(p<<16);
            }
        }
    }
}

void txt_fromcbuf(byte *buf,Tile *t)
{
    int x,y;
    dword *d;
    dword cols[4]={0xff000000,0xff101010,
                   0xff101010,0xff000000};

    d=(dword *)buf;
    for(y=0;y<t->ys;y++)
    {
        for(x=0;x<t->xs;x++)
        {
            d[x+y*t->xs]=cols[(y&1)*2+(x&1)];
        }
    }
}

#define SWAP(s,d) sd=s,s=d,d=sd

// texels a clamped tile repeats across itself when its wrap mask is smaller
// than the tile (clamp applies at the tile edge, the mask inside it);
// 0 when the whole tile is used
int txt_maskwrap(int cm,int mask,int size)
{
    if((cm&2) && mask && mask<=10 && (1<<mask)<size) return(1<<mask);
    return(0);
}

// RDP_SETTILESIZE: a wrap mask smaller than the tile means only 1<<mask
// texels exist and the image repeats across the tile. Without clamp the
// tile is sized to the mask and wraps (Glide64 TexCache.cpp "wrap all the
// way"). With clamp the RDP clamps at the tile edge and repeats only inside
// it (txt_loaddata builds that), which Glide64 keeps for tiles up to 256
// texels; larger clamped tiles (Smash's 320x256 castle roofs) wrap too,
// since the whole tile doesn't fit a texture.
void txt_masksize(Tile *t,dword *cmd)
{
    int ws=FIELD(cmd[1],14,10)-FIELD(cmd[0],14,10)+1;
    int wt=FIELD(cmd[1],2,10) -FIELD(cmd[0],2,10) +1;
    int shrinks=t->masks && t->masks<=8 && (1<<t->masks)<ws;
    int shrinkt=t->maskt && t->maskt<=8 && (1<<t->maskt)<wt;
    int keeps,keept;

    // a tile can be sized more than once per SETTILE
    t->cms=t->cmsset;
    t->cmt=t->cmtset;
    keeps=shrinks && (t->cms&2) && ws<=256;
    keept=shrinkt && (t->cmt&2) && wt<=256;

    if(keeps || keept)
    {
        // txt_prepare only takes tiles up to 16384 texels
        int xs=(shrinks && !keeps)?1<<t->masks:ws;
        int ys=(shrinkt && !keept)?1<<t->maskt:wt;
        if(xs*ys>16384) keeps=keept=0;
    }
    if(shrinks && !keeps)
    {
        cmd[1]=(cmd[1]&~(0x3ff<<14))|((FIELD(cmd[0],14,10)+(1<<t->masks)-1)<<14);
        t->cms&=~2;
    }
    if(shrinkt && !keept)
    {
        cmd[1]=(cmd[1]&~(0x3ff<<2))|((FIELD(cmd[0],2,10)+(1<<t->maskt)-1)<<2);
        t->cmt&=~2;
    }
}

// repeats the mx*my texels in src across dx*dy, mirroring every other copy
// in an axis with its mirror bit set
void txt_repeatmask(byte *dst,int dx,int dy,byte *src,int mx,int my,int mirx,int miry)
{
    dword *dw=(dword *)dst;
    dword *ds=(dword *)src;
    int    x,y,u,v;
    for(y=0;y<dy;y++)
    {
        v=y%my;
        if(miry && ((y/my)&1)) v=my-1-v;
        for(x=0;x<dx;x++)
        {
            u=x%mx;
            if(mirx && ((x/mx)&1)) u=mx-1-u;
            dw[x+y*dx]=ds[u+v*mx];
        }
    }
}

void txt_loaddata(Texture *txt,Tile *t)
{
    int sx,sy,mx,my;
    int flags;
    int hasalpha;
    byte *s,*d,*sd;

    if((t->xs&1) && t->bpp==0) t->xs--;

    // OpenGL takes any texture size, so textures keep the tile's size (the
    // power-of-2, 256 and aspect ratio limits were Glide's)
    sx=t->xs;
    sy=t->ys;

    s=buf1;
    d=buf2;
    // convert to RGBA
    if(st.dumpgfx) logd("\n+tile txt_convert %08X (%i,%i) rl=%ip fmt=%s bpp=%s ",
        t->membase,t->xs,t->ys,t->memrl,fmt[t->fmt],bpp[t->bpp]);

    /*
    if(cart.iszelda && t->membase==rst.bufbase[RDP_BUF_Z])
    { // reusing old screen buffer
        logd(" FRAMEBUFCOPY ");
        txt_fromfb(s,t);
        txt->fromfb=1;
    }
    */
    txt->fromfb=t->fromfb;
    mx=my=0;
    if(t->fromfb==1)
    {
        logd(" FRAMEBUFFER ");
        txt_fromfb(s,t);
    }
    else if(t->fromfb==2)
    {
        logd(" CBUFSOURCE ");
        txt_fromcbuf(s,t);
    }
    else
    {
        mx=txt_maskwrap(t->cms,t->masks,t->xs);
        my=txt_maskwrap(t->cmt,t->maskt,t->ys);
        if(mx || my)
        {
            // only the masked texels exist in TMEM: convert those and
            // repeat them across the clamped tile
            Tile mt=*t;
            if(mx) mt.xs=mx;
            if(my) mt.ys=my;
            if(st.dumpgfx) logd(" maskwrap %ix%i ",mt.xs,mt.ys);
            txt_convert(d,&mt);
            txt_repeatmask(s,t->xs,t->ys,d,mt.xs,mt.ys,
                mx && (t->cms&1),my && (t->cmt&1));
        }
        else
        {
            // convert to RGBA
            txt_convert(s,t);
        }
    }

    // check for alpha
    hasalpha=txt_checkalpha(s,t->xs,t->ys);

    // scale to power of 2
    if(sx!=t->xs || sy!=t->ys)
    {
        txt_scale(d,sx,sy,sx,s,t->xs,t->ys,t->xs);
        SWAP(d,s);
    }

    // mirror, only without clamp: the RDP clamps to the tile before it
    // mirrors, so a clamped tile shows its mirrored copies only when the
    // mask is smaller than the tile, and txt_repeatmask already built those
    // (Glide64 TexCache.cpp also mirrors only unclamped tiles)
    if(t->cms==1)
    {
        if(st.dumpgfx) logd(" mirrorx ");
        txt_mirrorx(d,sx,sy,s);
        SWAP(d,s);
        sx*=2;
    }
    if(t->cmt==1)
    {
        if(st.dumpgfx) logd(" mirrory ");
        txt_mirrory(d,sx,sy,s);
        SWAP(d,s);
        sy*=2;
    }

    txt->xs=sx;
    txt->ys=sy;
    txt->cms=t->cms;
    txt->cmt=t->cmt;
    txt->masks=t->masks;
    txt->maskt=t->maskt;

    txt->creation_vidframe=rst.myframe;

    if(hasalpha) flags=X_RGBA4444;
    else flags=X_RGBA5551;

    // a mask of 0 never wraps on the RDP, so that axis clamps too (Glide64
    // does the same). Left to GL_REPEAT, filtering at a strip's edge pulled
    // in its opposite row: upscaled, Star Fox 64's "Press START" (6-row
    // strips, mask 0) had a dark copy of the letters' tops under them.
    {
        int cs=t->cms>=2 || (!t->masks && !(t->cms&1));
        int ct=t->cmt>=2 || (!t->maskt && !(t->cmt&1));
        if(cs || ct)
        {
            flags|=X_CLAMP;
            if(!cs) flags|=X_CLAMPNOX;
            if(!ct) flags|=X_CLAMPNOY;
        }
    }

//    if(t->membase==0x8028b2b0) fill(s,sx,sy,0xff0000ff);
//    if((t->membase&0xffff0000)==0x00340000) txt_fill(s,sx,sy,0xff0000ff);

//    if(rst.s_txtfilt==0) flags|=X_NOBILIN;

    // raw CI indices for an 8-bit color image: filtering between two indices
    // makes a third, unrelated colour (Yoshi's Story backgrounds speckled)
    if(txt->tluttype==TLUT_RAWINDEX) flags|=X_NOBILIN;

    txt->xhandle=x_createtexture(flags,txt->xs,txt->ys);
    if(txt->xhandle<0) exception("too many textures\n");
    x_loadtexturelevel(txt->xhandle,0,s);

    //if(st.dumpgfx) logd("\n+tile x_create %04X size %ix%i\n",flags,txt->xs,txt->ys);
}

void txt_setscales(int tile)
{
    Tile    *t=rst.tile+tile+rst.texturetile;
    Texture *txt=rst.txt+t->texture;
    float    xd,yd,xdm,ydm;

    if(!t->texture || t->xs<=0 || t->ys<=0)
    {
        logd("\n+tile ERROR??? tile %i setscale with no texture\n",tile);
        rst.txt_uscale=1/32.0/32.0;
        rst.txt_vscale=1/32.0/32.0;
        rst.txt_uadd=0;
        rst.txt_vadd=0;
        return;
    }

    xd=(1.0/32.0)/(t->xs);
    yd=(1.0/32.0)/(t->ys);

    // RDP shifts 0..10 divide coordinates; 11..15 multiply them.
    if(t->shifts>0 && t->shifts<=10) xdm=1.0/(1 <<     t->shifts );
    else if(t->shifts>10)           xdm=    (1 << (16-t->shifts));
    else                            xdm=1.0;
    if(t->shiftt>0 && t->shiftt<=10) ydm=1.0/(1 <<     t->shiftt );
    else if(t->shiftt>10)           ydm=    (1 << (16-t->shiftt));
    else                            ydm=1.0;

    if(1)
    {
        rst.txt_uadd=16.0-8.0*t->x0full/xdm;
        rst.txt_vadd=16.0-8.0*t->y0full/ydm;
    }
    else
    {
        rst.txt_uadd=16.0-8.0*t->x0full;
        rst.txt_vadd=16.0-8.0*t->y0full;
    }

    xd*=xdm;
    yd*=ydm;

    // mirror (txt_loaddata doubles only unclamped mirror tiles)
    if(t->cms==1) xd*=0.5;
    if(t->cmt==1) yd*=0.5;

    rst.txt_uscale=xd;
    rst.txt_vscale=yd;

    if(st.dumpgfx) logd("\n+tile setscales tile %i txt %i scale %.4f,%.4f add %.4f,%.4f base %i,%i",
        tile,t->texture,rst.txt_uscale,rst.txt_vscale,rst.txt_uadd,rst.txt_vadd,t->memx0,t->memy0);
}

void txt_select(int tile)
{
    Tile    *t=rst.tile+tile+rst.texturetile;
    Texture *txt=rst.txt+t->texture;

    if(st.dumpgfx) logd("\n+tile SELECT tile %i txt %i",tile,t->texture);
    x_texture(txt->xhandle);
    txt_setscales(tile);
}

void txt_select2(int tile1,int tile2)
{
    Tile    *t;
    Texture *txt;
    int      xh1,xh2;

    t=rst.tile+tile2+rst.texturetile;
    txt=rst.txt+t->texture;
    if(st.dumpgfx) logd("\n+tile SELECT-MT1 tile %i txt %i",tile1,t->texture);
    xh2=txt->xhandle;
    txt_setscales(tile2);

    rst.txt_uscale2=rst.txt_uscale;
    rst.txt_vscale2=rst.txt_vscale;
    rst.txt_uadd2=rst.txt_uadd;
    rst.txt_vadd2=rst.txt_vadd;

    t=rst.tile+tile1+rst.texturetile;
    txt=rst.txt+t->texture;
    if(st.dumpgfx) logd("\n+tile SELECT-MT2 tile %i txt %i",tile2,t->texture);
    xh1=txt->xhandle;
    txt_setscales(tile1);

    x_texture2(xh1,xh2);
}

void txt_prepare(int tile)
{
    Tile    *t=rst.tile+tile+rst.texturetile;
    Texture *txt;
    int      match=0,crcerror=0,sizeok;

    sizeok=0;
    // buf1/buf2 hold 65536 texels: up to 16384 before mirroring in x and y
    if((unsigned)t->xs<=1024 && (unsigned)t->ys<=1024 && t->xs*t->ys<=16384) sizeok=1;

    if(t->xs<=0 || t->ys<=0 || (unsigned)t->bpp>3 || (unsigned)t->fmt>7 || !sizeok)
    {
        logd("\n+tile %i ILLEGAL??? texture size (%ix%i)/format (%i/%i)",
            tile,t->xs,t->ys,t->bpp,t->fmt);
        t->texture=0;
        return;
    }

    // update tile membase
    t->membase=rst.tmemsrc[t->tmembase>>3];
    t->memrl  =rst.tmemrl [t->tmembase>>3];
    t->memx0  =rst.tmemx0 [t->tmembase>>3];
    t->memy0  =rst.tmemy0 [t->tmembase>>3];
    {
        // A tile starting inside an earlier load: Castlevania's save slot
        // panel loads a 64x32 CI8 block at TMEM 0 and draws its right 48
        // columns from TMEM byte 16 (the texture came from address 0)
        int k=(t->tmembase>>3)&511,j,off;
        if(!rst.tmemlen[k])
        {
            for(j=k-1;j>=0 && !rst.tmemlen[j];j--);
            if(j>=0 && j*8+rst.tmemlen[j]>k*8)
            {
                off=(k-j)*8;
                t->membase=rst.tmemsrc[j];
                t->memrl  =rst.tmemrl [j];
                t->memx0  =rst.tmemx0 [j];
                t->memy0  =rst.tmemy0 [j];
                if(!rst.tmemline[j]) t->membase+=off; // LOADBLOCK: linear
                else
                {
                    t->memy0+=off/rst.tmemline[j];
                    t->memx0+=off%rst.tmemline[j];
                }
                logd("\n+tile %i starts %i bytes into the load at TMEM %i",tile,off,j*8);
            }
        }
    }
    t->crc    =txt_calccrc(t);

    match=txt_findmatch(t);

    if(match>0)
    {
        t->texture=match;
        txt=rst.txt+t->texture;
        if(st.dumpgfx) logd("\n+tile %i cache hit (texture %i)",tile,t->texture);
    }
    else
    {
        if(match<0) t->texture=-match;
        else t->texture=txt_findempty(t);

        txt=rst.txt+t->texture;

        // free slot
        if(txt->xhandle) x_freetexture(txt->xhandle);
        memset(txt,0,sizeof(Texture));

        txt->membpp =t->membpp;
        txt->memfmt =t->memfmt;
        txt->membase=t->membase;
        txt->mempal =t->palette;
        txt->memx0  =t->memx0;
        txt->memy0  =t->memy0;
        txt->memxs  =t->xs;
        txt->memys  =t->ys;
        txt->memcrc =t->crc;
        txt->tilefmt=t->fmt;
        txt->tilebpp=t->bpp;
        txt->memrl=t->memrl;
        txt->tmemrl=t->tmemrl;
        txt->tluttype=txt_tlutkey();
        txt->used_vidframe=-1;

        txt_loaddata(rst.txt+t->texture,t);

        if(st.dumpgfx)
        {
            if(crcerror) logd("\n+tile %i cache crc miss (loading to texture %i)",tile,t->texture);
            else         logd("\n+tile %i cache address miss (loading to texture %i)",tile,t->texture);
        }

        rst.cnt_texturegen++;
    }

    rst.cnt_texture++;

    if(txt->used_vidframe!=rst.myframe)
    {
        txt->txtslot=rst.txtloads;
        if(rst.txtloads<MAXLOAD)
        {
            rst.txtload[rst.txtloads++]=t->texture;
        }
        else
        {
            warning("rdp: too many textureloads in frame!");
        }
        txt->used_vidframe=rst.myframe;
    }

    logd(" [txtload %i]",txt->txtslot);
}

/****************************************************************************
** Drawing
*/

static void setfog(int pass)
{
    float cr,cg,cb;
    int type;

    if(rst.fogenable)
    {
        if(pass) type=X_LINEARADD;
        else type=X_LINEAR;
    }
    else type=X_DISABLE;

    if(type==rst.foglasttype) return;

    rst.foglasttype=type;

    // send to x-engine
    if(type==X_DISABLE)
    {
        x_fog(X_DISABLE,0,0,1.0,0.5,0.1);
    }
    else
    {
        cr=rst.colf[C_FOG][0];
        cg=rst.colf[C_FOG][1];
        cb=rst.colf[C_FOG][2];
        // max*1.2... looks better, not sure what is right
        x_fog(type,rst.fogmin,rst.fogmax*1.2,cr,cg,cb);
    }
}

float getcolor(int j,int r,int i)
{
    int a;
    float x;
    a=rst.s_combx[j][r];
    x=rst.colf[a&C_INDEX][i];
    if(a&C_NEGATE) x=1.0-x;
    if(a&C_MULCASE) x*=rst.colf[(a>>8)&C_INDEX][i];
    return(x);
}

void mixcolor(float *d,float *shade,int col,int alp)
{
    int   c,c1,c2;
    float cr,cg,cb,ca;

    rst.colf[C_SHADE][0]=shade[0];
    rst.colf[C_SHADE][1]=shade[1];
    rst.colf[C_SHADE][2]=shade[2];
    rst.colf[C_SHADE][3]=shade[3];
    rst.colf[C_SHAA][0]=shade[3];
    rst.colf[C_SHAA][1]=shade[3];
    rst.colf[C_SHAA][2]=shade[3];
    rst.colf[C_SHAA][3]=shade[3];

    // color
    if(col==C_FULL || alp==C_FULL)
    {
        // full blend based on rst.s_combx
        int i,j;
        float x;

        for(i=0;i<4;i++)
        {
            if(i==3) j=1; else j=0;
            x =getcolor(j,0,i);
            x-=getcolor(j,1,i);
            x*=getcolor(j,2,i);
            x+=getcolor(j,3,i);
            if(x<0) x=0;
            if(x>1) x=1;
            d[i]=x;
        }
        /*
        print("direct %.2f %.2f %.2f %.2f shadeg %.2f  mix %i %i %i %i \n",
            d[0],d[1],d[2],d[3],
            rst.colf[C_SHADE][1],
            rst.s_combx[j][0],
            rst.s_combx[j][1],
            rst.s_combx[j][2],
            rst.s_combx[j][3]);
        */
    }

    c=col;
    if(c!=C_FULL)
    {
        c1=c&C_INDEX;
        cr=rst.colf[c1][0];
        cg=rst.colf[c1][1];
        cb=rst.colf[c1][2];
        if(c&C_NEGATE)
        {
            cr=1.0f-cr;
            cg=1.0f-cg;
            cb=1.0f-cb;
        }

        if(c&C_MULCASE)
        {
            c2=(c>>8)&C_INDEX;
            cr*=rst.colf[c2][0];
            cg*=rst.colf[c2][1];
            cb*=rst.colf[c2][2];
        }

        d[0]=cr;
        d[1]=cg;
        d[2]=cb;
    }

    // alpha
    c=alp;
    if(c!=C_FULL)
    {
        c1=c&C_INDEX;
        ca=rst.colf[c1][3];
        if(c&C_NEGATE)
        {
            ca=1.0-ca;
        }

        if(c&C_MULCASE)
        {
            c2=(c>>8)&C_INDEX;
            ca*=rst.colf[c2][3];
        }

        d[3]=ca;
    }
}

void drawprims(int p,int i0,int i1)
{
    Primitive *pr;
    Vertex *vx;
    int i,j,ji;

    rst.vxtabinitedcnt++; // forces a new init to all vertices used

    if(st.dumpgfx)
    {
        logd("\n+FLUSH pass:%i/%i txttile:%i blend:%04X/%04X comb:%04X/%04X col:%05X/%05X envc:%i",
            p+1,COM.passes,
            COM.txt[p],
            COM.blend1[p],COM.blend2[p],
            COM.com[p][0],COM.com[p][1],
            COM.col[p][0],COM.col[p][1],
            COM.env);
    }

    // draw
    if(COM.dualtxt)
    {
        if(st.dumpgfx) logd("DUALTXT! ");
        x_begin(X_TRIANGLES);
        for(i=i0;i<i1;i++)
        {
            pr=rst.prtab+i;
            for(j=0;j<3;j++)
            {
                ji=(j+2); if(ji>=3) ji-=3;
                vx=pr->c[ji];

                if(rst.vxtabinited[vx-rst.vxtab]!=rst.vxtabinitedcnt)
                {
                    // vertex not yet inited on this pass
                    rst.vxtabinited[vx-rst.vxtab]=rst.vxtabinitedcnt;

                    // color
                    mixcolor(vx->cc,vx->col,COM.col[p][0],COM.col[p][1]);

                    // texture
                    vx->ct[0]=-1;
                    vx->ct[1]=-1;
                }

                if(!j || !rst.flat)
                {
                    x_vxcolor4(vx->cc[0],vx->cc[1],vx->cc[2],vx->cc[3]);
                }
                /*
                x_vxtex ((vx->tex[0]+rst.txt_uadd)*rst.txt_uscale,
                         (vx->tex[1]+rst.txt_vadd)*rst.txt_vscale);
                x_vxtex2((vx->tex[0]+rst.txt_uadd2)*rst.txt_uscale2,
                         (vx->tex[1]+rst.txt_vadd2)*rst.txt_vscale2);
                */
                x_vxtex ((vx->tex[0]+rst.txt_uadd )*rst.txt_uscale,
                         (vx->tex[1]+rst.txt_vadd )*rst.txt_vscale);
                x_vxtex2((vx->tex[0]+rst.txt_uadd2)*rst.txt_uscale,
                         (vx->tex[1]+rst.txt_vadd2)*rst.txt_vscale);
                x_vxdepth(vx->zs);
                x_vxposv((xt_pos *)vx->pos);
            }
        }
        x_end();
    }
    else
    {
        x_begin(X_TRIANGLES);
        for(i=i0;i<i1;i++)
        {
            pr=rst.prtab+i;
            for(j=0;j<3;j++)
            {
                ji=(j+2); if(ji>=3) ji-=3;
                vx=pr->c[ji];

                if(rst.vxtabinited[vx-rst.vxtab]!=rst.vxtabinitedcnt)
                {
                    // vertex not yet inited on this pass
                    rst.vxtabinited[vx-rst.vxtab]=rst.vxtabinitedcnt;

                    // color
                    mixcolor(vx->cc,vx->col,COM.col[p][0],COM.col[p][1]);

                    // texture
                    vx->ct[0]=(vx->tex[0]+rst.txt_uadd)*rst.txt_uscale;
                    vx->ct[1]=(vx->tex[1]+rst.txt_vadd)*rst.txt_vscale;
                }

                if(!j || !rst.flat)
                {
                    x_vxcolor4(vx->cc[0],vx->cc[1],vx->cc[2],vx->cc[3]);
                }
                x_vxtex   (vx->ct[0],vx->ct[1]);
                x_vxdepth(vx->zs);
                x_vxposv  ((xt_pos *)vx->pos);
            }
        }
        x_end();
    }

    drawprims_dump(i0,i1);
}

// N64 combiner draw: vertex color is the shade itself, tile 0 and tile 1
// each get their own coordinates. Rectangles have no shade: it reads 0.
static void drawprims_n64(int i0,int i1)
{
    static const float zero[4]={0,0,0,0};
    Primitive *pr;
    Vertex *vx;
    int i,j,ji,rect=rst.rectmode==1;

    rst.vxtabinitedcnt++;
    if(st.dumpgfx) logd("\n+FLUSH n64comb cycles:%i tex:%i blend:%04X/%04X rect:%i",
        n64cycles,COM.texenable,rst.s_blend1,rst.s_blend2,rect);

    x_begin(X_TRIANGLES);
    for(i=i0;i<i1;i++)
    {
        pr=rst.prtab+i;
        for(j=0;j<3;j++)
        {
            const float *c;
            ji=(j+2); if(ji>=3) ji-=3;
            vx=pr->c[ji];
            c=rect?zero:vx->col;
            if(rst.vxtabinited[vx-rst.vxtab]!=rst.vxtabinitedcnt)
            {
                rst.vxtabinited[vx-rst.vxtab]=rst.vxtabinitedcnt;
                memcpy(vx->cc,c,sizeof(vx->cc));
                vx->ct[0]=(vx->tex[0]+rst.txt_uadd)*rst.txt_uscale;
                vx->ct[1]=(vx->tex[1]+rst.txt_vadd)*rst.txt_vscale;
            }
            if(!j || !rst.flat) x_vxcolor4(c[0],c[1],c[2],c[3]);
            x_vxtex (vx->ct[0],vx->ct[1]);
            x_vxtex2((vx->tex[0]+rst.txt_uadd2)*rst.txt_uscale2,
                     (vx->tex[1]+rst.txt_vadd2)*rst.txt_vscale2);
            x_vxdepth(vx->zs);
            x_vxposv((xt_pos *)vx->pos);
        }
    }
    x_end();

    drawprims_dump(i0,i1);
}

static void drawprims_dump(int i0,int i1)
{
    Primitive *pr;
    Vertex *vx;
    int i,j;

    if(st.dumpgfx)
    {
        for(i=i0;i<i1;i++)
        {
            pr=rst.prtab+i;
            logd("\ndrawprim");
            for(j=0;j<3;j++)
            {
                vx=pr->c[j];
                logd("\ndvx[%04i: %9.2f %9.2f %9.2f uv %5.2f %5.2f rgb %.2f %.2f %.2f %.2f ]",
                    vx-rst.vxtab,
                    vx->pos[0],vx->pos[1],vx->pos[2],
                    vx->ct[0],vx->ct[1],
                    vx->cc[0],vx->cc[1],vx->cc[2],vx->cc[3]);
                if(rst.flat && j==2) logd(" (flat)");
            }
        }
    }
}

// draw primitives collected since last modechange
// update rst.last_prtabi
void flushprims(void)
{
    int i,i0,i1;

    if(rst.prtabi==rst.last_prtabi) return;

    if(st.dumpgfx) logd("\n+FLUSH (%i prims) cycles=%i atst=%i (blenda=%02X)",
        rst.prtabi-rst.last_prtabi,rst.s_cycles,
        rst.s_alphatst,rst.col[C_BLEND][3]);

    i0=rst.last_prtabi;
    i1=rst.prtabi;
    rst.last_prtabi=rst.prtabi;

    x_geometry(GEOMFLAGS);
    x_blendalpha(rst.colf[C_FOG][3]); // X_CONSTALPHA blends
//    if(COM.blend1[0]==X_ONE) rst.s_alphatst=0;

    if(1)
    {
        rst.lastalphatst=rst.s_alphatst;
        if(rst.s_alphatst==5)
        {
            // compare value is coverage*32 (ares parallel-rdp combiner.h):
            // 255 inside a triangle, so only edge pixels could fail
            x_alphatest(1.0); // off
        }
        else if(rst.s_alphatst==4 ||
          (rst.s_alphatst==1))// && rst.colf[C_BLEND][3]>0.1))
        {
            if(0)
            {
                float f;
                f=rst.colf[C_BLEND][3];
                if(f<0.1) f=0.1;
                logd("\n+alphatst %i %.3f",rst.s_alphatst,f);
                if(rst.s_alphatst!=1) x_alphatest(0.6);
                else x_alphatest(f);
            }
            else
            {
                x_alphatest(0.25);
            }
        }
        else
        {
            int bl=rst.s_c==&comn64?rst.s_blend1:COM.blend1[0];
            if(bl==X_ALPHA) x_alphatest(0.05);
            else x_alphatest(1.0);
        }
        // Fill mode writes the fill color with no blender or alpha compare.
        // Its rectangles have alpha 0 (no shade), and a blender left in the
        // render mode by the previous 2-cycle draw made them invisible:
        // Majora's Mask's letterbox bars showed the grey clear in some frames.
        if(rst.s_cycles==4) x_alphatest(1.0);
    }

    {
        rst.lastzmode=rst.s_zmode;

        if(rst.s_zmode>0) x_zdecal(1.01);
        else x_zdecal(1.0);
    }

    // offscreen buffer: note the alpha (coverage) bit these pixels get, 2 = set,
    // 1 = clear; fills write their color's, everything else sets it (rtt_end)
    if(rst.rtt_on)
    {
        int set=1;
        if(rst.s_cycles==4)
            set=rst.rtt_bpp==3?(rst.rawfillcolor&0xff)!=0:(rst.rawfillcolor&1);
        x_rtt_cover(set?2:1);
    }

    if(rst.s_c==&comn64)
    { // the N64 combiner in the shader: one pass, the render mode's blender
        setfog(0);
        x_blend(rst.s_blend1,rst.s_blend2);
        x_n64combine(n64cycles,&n64cc[0][0],&n64ac[0][0],
                     rst.colf[C_PRIM],rst.colf[C_ENV],rst.colf[C_PLODF][0]);
        x_n64convert(rst.k4,rst.k5);
        x_mask(X_ENABLE,rst.s_zupd?X_ENABLE:X_DISABLE,
                        rst.s_zcmp?X_ENABLE:X_DISABLE);
        if(COM.texenable) txt_select2(0,1);
        drawprims_n64(i0,i1);
        return;
    }

    { // draw
        for(i=0;i<COM.passes;i++)
        {
            float c[4];
            setfog(i);
            mixcolor(c,rst.colf[C_ONE],COM.env,COM.env);
            x_envcolor(c[0],c[1],c[2],c[3]);
            if(st.dumpgfx) logd(" ENV=%.2f %.2f %.2f %.2f ",c[0],c[1],c[2],c[3]);
            x_blend(COM.blend1[i],COM.blend2[i]);
            if(rst.s_cycles==4) x_blend(X_ONE,X_ZERO); // fill mode (see above)
            if(COM.dualtxt)
            {
                x_procombine2(COM.com[i][0],COM.com[i][1],COM.dualtxt,0);
            }
            else
            {
                x_procombine(COM.com[i][0],COM.com[i][1]);
            }
            if(COM.forceatst[i]) switch(COM.forceatst[i])
            {
                case -1: x_alphatest(1.00); break;
                case  1: x_alphatest(0.05); break;
                case  2: x_alphatest(0.25); break;
                case  3: x_alphatest(0.50); break;
                case  4: x_alphatest(0.75); break;
                case  9:
                    {
                        static int cnt=0;
                        cnt++;
                        switch(cnt&3)
                        {
                        case 0: x_alphatest(0.05); break;
                        case 1: x_alphatest(0.25); break;
                        case 2: x_alphatest(0.15); break;
                        case 3: x_alphatest(0.35); break;
                        }
                    }
                    break;
            }
            if(COM.forcezcmp[i]>0)
            {
                x_mask(X_ENABLE,COM.forcezupd[i],COM.forcezcmp[i]);
            }
            else if(i==0)
            {
                x_mask(X_ENABLE,rst.s_zupd?X_ENABLE:X_DISABLE,
                                rst.s_zcmp?X_ENABLE:X_DISABLE);
            }
            if(COM.texenable)
            {
                if(COM.dualtxt)
                {
                    txt_select2(COM.txt[0],COM.txt[1]);
                }
                else
                {
                    if(i==0 || COM.txt[0]!=COM.txt[i]) txt_select(COM.txt[i]);
                }
            }
            drawprims(i,i0,i1);
        }
    }
}

void clearprims(void)
{
    rst.vxtabi=0;
    rst.prtabi=0;
    // the flushed mark too: left at the old frame's count, the next flush
    // spanned a negative range and dropped the frame's first primitives
    rst.last_prtabi=0;
}

void wireprims(void)
{
    static int rcr,rcg,rcb;
    Primitive *pr;
    int prcnt,cnt;
    int j;

    if(!showinfo && !showwire) return;

    rcr+=77; rcr&=255;
    rcg+=37; rcg&=255;
    rcb+=97; rcb&=255;

    x_flush();

    debugdrawmode();

    x_reset();
    x_geometry(X_WIRE);
    x_mask(X_ENABLE,X_DISABLE,X_DISABLE);

    pr=rst.prtab;
    prcnt=rst.prtabi;
    cnt=0;

    while(prcnt-->0)
    {
        if(!pr->c[0])
        {
            float x0,y0,x1,y1;
            // viewport change
            x0=pr->c[2]->pos[0];
            y0=pr->c[2]->pos[1];
            x1=pr->c[2]->tex[0];
            y1=pr->c[2]->tex[1];
            if(showinfo)
            {
                x0+=320.0;
                x1+=320.0;
            }
            //print("prcnt %-5i: (%.0f,%.0f)-(%.0f,%.0f)\n",prcnt,x0,y0,x1,y1);
            x_viewport(x0,y0,x1,y1);
            pr++;
            continue;
        }
        x_begin(X_TRIANGLES);
        {
            cnt++;
            switch(pr->wirecolor)
            { // wirecolor
                case 1:
                    x_vxcolor(0.0,0.0,0.6);
                    break;
                case 2:
                    x_vxcolor(0.0,0.6,0.0);
                    break;
                case 3:
                    x_vxcolor(0.0,0.6,0.6);
                    break;
                case 4:
                    x_vxcolor(0.6,0.0,0.0);
                    break;
                case 5:
                    x_vxcolor(0.6,0.0,0.6);
                    break;
                case 6:
                    x_vxcolor(0.6,0.6,0.0);
                    break;
                case 7:
                    x_vxcolor(0.5,0.5,0.5);
                    break;
                case 8:
                    x_vxcolor(0.2,0.2,0.2);
                    break;
                case 9:
                    x_vxcolor(0.0,0.0,1.0);
                    break;
                case 10:
                    x_vxcolor(0.0,1.0,0.0);
                    break;
                case 11:
                    x_vxcolor(0.0,1.0,1.0);
                    break;
                case 12:
                    x_vxcolor(1.0,0.0,0.0);
                    break;
                case 13:
                    x_vxcolor(1.0,0.0,1.0);
                    break;
                case 14:
                    x_vxcolor(1.0,1.0,0.0);
                    break;
                case 15:
                    x_vxcolor(1.0,1.0,1.0);
                    break;
                default: // flash
                    x_vxcolor(rcr/256.0,rcg/256.0,rcb/256.0);
                    break;
            }
            for(j=0;j<3;j++)
            {
                x_vxposv((xt_pos *)pr->c[j]->pos);
            }
        }
        x_end();
        pr++;
    }

    x_flush();

    debugdrawmode();
}

void rdp_fillrect(TexRect *tr)
{
    Primitive *p,*p2;
    Vertex *v;

    viewport(1);
    if(rst.modechange) newmode();

    // LLE mode: size comes from the VI, unless it has no height yet
    if(rst.firstfillrect && !rst.rtt_on && (!st.lleos || !init.viewporthig))
    {
        if(tr->x0-tr->x1>250 && tr->y0-tr->y1>150)
        {
            int x=tr->x0+1;
            int y=tr->y0+1;
            int cw=rst.bufwid[RDP_BUF_C];
            if(x>295 && x<325)
            {
                x=320;
                y=240;
            }
            // wider than a standard color buffer: a generous clear, not the
            // screen (Chameleon Twist clears 338x248 into its 320 wide
            // buffer, and the size flipped with SETCIMG's every frame)
            if((cw==320 || cw==640) && x>cw) ;
            else if(x!=init.viewportwid || y!=init.viewporthig)
            {
                init.viewportwid=x;
                init.viewporthig=y;
                print("rdp: new screen resolution %ix%i\n",x,y);
                // the projection was set up for the old size at frame start
                rdp_viewport(init.gfxwid/2,init.gfxhig/2,init.gfxwid/2,init.gfxhig/2);
            }
        }
        rst.firstfillrect=0;
    }

    if(rst.s_cycles>2)
    {
        tr->x0+=1.0;
        tr->y0+=1.0;
    }

    if(rst.s_zsrc==1)
    {
        return;
        // prim color src, hexen uses to clear zbuf
        if(tr->x0-tr->x1>250 && tr->y0-tr->y1>150)
        {
            x_clear(0,1,0,0,0);
        }
        else return;
    }

    if(rst.bufbase[RDP_BUF_Z]==rst.bufbase[RDP_BUF_C])
    {
        // z-buffer clear: clear the depth under the scissor now, not only at
        // frame start. Smash Bros clears it for each intro panel, and the
        // stage's depth hid the character drawn after it.
        flushprims();
        x_clear(0,1,0,0,0);
        return;
    }
    if(rst.rawfillcolor==0xFFFCFFFC)
    {
        // z-buffer clear value into a color buffer
        return;
    }

    rst.fillrectcnt++;

    if(st.dumpgfx)
    {
        logd("\n+fillrect %i (%.1f,%.1f)-(%.1f,%.1f) screen (%.2f,%.2f)-(%.2f,%.2f) ",
            rst.fillrectcnt,
            tr->x1,tr->y1,
            tr->x0,tr->y0,
            tr->x1*oxm+oxa,tr->y1*oym+oya,
            tr->x0*oxm+oxa,tr->y0*oym+oya);
    }

//    if(rst.fillrectcnt!=3) return;

    // generate primitive
    p=newpr();
    p->wirecolor=15;

    // generate vertices
    v=p->c[0]=newvx();
    v->pos[0]=tr->x0*oxm+oxa;
    v->pos[1]=tr->y0*oym+oya;
    v->pos[2]=1.0;

    v=p->c[1]=newvx();
    v->pos[0]=tr->x0*oxm+oxa;
    v->pos[1]=tr->y1*oym+oya;
    v->pos[2]=1.0;

    v=p->c[2]=newvx();
    v->pos[0]=tr->x1*oxm+oxa;
    v->pos[1]=tr->y1*oym+oya;
    v->pos[2]=1.0;

    // generate second primitive
    p2=newpr();
    p2->wirecolor=15;
    p2->c[0]=p->c[0];
    p2->c[1]=p->c[2];

    v=p2->c[2]=newvx();
    v->pos[0]=tr->x1*oxm+oxa;
    v->pos[1]=tr->y0*oym+oya;
    v->pos[2]=1.0;

    // rectangles have no shade: SHADE reads 0 (fill mode uses C_FILL). The
    // newvx placeholder was magenta: Majora's Mask draws its letterbox edge
    // lines as 1-cycle fillrects with a SHADE combiner and alpha blending.
    {
        int i;
        for(i=0;i<3;i++) memset(p->c[i]->col,0,sizeof(p->c[i]->col));
        memset(p2->c[2]->col,0,sizeof(p2->c[2]->col));
    }

    st2.gfx_tris+=2;
}

void rdp_texrect(TexRect *tr)
{
    Primitive *p,*p2;
    Vertex *v;
    float u0,u1,v0,v1;

    // into the z-buffer: a depth mask, not a picture. Smash Bros draws the
    // round mask of its off-screen bubble there; on screen it was a black
    // square around the bubble.
    if(rst.bufbase[RDP_BUF_Z]==rst.bufbase[RDP_BUF_C]) return;

    rst.tris++;

    viewport(1);
    if(rst.modechange) newmode();

    // A wrapping tile repeats every 2^mask texels, the GL texture every
    // xs: when the mask is wider, a start past the first period samples
    // the wrong place. Big Mountain 2000 draws a 144 wide text strip with
    // mask 8 from s=256 (= 0): shown from texel 112, the text was rotated.
    // Brought into the first period when the rectangle stays inside it.
    {
        Tile *t=rst.tile+rst.texturetile;
        float span=fabs(tr->s1)*((tr->x0-tr->x1)>(tr->y0-tr->y1)?(tr->x0-tr->x1):(tr->y0-tr->y1))/32.0f;
        if(t->cms<2 && t->masks>0 && (1<<t->masks)>t->xs)
        {
            float period=(float)(32<<t->masks)*(t->cms==1?2:1);
            float s=tr->s0-period*floorf(tr->s0/period);
            if(s+span<=period) tr->s0=s;
        }
        span=fabs(tr->t1)*((tr->x0-tr->x1)>(tr->y0-tr->y1)?(tr->x0-tr->x1):(tr->y0-tr->y1))/32.0f;
        if(t->cmt<2 && t->maskt>0 && (1<<t->maskt)>t->ys)
        {
            float period=(float)(32<<t->maskt)*(t->cmt==1?2:1);
            float s=tr->t0-period*floorf(tr->t0/period);
            if(s+span<=period) tr->t0=s;
        }
    }

    if(rst.s_cycles>2)
    {
        u0=tr->s0;
        v0=tr->t0;
        u1=tr->s1*(tr->x0-tr->x1)/32.0;
        v1=tr->t1*(tr->y0-tr->y1)/32.0;
        if(rst.s_cycles>2) u1*=0.25;
        u1+=u0;
        v1+=v0;
        tr->x0+=1.0;
        tr->y0+=1.0;
    }
    else
    {
        // pixel i samples s0+i*dsdx (its top left); with txt_setscales'
        // half texel that is a GL texel centre when the edges carry
        // s0-dsdx/2 and s0+(w-1/2)*dsdx. Edges at s0 and s0+(w-1)*dsdx
        // squeezed w texels into w-1 pixels: Star Fox 64's title text,
        // 6-row strips, sampled between rows and each strip's last row
        // came out a blend of two (a step under the "Press START" dot).
        float ds=tr->s1/32.0f,dt=tr->t1/32.0f;
        u0=tr->s0-0.5f*ds;
        v0=tr->t0-0.5f*dt;
        u1=tr->s0+(tr->x0-tr->x1-0.5f)*ds;
        v1=tr->t0+(tr->y0-tr->y1-0.5f)*dt;
    }

    // less than a row tall: at least one. A 1-row rectangle is one
    // scanline as it is; stretched to 2, Star Fox 64's title doubled the
    // faint anti-aliased bottom row of "Press START" (13 rows drawn as
    // 6+6+1 strips) into a dark band.
    if(tr->y0-tr->y1<1.0f)
    {
        tr->y0++;
        if(rst.s_cycles<=2) v0=v1=tr->t0; // its one texel row, not a ramp
    }

    if(tr->flip)
    {
        float t;
        t=v1;
        v1=v0;
        v0=t;
    }

    // generate primitive
    p=newpr();
    p->wirecolor=15;

    v=p->c[0]=newvx();
    v->pos[0]=tr->x0*oxm+oxa;
    v->pos[1]=tr->y0*oym+oya;
    v->pos[2]=1.0;
    v->tex[0]=u1;
    v->tex[1]=v1;

    v=p->c[1]=newvx();
    v->pos[0]=tr->x0*oxm+oxa;
    v->pos[1]=tr->y1*oym+oya;
    v->pos[2]=1.0;
    v->tex[0]=u1;
    v->tex[1]=v0;

    v=p->c[2]=newvx();
    v->pos[0]=tr->x1*oxm+oxa;
    v->pos[1]=tr->y1*oym+oya;
    v->pos[2]=1.0;
    v->tex[0]=u0;
    v->tex[1]=v0;

    // generate second primitive
    p2=newpr();
    p2->wirecolor=15;
    p2->c[0]=p->c[0];
    p2->c[1]=p->c[2];

    v=p2->c[2]=newvx();
    v->pos[0]=tr->x1*oxm+oxa;
    v->pos[1]=tr->y0*oym+oya;
    v->pos[2]=1.0;
    v->tex[0]=u0;
    v->tex[1]=v1;

    st2.gfx_tris+=2;
}

// S2DEX sprite: a textured quad with corners ul,ur,lr,ll in screen pixels
// (any shape: G_OBJ_SPRITE rotates with the 2D matrix), s,t in 1/32 texel
// as the texrect's. Triangles in the texrect's order (lr,ur,ul),(lr,ul,ll).
void rdp_texquad(const float *x,const float *y,const float *s,const float *t)
{
    static const int order[6]={2,1,0,2,0,3};
    Vertex    *v[4];
    Primitive *p=NULL;
    int        i;

    rst.tris++;

    viewport(1);
    if(rst.modechange) newmode();

    for(i=0;i<4;i++)
    {
        v[i]=newvx();
        v[i]->pos[0]=x[i]*oxm+oxa;
        v[i]->pos[1]=y[i]*oym+oya;
        v[i]->pos[2]=1.0;
        v[i]->tex[0]=s[i];
        v[i]->tex[1]=t[i];
    }
    for(i=0;i<6;i++)
    {
        if(!(i%3))
        {
            p=newpr();
            p->wirecolor=15;
        }
        p->c[i%3]=v[order[i]];
    }
    st2.gfx_tris+=2;
}

// the display list's G_MV_VIEWPORT, kept for rtt_end
void rdp_gameviewport(float xm,float ym,float xa,float ya)
{
    rst.gamevp[0]=xm; rst.gamevp[1]=ym;
    rst.gamevp[2]=xa; rst.gamevp[3]=ya;
    rst.gamevpset=1;
    rdp_viewport(xm,ym,xa,ya);
}

void rdp_viewport(float xm,float ym,float xa,float ya)
{
    int xs=320;
    int ys=240;

    if(init.viewportwid)
    {
        xs=init.viewportwid;
        if(init.viewporthig) ys=init.viewporthig; // 0: not known yet (lle.c)
    }

    // screen pixels to GL: x maps to x*gfxwid/xs, as the 3D viewport and the
    // scissor do. Scaling by xs-1 put rectangle edges up to 2 pixels too far
    // right/down at 640x480: Majora's Mask's bottom letterbox bar started
    // below the scissored scene and left a grey line.
    oxm=+2.0/xs;
    oym=-2.0/ys;

    // prims queued so far belong to the old viewport
    if(rst.prtabi!=rst.last_prtabi) flushprims();

    xm*=(float)init.gfxwid/xs;
    ym*=(float)init.gfxhig/ys;
    xa*=(float)init.gfxwid/xs;
    ya*=(float)init.gfxhig/ys;
    rst.view_x0=xa-xm;
    rst.view_y0=ya-ym;
    rst.view_x1=xa+xm-1;
    rst.view_y1=ya+ym-1;
    // Not clamped to the screen: an off-screen viewport keeps its centre and
    // scale (glViewport takes it as is; Glide's clip window could not).
    // F-Zero X draws its 30 machine-select models with 320x240 viewports
    // centred on the grid cells.
    logd("\n!rdp_viewport (%i,%i)-(%i,%i)",
        rst.view_x0,rst.view_y0,
        rst.view_x1,rst.view_y1);
    rst.rectmode=-1; // viewport() skips when the draw mode is unchanged
    realdrawmode();
}

static void settexturetile(int tile)
{
    if(tile<6 && tile!=rst.nexttexturetile)
    {
        // only newmode() applies the tile: without a mode change, prims
        // drawn with the old and new tile were batched and all used one
        // (F-Zero X switches G_TEXTURE tiles 1-3 between machine parts)
        rst.nexttexturetile=tile;
        rst.modechange=rst.txtchange=1;
    }
}

void rdp_texture(int on,int tile,int level)
{
    if(tile<6) rst.tritile=tile;
    settexturetile(tile);
}

void rdp_fogrange(float min,float max)
{
    rst.fogmin=min;
    rst.fogmax=max;
    if(rst.fogmin==0.0 && rst.fogmax==0.0) rst.fogenable=0;
    else rst.fogenable=1;
    rst.foglasttype=-1;
}

void fillcbuf(void)
{
    int   xs,ys,rl;
    dword addr;

    addr=rst.bufbase[RDP_BUF_C];
    rl  =rst.bufwid [RDP_BUF_C]*2;
    xs  =rst.view_x1-rst.view_x0;
    ys  =rst.view_y1-rst.view_y0;
    addr+=rst.view_x0*2;
    addr+=rst.view_y0*2*rl;

    print("FillCBUF %08X: rl=%i (%i,%i)-(%i,%i) sz(%i,%i)\n",
        addr,rl,
        rst.view_x0,rst.view_y0,
        rst.view_x1,rst.view_y1,
        xs,ys);
}

void rdp_tri(int *vind)
{
    Primitive *p;
    int i;

    rst.tris++;

    settexturetile(rst.tritile); // back from a texture rectangle's own tile
    viewport(0);
    if(rst.modechange) newmode();

    if(cart.dlist_diddlyvx && rst.vxtabi+3>MAXVX)
    {
        error("rdp: triangle buffer full");
        return;
    }

    p=newpr();

    if(rst.vxtabi>MAXVX || rst.prtabi>MAXPR)
    {
        exception("dlist: vx/pr-tab corrupted!\n");
        return;
    }

    for(i=0;i<3;i++)
    {
        if(cart.dlist_diddlyvx)
        {
            // DKR assigns UVs per triangle, reusing the vertex indices.
            // Preserve them until this deferred primitive is rendered.
            p->c[i]=newvx();
            *p->c[i]=*rdpvx[vind[i]];
        }
        else p->c[i]=rdpvx[vind[i]];
    }
    p->wirecolor=rst.debugwirecolor;

    st2.gfx_tris++;
}

// assign new vertices to rdp vertices first..(first+num-1)
void rdp_newvtx(int first,int num)
{
    int i;
    if(st.dumpgfx)
    {
        logd("\nrdp_newvtx(base=%i,first=%i,num=%i) ",rst.vxtabi,first,num);
    }
    if(num+rst.vxtabi>=MAXVX)
    {
        error("rdp: loadvx too many vertices in frame (max %i)",MAXVX);
        flushprims(); // draw what uses the old vertices before reusing them
        rst.vxtabi=0;
    }

    num+=first;
    for(i=first;i<num;i++)
    {
        rdpvx[i]=rst.vxtab+rst.vxtabi;
        rdpvx[i]->zs=-1.0f; // depth from w (the slot may hold a raw-RDP z)
        rst.vxtabi++;
    }
}

// moves rdp vertex i to a fresh slot holding a copy, so a change to it
// (G_MODIFYVTX) doesn't reach triangles already queued with the old slot
void rdp_dupvtx(int i)
{
    Vertex *old=rdpvx[i];
    if(rst.vxtabi+1>=MAXVX)
    {
        flushprims(); // draw what uses the old vertices before reusing them
        rst.vxtabi=0;
    }
    rdpvx[i]=rst.vxtab+rst.vxtabi++;
    *rdpvx[i]=*old;
}

/****************************************************************************
/* Rendering State conversion - Combine
*/

int combmap[32][8]={ // X,Y,M,A, AlphaXYA, AlphaM
C_COMB  ,C_COMB  ,C_COMB  ,C_COMB  ,C_COMB  ,C_LODF  ,0,0,
C_TEX0  ,C_TEX0  ,C_TEX0  ,C_TEX0  ,C_TEX0  ,C_TEX0  ,0,0,
C_TEX1  ,C_TEX1  ,C_TEX1  ,C_TEX1  ,C_TEX1  ,C_TEX1  ,0,0,
C_PRIM  ,C_PRIM  ,C_PRIM  ,C_PRIM  ,C_PRIM  ,C_PRIM  ,0,0,
C_SHADE ,C_SHADE ,C_SHADE ,C_SHADE ,C_SHADE ,C_SHADE ,0,0,
C_ENV   ,C_ENV   ,C_ENV   ,C_ENV   ,C_ENV   ,C_ENV   ,0,0,
C_ONE   ,C_CENTER,C_SCALE ,C_DUNNO ,C_ONE   ,C_PLODF ,0,0,
C_NOISE ,C_K4    ,C_COMBA ,C_ZERO  ,C_ZERO  ,C_ZERO  ,0,0,
//
C_TEX0A ,C_TEX0A ,C_TEX0A ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0,
C_TEX1A ,C_TEX1A ,C_TEX1A ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0,
C_PRIA  ,C_PRIA  ,C_PRIA  ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0,
C_SHAA  ,C_SHAA  ,C_SHAA  ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0,
C_ENVA  ,C_ENVA  ,C_ENVA  ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0,
C_LODF  ,C_LODF  ,C_LODF  ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0,
C_PLODF ,C_PLODF ,C_PLODF ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0,
C_ZERO  ,C_ZERO  ,C_K5    ,C_DUNNO ,C_DUNNO ,C_DUNNO ,0,0};

int combtab[16][8]={ // x0/1,pos,num,map,c2,c1,-,-
0,20,0x0f,0,RDP_X,0,0,0,  //saC0
1,28,0x0f,1,RDP_Y,0,0,0,  //sbC0
0,15,0x1f,2,RDP_M,0,0,0,  // mC0
1,15,0x07,3,RDP_A,0,0,0,  // aC0
0,12,0x07,4,RDP_X,1,0,0,  //saA0
1,12,0x07,4,RDP_Y,1,0,0,  //sbA0
0, 9,0x07,5,RDP_M,1,0,0,  // mA0
1, 9,0x07,4,RDP_A,1,0,0,  // aA0
0, 5,0x0f,0,RDP_X,2,0,0,  //saC1
1,24,0x0f,1,RDP_Y,2,0,0,  //sbC1
0, 0,0x1f,2,RDP_M,2,0,0,  // mC1
1, 6,0x07,3,RDP_A,2,0,0,  // aC1
1,21,0x07,4,RDP_X,3,0,0,  //saA1
1, 3,0x07,4,RDP_Y,3,0,0,  //sbA1
1,18,0x07,5,RDP_M,3,0,0,  // mA1
1, 0,0x07,4,RDP_A,3,0,0}; // aA1

static Combine comfill;
static Combine comcopy;
static int     dump; // for com routines

/****************************************************************************
** The N64 color combiner, evaluated as is by the shader (x_n64combine):
** (A-B)*C+D per cycle for color and alpha, with GLideN64's input rules. It
** replaced the pattern compiler below (com_new), which mapped modes onto
** Voodoo texture/color units and could not take color from one texture and
** alpha from the other, or sum two textures (Pokemon Stadium's battle
** buttons and floor). "combiner 0" brings the old compiler back.
*/
// x_n64combine input codes from the combine word's selectors
static const signed char n64_ca[16]={0,1,2,3,4,5,6,X_N64_NOISE,7,7,7,7,7,7,7,7};
static const signed char n64_cb[16]={0,1,2,3,4,5,7,X_N64_K4,7,7,7,7,7,7,7,7};  // CENTER: 0
static const signed char n64_cc[32]={0,1,2,3,4,5,7,8,9,10,11,12,13,X_N64_LODF,X_N64_PRIMLODF,X_N64_K5,
                                     7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7}; // SCALE: 0
static const signed char n64_cd[8] ={0,1,2,3,4,5,6,7};
static const signed char n64_aa[8] ={0,1,2,3,4,5,6,7};                 // also B, D
static const signed char n64_ac[8] ={X_N64_LODF,1,2,3,4,5,X_N64_PRIMLODF,7};

void rdp_forcemodechange(void)
{
    rst.modechange=2;
}

static int n64_fix(int v,int cycle,int cycles)
{
    if(cycles==1)
    {   // 1-cycle: no combined value yet, TEXEL1 is TEXEL0
        if(v==0 || v==8) return 7;
        if(v==2) return 1;
        if(v==10) return 9;
    }
    else if(cycle==0)
    {   // 2-cycle, first cycle: combined isn't there yet
        if(v==0 || v==8) return X_N64_HALF;
    }
    else
    {   // second cycle: the RDP feeds TEXEL1 as TEXEL0 and the next
        // pixel's TEXEL0 as TEXEL1 (_correctSecondStageParams)
        if(v==1) return 2;
        if(v==2) return 1;
        if(v==9) return 10;
        if(v==10) return 9;
    }
    return v;
}

static void n64_combine(dword x0,dword x1)
{
    int raw[2][8],c,i,texuse=0;

    // color A,B,C,D then alpha A,B,C,D of each cycle (SETCOMBINE fields)
    raw[0][0]=n64_ca[(x0>>20)&15]; raw[1][0]=n64_ca[(x0>> 5)&15];
    raw[0][1]=n64_cb[(x1>>28)&15]; raw[1][1]=n64_cb[(x1>>24)&15];
    raw[0][2]=n64_cc[(x0>>15)&31]; raw[1][2]=n64_cc[(x0    )&31];
    raw[0][3]=n64_cd[(x1>>15)& 7]; raw[1][3]=n64_cd[(x1>> 6)& 7];
    raw[0][4]=n64_aa[(x0>>12)& 7]; raw[1][4]=n64_aa[(x1>>21)& 7];
    raw[0][5]=n64_aa[(x1>>12)& 7]; raw[1][5]=n64_aa[(x1>> 3)& 7];
    raw[0][6]=n64_ac[(x0>> 9)& 7]; raw[1][6]=n64_ac[(x1>>18)& 7];
    raw[0][7]=n64_aa[(x1>> 9)& 7]; raw[1][7]=n64_aa[(x1    )& 7];

    if(rst.s_cycles==1)
    {   // 1-cycle mode runs the second cycle's equation
        n64cycles=1;
        for(i=0;i<4;i++)
        {
            n64cc[0][i]=n64_fix(raw[1][i],0,1);
            n64ac[0][i]=n64_fix(raw[1][4+i],0,1);
        }
    }
    else
    {   // equal cycles are one stage, as in GLideN64 (Combiner_GetStages)
        n64cycles=memcmp(raw[0],raw[1],sizeof(raw[0]))?2:1;
        for(c=0;c<n64cycles;c++)
            for(i=0;i<4;i++)
            {
                n64cc[c][i]=n64_fix(raw[c][i],c,2);
                n64ac[c][i]=n64_fix(raw[c][4+i],c,2);
            }
    }

    for(c=0;c<n64cycles;c++)
        for(i=0;i<4;i++)
        {
            if(n64cc[c][i]==1 || n64cc[c][i]==9 || n64ac[c][i]==1) texuse|=1;
            if(n64cc[c][i]==2 || n64cc[c][i]==10 || n64ac[c][i]==2) texuse|=2;
        }
    comn64.texenable=texuse;
    comn64.passes=1;

    if(dump)
    {
        logd("\n+mode n64comb: cycles %i",n64cycles);
        for(c=0;c<n64cycles;c++)
            logd(" | c%i rgb (%i-%i)*%i+%i a (%i-%i)*%i+%i",c,
                n64cc[c][0],n64cc[c][1],n64cc[c][2],n64cc[c][3],
                n64ac[c][0],n64ac[c][1],n64ac[c][2],n64ac[c][3]);
        logd(" tex %i",texuse);
    }
}

char *dump_combinenm(char *buf0,int c)
{
    char *buf=buf0;
    *buf=0;
    if(c&C_MULCASE)
    {
        *buf++='(';
        strcpy(buf,cname[c&C_INDEX]); buf+=strlen(buf);
        *buf++='*';
        strcpy(buf,cname[(c>>8)&C_INDEX]); buf+=strlen(buf);
        *buf++=')';
    }
    else
    {
        strcpy(buf,cname[c&C_INDEX]); buf+=strlen(buf);
    }

    if(c&C_SPECIALCASE)
    {
        *buf++='!';
        *buf++='S';
    }
    *buf++=0;

    return(buf0);
}

char *dump_combineeq(char *buf0,int *cx,int style,int recurse)
{
    char *x,*y,*m,*a;
    char *buf=buf0;
    int   ri;
    static char tmp[16][80];

    if(recurse>0) ri=1; else ri=0;

    if(recurse>0 && cx[RDP_X]==C_COMB) x=dump_combineeq(tmp[ri+ 0],cx-8,STYLE_IGNORE,-1);
    else                               x=dump_combinenm(tmp[ri+ 2],cx[RDP_X]);
    if(recurse>0 && cx[RDP_Y]==C_COMB) y=dump_combineeq(tmp[ri+ 4],cx-8,STYLE_IGNORE,-1);
    else                               y=dump_combinenm(tmp[ri+ 6],cx[RDP_Y]);
    if(recurse>0 && cx[RDP_M]==C_COMB) m=dump_combineeq(tmp[ri+ 8],cx-8,STYLE_IGNORE,-1);
    else                               m=dump_combinenm(tmp[ri+10],cx[RDP_M]);
    if(recurse>0 && cx[RDP_A]==C_COMB) a=dump_combineeq(tmp[ri+12],cx-8,STYLE_IGNORE,-1);
    else                               a=dump_combinenm(tmp[ri+14],cx[RDP_A]);

    if(recurse==-1)
    {
        *buf++='[';
        *buf++=' ';
    }

    if(style==STYLE_IGNORE)
    {
        if(*x=='0' && *y=='0' && *m=='0') style=STYLE_CONST;
        else if(*y=='0' && *a=='0') style=STYLE_MUL;
        else if(*y=='0') style=STYLE_MULADD;
        else if(cx[RDP_Y]==cx[RDP_A]) style=STYLE_BLEND;
        else style=STYLE_FULL;
    }

    switch(style)
    {
    case STYLE_COMB:
    case STYLE_CONST:
        sprintf(buf,"%s",a);
        break;
    case STYLE_MUL:
        sprintf(buf,"%s*%s",x,m);
        break;
    case STYLE_MULADD:
        sprintf(buf,"%s*%s+%s",x,m,a);
        break;
    case STYLE_BLEND:
        sprintf(buf,"%s->%s,%s",y,x,m);
        break;
    default:
        strcpy(buf,"??:");
        buf+=3;
    case STYLE_FULL:
        sprintf(buf,"(%s-%s)*%s+%s",x,y,m,a);
        break;
    }

    if(recurse==-1)
    {
        buf+=strlen(buf);
        *buf++=' ';
        *buf++=']';
        *buf++=0;
    }

    return(buf0);
}

void dumpscombine(char *txt)
{
    int alp=0,alp0,alp1;
    char buf1[80];
    if(*txt=='*')
    {
        alp0=0;
        alp1=1;
        txt++;
    }
    else if(*txt=='!')
    {
        alp0=1;
        alp1=1;
        txt++;
    }
    else
    {
        alp0=0;
        alp1=0;
    }
    for(alp=alp0;alp<=alp1;alp++)
    {
        dump_combineeq(buf1,rst.s_combx[2+alp],rst.s_combinestyle[2+alp],1);
        logd("\n%s%c: %-38s",txt,alp?'A':'C',buf1);
        logd( "cs0=%-6s",stylename[rst.s_combinestyle[alp+0]]);
        logd(" cs1=%-6s",stylename[rst.s_combinestyle[alp+2]]);
        logd(" tx0=%i",rst.s_combinetex[alp+0]);
        logd(" tx1=%i",rst.s_combinetex[alp+2]);
        logd(" cyc=%i",rst.s_combinecycles);
    }
}

void dump_combineresult(void)
{
    int i;
    logd("\n+mode combine-^^^: texenable=%i ",COM.texenable);
    if(COM.passes>1 && COM.col[0][1]!=C_ONE)
    {
        logd(" Transparent-Multipass! ");
    }
    logd("ENV=%s ",colortext(rst.col[C_ENV]));
    logd("PRIA=%s ",colortext(rst.col[C_PRIA]));
    logd("PRIM=%s ",colortext(rst.col[C_PRIM]));
    logd("PLODF=%02X ",rst.col[C_PLODF][3]);
    logd("passes=%i: ",COM.passes);
    for(i=0;i<COM.passes;i++)
    {
        logd("\n+mode combine-^^%i: ",i);
        logd("Col:%05X/%05X,Com:%04X/%04X,Bl:%04X/%04X,Env:%04X,T%i ",
            COM.col[i][0],COM.col[i][1],
            COM.com[i][0],COM.com[i][1],
            COM.blend1[i],COM.blend2[i],
            COM.env,
            COM.txt[i]);
    }
}

void dump_combineeqs(int alp)
{
    char buf1[100];
    char buf2[100];
    dump_combineeq(buf1,rst.s_combx[0+alp],STYLE_FULL,0);
    dump_combineeq(buf2,rst.s_combx[2+alp],STYLE_FULL,0);
    logd("[ Cycle1: %s Cycle2: %s ]",buf1,buf2);
}

void dump_combine(dword x0,dword x1)
{
    int  cn=rst.s_cycles;
    char buf1[100];
    char buf2[100];

    if(cn==3)
    {
        logd("\n+mode combine-all: COPY");
        return;
    }
    if(cn==4)
    {
        logd("\n+mode combine-all: FILL");
        return;
    }

    if(0)
    {
        logd("\n+mode combine-col: ");
        dump_combineeq(buf1,rst.s_combx[0],STYLE_IGNORE,0);
        dump_combineeq(buf2,rst.s_combx[2],STYLE_IGNORE,0);
        if(cn==2) logd("%-38s Cycle2: %s",buf1,buf2);
        else      logd("%s",buf1);

        logd("\n+mode combine-alp: ");
        dump_combineeq(buf1,rst.s_combx[1],STYLE_IGNORE,0);
        dump_combineeq(buf2,rst.s_combx[3],STYLE_IGNORE,0);
        if(cn==2) logd("%-38s Cycle2: %s",buf1,buf2);
        else      logd("%s",buf1);
    }
    else
    {
        int alp;
        for(alp=0;alp<=1;alp++)
        {
            dump_combineeq(buf1,rst.s_combx[2+alp],rst.s_combinestyle[2+alp],1);
            logd("\n+mode combine-%s: %-38s",alp?"alp":"col",buf1);
            dump_combineeq(buf1,rst.s_combxbak[0+alp],0,0);
            dump_combineeq(buf2,rst.s_combxbak[2+alp],0,0);
            if(cn==2)  logd("<= Cycle1: %s Cycle2: %s",buf1,buf2);
            else       logd("<= Cycle1: %s",buf1);
        }
        dump_combineresult();
    }
}

int mode_findcache(dword x0,dword x1,dword o0,dword o1)
{
    int ci;
    for(ci=0;ci<rst.combcacheused;ci++)
    {
        if(rst.combcache[ci].combine0==x0 &&
           rst.combcache[ci].combine1==x1 &&
           rst.combcache[ci].other0  ==o0 &&
           rst.combcache[ci].other1  ==o1)
        {
            rst.s_c=&rst.combcache[ci];
            return(ci);
        }
    }
    return(-1);
}

int mode_newcache(dword x0,dword x1,dword o0,dword o1)
{
    int ci,a;

    ci=rst.combcacheused++;
    if(ci>=MAXCOMB) a=0;
    rst.combcacheused=ci+1;

    rst.s_c=&rst.combcache[ci];
    memset(rst.s_c,0,sizeof(Combine));
    COM.combine0=x0;
    COM.combine1=x1;
    COM.other0  =o0;
    COM.other1  =o1;

    return(ci);
}

// set combx[i] to passthrough
void com_setcomb(int i)
{
    rst.s_combinestyle[i]=STYLE_COMB;
    rst.s_combx[i][RDP_X]=0;
    rst.s_combx[i][RDP_Y]=0;
    rst.s_combx[i][RDP_M]=0;
    rst.s_combx[i][RDP_A]=C_COMB;
}

// set combx[i] to passthrough
void com_setzero(int i)
{
    rst.s_combinestyle[i]=STYLE_CONST;
    rst.s_combx[i][RDP_X]=0;
    rst.s_combx[i][RDP_Y]=0;
    rst.s_combx[i][RDP_M]=0;
    rst.s_combx[i][RDP_A]=0;
}

// set combx[i] to passthrough
void com_setcopy(int d,int s)
{
    rst.s_combinestyle[d]=rst.s_combinestyle[s];
    rst.s_combx[d][RDP_X]=rst.s_combx[s][RDP_X];
    rst.s_combx[d][RDP_Y]=rst.s_combx[s][RDP_Y];
    rst.s_combx[d][RDP_M]=rst.s_combx[s][RDP_M];
    rst.s_combx[d][RDP_A]=rst.s_combx[s][RDP_A];
}

// convert x0,x1 => rst.combine[]/combinebak[] array
void com_convertcombine(dword x0,dword x1)
{
    int c,i,i1,i2,a,v,x,pos,msk,map,c1,c2;
    for(c=0;c<2;c++)
    {
        i1=8*c;
        i2=i1+8;
        for(i=i1;i<i2;i++)
        {
            if (combtab[i][0]) x=x1; else x=x0;
            pos=combtab[i][1];
            msk=combtab[i][2];
            map=combtab[i][3];
            c2 =combtab[i][4];
            c1 =combtab[i][5];

            a=( x>>pos ) & msk;
            if(a>15) v=C_ZERO;
            else     v=combmap[a][map];

            rst.s_combx[c1][c2]=v;
            rst.s_combxbak[c1][c2]=v;
        }
    }
    rst.s_combinecycles=rst.s_cycles;

    // In the second cycle of 2-cycle mode the RDP feeds TEXEL1 as "TEXEL0"
    // and the next pixel's TEXEL0 as "TEXEL1" (GLideN64
    // _correctSecondStageParams). Pokemon Stadium's menu text multiplies a
    // tile-1 gradient (cycle 1) by the tile-0 glyphs (TEXEL1 in cycle 2).
    if(rst.s_cycles==2)
    {
        for(c1=2;c1<4;c1++) for(i=0;i<4;i++)
        {
            v=rst.s_combx[c1][i];
            if     (v==C_TEX0 ) v=C_TEX1;
            else if(v==C_TEX1 ) v=C_TEX0;
            else if(v==C_TEX0A) v=C_TEX1A;
            else if(v==C_TEX1A) v=C_TEX0A;
            rst.s_combx[c1][i]=v;
            rst.s_combxbak[c1][i]=v;
        }
    }
}

// calculate rst.combinestyle,rst.combinetex,rst.combinetexboth
void com_calculatestyles(void)
{
    int i,j,a,s;
    // determine styles
    for(i=0;i<rst.s_combinecycles*2;i++)
    {
        if(rst.s_combx[i][RDP_Y]!=C_ZERO)
        {
            if(rst.s_combx[i][RDP_Y]!=rst.s_combx[i][RDP_A])
                 s=STYLE_FULL;
            else s=STYLE_BLEND;
        }
        else
        {
            if(rst.s_combx[i][RDP_A]!=C_ZERO)
            {
                if(rst.s_combx[i][RDP_X]==C_ZERO &&
                   rst.s_combx[i][RDP_M]==C_ZERO )
                {
                    if(rst.s_combx[i][RDP_A]==C_COMB)
                         s=STYLE_COMB;
                    else s=STYLE_CONST;
                }
                else
                {
                    if(rst.s_combx[i][RDP_M]==C_ONE) s=STYLE_ADD;
                    else s=STYLE_MULADD;
                }
            }
            else if(rst.s_combx[i][RDP_X]==C_ZERO && rst.s_combx[i][RDP_M]==C_ZERO)
            {
                s=STYLE_CONST;
            }
            else
            {
                s=STYLE_MUL;
            }
        }
        rst.s_combinestyle[i]=s;
    }
    // clear first cycle if no C_COMB in second cycle
    if(rst.s_combinecycles==2)
    {
        for(i=2;i<4;i++)
        {
            for(j=0;j<4;j++)
            {
                if(rst.s_combx[i][j]==C_COMB) break;
            }
            if(j==4)
            {
                if(dump) logd(" #setzero%i ",i-2);
                com_setzero(i-2);
            }
            if(rst.s_combx[i][RDP_Y]==C_COMB) break;
            if(rst.s_combx[i][RDP_M]==C_COMB) break;
            if(rst.s_combx[i][RDP_A]==C_COMB) break;
        }
    }
    // determine texture presense
    rst.s_combinetexboth=0;
    for(i=0;i<4;i++)
    {
        s=0;
        for(j=0;j<4;j++)
        {
            a=rst.s_combx[i][j];
            if(a==C_TEX0 || a==C_TEX0A) s|=1;
            if(a==C_TEX1 || a==C_TEX1A) s|=2;
        }
        rst.s_combinetex[i]=s;
        rst.s_combinetexboth|=s;
    }
}

// reorder noncolors to X parameter in muls/adds
// also remove trivial *1
void com_reorder(void)
{
    int i,s,sd;
    for(i=0;i<rst.s_combinecycles*2;i++)
    {
        s=rst.s_combinestyle[i];
        if(s==STYLE_MUL || s==STYLE_MULADD)
        { // swap texture to X in mul case
            if(rst.s_combx[i][RDP_X]<C_TEX0)
            {
                SWAP(rst.s_combx[i][RDP_X],rst.s_combx[i][RDP_M]);
            }
            // check for *1, remove it
            if(rst.s_combx[i][RDP_M]==C_ONE && s==STYLE_MUL)
            {
                rst.s_combinestyle[i]=STYLE_CONST;
                rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_X];
                rst.s_combx[i][RDP_X]=C_ZERO;
                rst.s_combx[i][RDP_Y]=C_ZERO;
                rst.s_combx[i][RDP_M]=C_ZERO;
            }
        }
        else if(s==STYLE_ADD)
        { // swap texture to X in add case
            if(rst.s_combx[i][RDP_X]<C_TEX0)
            {
                SWAP(rst.s_combx[i][RDP_X],rst.s_combx[i][RDP_A]);
            }
        }
    }
}

// convert PRIM to PRIM*SHADE (flatmode)
void com_flat(void)
{
    int i,j;
    for(i=0;i<rst.s_combinecycles*2;i++)
    {
        for(j=0;j<4;j++)
        {
            if(rst.s_combx[i][j]==C_PRIM)
            {
                rst.s_combx[i][j]=C_MUL(C_SHADE,C_PRIM);
            }
        }
    }
}

// remove all states with (X-Y)*LOD+A, convert to A
void com_removelod(void)
{
    int i;
    for(i=0;i<rst.s_combinecycles*2;i++)
    {
        /* // test to remove a zelda specific case
        if((rst.s_combx[i][RDP_M]==C_SHADE ||
           rst.s_combx[i][RDP_M]==C_PRIM) && i>=2)
        {
            com_setcomb(i);
            continue;
        }
        */
        // blend(TEX,TEX,lod)
        if(rst.s_combx[i][RDP_M]==C_LODF || rst.s_combx[i][RDP_M]==C_PLODF)
        {
            rst.s_combinestyle[i]=STYLE_CONST;
            if(rst.s_combx[i][RDP_X]==C_TEX0) rst.s_combx[i][RDP_A]=C_TEX0;
            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            if(dump) logd(" #LodRemove%i ",i);
            continue;
        }
        if(rst.s_combinestyle[i]==STYLE_ADD &&
           rst.s_combx[i][RDP_X]==C_PLODF &&
           rst.s_combx[i][RDP_M]==C_ONE)
        {
            rst.s_combinestyle[i]=STYLE_CONST;
            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            if(dump) logd(" #Lod*1-Remove%i ",i);
            continue;
        }
        if(rst.s_combinestyle[i]==STYLE_MULADD &&
           rst.s_combx[i][RDP_X]==C_PLODF &&
           rst.s_combx[i][RDP_M]==C_ONE)
        {
            rst.s_combinestyle[i]=STYLE_CONST;
            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            if(dump) logd(" #MulAddPLODF%i ",i);
            continue;
        }
    }
}

static int com_isconst(int c)
{
    return(c==C_PRIM || c==C_ENV || c==C_PRIA || c==C_ENVA);
}

static int com_istex(int c)
{
    return(c==C_TEX0 || c==C_TEX1 || c==C_TEX0A || c==C_TEX1A);
}

// X*M+A has no mode: keep one term. Normally the product, but when A is a
// texture and the product is scaled by a constant, the texture: it holds
// the shape, the product a tint on it. Castlevania's menu text has alpha
// ENV*TEX1+TEX0 with ENV alpha 2: the product alone made the text vanish.
void com_removemuladd(void)
{
    int i;
    for(i=0;i<rst.s_combinecycles*2;i++)
    {
        if(rst.s_combinestyle[i]==STYLE_MULADD)
        {
            int x=rst.s_combx[i][RDP_X],m=rst.s_combx[i][RDP_M];
            if(com_istex(rst.s_combx[i][RDP_A]) && (com_isconst(x) || com_isconst(m)))
            {
                rst.s_combinestyle[i]=STYLE_CONST;
                rst.s_combx[i][RDP_X]=C_ZERO;
                rst.s_combx[i][RDP_M]=C_ZERO;
                if(dump) logd(" #MulAddKeepAdd ",i);
                continue;
            }
            rst.s_combinestyle[i]=STYLE_MUL;
            rst.s_combx[i][RDP_A]=0;
            if(dump) logd(" #MulAddRemove ",i);
        }
    }
}

// remove first cycle [tex0->tex1,const] if dual cycle mode
void com_removetextex2(void)
{
    int i,a;
    if(rst.s_combinecycles<2) return;
    for(i=0;i<2;i++)
    {
        if(rst.s_combinestyle[i]==STYLE_BLEND &&
           rst.s_combinestyle[i+2]!=STYLE_CONST &&
//           rst.s_combinestyle[i+2]!=STYLE_MUL &&
           !ISCOLOR(rst.s_combx[i][RDP_X]) &&
           !ISCOLOR(rst.s_combx[i][RDP_Y]) &&
            ISCOLOR(rst.s_combx[i][RDP_M]) &&
            rst.s_combx[i][RDP_M]!=C_SHADE &&
            rst.s_combx[i][RDP_M]!=C_PRIM )
        {
            // Tex->Tex,Color - color not SHADE or PRIM
            if(i==1) a=4*rst.col[rst.s_combx[i][RDP_M]][3];
            else
            {
                a=rst.col[rst.s_combx[i][RDP_M]][0]
                 +rst.col[rst.s_combx[i][RDP_M]][1]
                 +rst.col[rst.s_combx[i][RDP_M]][2]
                 +rst.col[rst.s_combx[i][RDP_M]][3];
            }
            /*
            if(rst.s_combx[i][RDP_X]==C_TEX0 ||
               rst.s_combx[i][RDP_Y]==C_TEX0)
            {
                rst.s_combx[i][RDP_A]=C_TEX0;
            }
            else
            */
            if(a<0x10*4)
            {
                rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_Y];
                rst.s_combx[i][RDP_X]=0;
                rst.s_combx[i][RDP_Y]=0;
                rst.s_combx[i][RDP_M]=0;
                rst.s_combinestyle[i]=STYLE_CONST;
                if(dump) logd(" #Simplify[TexTex]%i ",i);
            }
            else if(a>0xf0*4)
            {
                rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_X];
                rst.s_combx[i][RDP_X]=0;
                rst.s_combx[i][RDP_Y]=0;
                rst.s_combx[i][RDP_M]=0;
                rst.s_combinestyle[i]=STYLE_CONST;
                if(dump) logd(" #Simplify[TexTex]%i ",i);
            }
        }
        if(rst.s_combinestyle[i]==STYLE_FULL &&
           ( rst.s_combx[i][RDP_A]==C_TEX0 ||
             rst.s_combx[i][RDP_A]==C_TEX1 ||
             rst.s_combx[i][RDP_A]==C_TEX0A ||
             rst.s_combx[i][RDP_A]==C_TEX1A ) )
        {
            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            rst.s_combinestyle[i]=STYLE_CONST;

            if(dump) logd(" #Simplify[Full]%i ",i);
        }
    }
}

int avgcolor(int c,int i)
{
    int a;
    if(i&1) a=rst.col[c][3];
    else
    {
        a=rst.col[c][0]
         +rst.col[c][1]
         +rst.col[c][2]
         +rst.col[c][3];
        a>>=2;
    }
    return(a);
}

// remove blend where blender is PRIM/ENV and it's fully to one way
// remove also multiplys where multiplier is 1 or 0
void com_removefullblend(void)
{
    int i,a;
    if(rst.s_cycles==2) for(i=0;i<4;i++)
    {
        if(rst.s_combinestyle[i]==STYLE_MUL &&
           ISCOLOR(rst.s_combx[i][RDP_M]) &&
           rst.s_combx[i][RDP_M]!=C_SHADE &&
           rst.s_combx[i][RDP_M]!=C_SHAA &&
           rst.s_combx[i][RDP_M]!=C_PRIM )   // prim often changes
        {
            a=avgcolor(rst.s_combx[i][RDP_M],i&1);
            if(a==255)
            {
                rst.s_combinestyle[i]=STYLE_CONST;
                rst.s_combx[i][RDP_M]=0;
                rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_X];
                rst.s_combx[i][RDP_X]=0;
                rst.s_combx[i][RDP_Y]=0;
                if(dump) logd(" #Simplify[*FF]%i ",i);
            }
            else if(a==0)
            {
                rst.s_combinestyle[i]=STYLE_CONST;
                rst.s_combx[i][RDP_M]=0;
                rst.s_combx[i][RDP_A]=C_ZERO;
                rst.s_combx[i][RDP_X]=0;
                rst.s_combx[i][RDP_Y]=0;
                if(dump) logd(" #Simplify[*00]%i ",i);
            }
        }
    }
    for(i=0;i<4;i++)
    {
        if(rst.s_combinestyle[i]==STYLE_BLEND &&
           (!ISCOLOR(rst.s_combx[i][RDP_X]) ||
            !ISCOLOR(rst.s_combx[i][RDP_Y]) ) &&
            ISCOLOR(rst.s_combx[i][RDP_M]) &&
            rst.s_combx[i][RDP_M]!=C_SHADE &&
            rst.s_combx[i][RDP_M]!=C_SHAA )
        {
            a=avgcolor(rst.s_combx[i][RDP_M],i&1);
            if(a==0)
            {
                rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_Y];
                if(dump) logd(" #Simplify[<-100%%]%i ",i);
            }
            else if(a==255)
            {
                rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_X];
                if(dump) logd(" #Simplify[->100%%]%i ",i);
            }
            else continue;

            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            rst.s_combinestyle[i]=STYLE_CONST;
        }
    }
}

// combine second cycle *COLOR to first cycle colors if possible (set second to passthrough)
void com_combinemul2(void)
{
    int i;
    if(rst.s_combinecycles<2) return;
    for(i=0;i<2;i++)
    {
        if(rst.s_combinestyle[i+2]==STYLE_MUL &&
           rst.s_combx[i+2][RDP_X]==C_COMB &&
           ISCOLOR(rst.s_combx[i+2][RDP_M]) )
        { // second cycle is *COLOR
            if(rst.s_combinestyle[i]==STYLE_MUL &&
               ISCOLOR(rst.s_combx[i][RDP_X]) )
            { // first is COLOR*?
                rst.s_combx[i][RDP_X]=C_MUL(rst.s_combx[i][RDP_X],
                                            rst.s_combx[i+2][RDP_M]);
            }
            else if(rst.s_combinestyle[i]==STYLE_MUL &&
               ISCOLOR(rst.s_combx[i][RDP_M]) )
            { // first is ?*COLOR
                rst.s_combx[i][RDP_M]=C_MUL(rst.s_combx[i][RDP_M],
                                            rst.s_combx[i+2][RDP_M]);
            }
            else if(rst.s_combinestyle[i]==STYLE_BLEND &&
               ISCOLOR(rst.s_combx[i][RDP_X]) &&
               ISCOLOR(rst.s_combx[i][RDP_Y]) &&
               !ISCOLOR(rst.s_combx[i][RDP_M]))
            { // first is COLOR->COLOR,?
                rst.s_combx[i][RDP_X]=C_MUL(rst.s_combx[i][RDP_X],
                                            rst.s_combx[i+2][RDP_M]);
                rst.s_combx[i][RDP_Y]=C_MUL(rst.s_combx[i][RDP_Y],
                                            rst.s_combx[i+2][RDP_M]);
                rst.s_combx[i][RDP_A]=C_MUL(rst.s_combx[i][RDP_A],
                                            rst.s_combx[i+2][RDP_M]);
            }
            else if(rst.s_combinestyle[i]==STYLE_BLEND &&
               !ISCOLOR(rst.s_combx[i][RDP_X]) &&
               !ISCOLOR(rst.s_combx[i][RDP_Y]) &&
               ISCOLOR(rst.s_combx[i][RDP_M]) )
            { // first is TEX?->TEX?,COLORf:
                rst.s_combx[i][RDP_M]=C_MUL(rst.s_combx[i][RDP_M],
                                            rst.s_combx[i+2][RDP_M])|C_SPECIALCASE;
                // special case handling for this
            }
            else if(rst.s_combinestyle[i]==STYLE_FULL &&
               ISCOLOR(rst.s_combx[i][RDP_X]) &&
               ISCOLOR(rst.s_combx[i][RDP_Y]) &&
               ISCOLOR(rst.s_combx[i][RDP_A]) )
            { // first is (COLOR-COLOR)*?+COLOR
                rst.s_combx[i][RDP_X]=C_MUL(rst.s_combx[i][RDP_X],
                                            rst.s_combx[i+2][RDP_M]);
                rst.s_combx[i][RDP_Y]=C_MUL(rst.s_combx[i][RDP_Y],
                                            rst.s_combx[i+2][RDP_M]);
                rst.s_combx[i][RDP_A]=C_MUL(rst.s_combx[i][RDP_A],
                                            rst.s_combx[i+2][RDP_M]);
            }
            else continue;

            com_setcomb(i+2);
            if(dump) logd(" #C_MUL ",i);
        }
    }
}

// combine single cycle COLOR*COLOR
void com_combinemul1(void)
{
    int i;
    for(i=0;i<2;i++)
    {
        if(rst.s_combinestyle[i]==STYLE_MUL &&
           ISCOLOR(rst.s_combx[i][RDP_X]) &&
           ISCOLOR(rst.s_combx[i][RDP_M]) )
        { // second cycle is *COLOR
            rst.s_combx[i][RDP_A]=C_MUL(rst.s_combx[i][RDP_X],
                                        rst.s_combx[i][RDP_M]);
            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            if(dump) logd(" #SimplifyA%i ",i);
        }
    }
}

// combine single 1*X or X*1
void com_combinemulone(void)
{
    int i;
    for(i=0;i<4;i++)
    {
        if(rst.s_combinestyle[i]==STYLE_MUL &&
           rst.s_combx[i][RDP_X]==C_ONE)
        { // second cycle is *COLOR
            rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_M];
            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            if(dump) logd(" #*1 ");
        }
        else if(rst.s_combinestyle[i]==STYLE_MUL &&
                rst.s_combx[i][RDP_M]==C_ONE)
        { // second cycle is *COLOR
            rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_X];
            rst.s_combx[i][RDP_X]=0;
            rst.s_combx[i][RDP_Y]=0;
            rst.s_combx[i][RDP_M]=0;
            if(dump) logd(" #*1 ");
        }
    }
}

// convert dual cycle to single cycle if possible (set second to passthrough)
void com_joincycles(void)
{
    int i,j;
    if(rst.s_combinecycles<2) return;
    for(i=0;i<2;i++)
    {
        if(rst.s_combinestyle[i]==STYLE_CONST)
        { // first cycle = const, replace COMB -> const
            for(j=0;j<4;j++)
            {
                if(rst.s_combx[i+2][j]==C_COMB)
                {
                    rst.s_combx[i+2][j]=rst.s_combx[i][RDP_A];
                }
            }
            // copy second cycle to first
            memcpy(rst.s_combx[i],rst.s_combx[i+2],4*4);
            com_setcomb(i+2);
        }
    }
}

int  com_issingle(int i)
{
    if(rst.s_combinecycles<2) return(1);
    if(rst.s_combinestyle[i+2]==STYLE_COMB) return(1);
    return(0);
}

void com_forcesingle(int i)
{
    // need to remove second cycle, for now let's just drop it
    com_setcomb(i+2);
    if(dump) logd(" #droppedC2! ");
}

void com_forcesimple(int i)
{
    // need to simplyfy, just leave A
    if(rst.s_combinestyle[i]==STYLE_BLEND &&
       rst.s_combx[i][RDP_M]&C_SPECIALCASE)
    {
        rst.s_combx[i][RDP_Y]=0;
        rst.s_combx[i][RDP_X]=rst.s_combx[i][RDP_A];
        rst.s_combx[i][RDP_M]=(rst.s_combx[i][RDP_M]>>8)&255;
        rst.s_combx[i][RDP_A]=0;
        rst.s_combinestyle[i]=STYLE_MUL;
        if(dump) logd(" #droppedBLENDS! ");
    }
    /*
    else if(rst.s_combinestyle[i]==STYLE_BLEND &&
            ISCOLOR(rst.s_combx[i][RDP_Y]) &&
            rst.s_combx[i][RDP_X]==C_TEX0)
    {
        rst.s_combx[i][RDP_X]=rst.s_combx[i][RDP_X];
        rst.s_combx[i][RDP_M]=rst.s_combx[i][RDP_M];
        rst.s_combx[i][RDP_A]=0;
        rst.s_combx[i][RDP_Y]=0;
        rst.s_combinestyle[i]=STYLE_MUL;
        if(dump) logd(" #droppedCOL->TXT! ");
    }
    */
    else
    {
        if(rst.s_combinestyle[i]==STYLE_FULL)
        {
            if((rst.s_combx[i][RDP_M]==C_TEX0 ||
                rst.s_combx[i][RDP_M]==C_TEX1) &&
                ISCOLOR(rst.s_combx[i][RDP_A]))
            {
               rst.s_combx[i][RDP_X]=rst.s_combx[i][RDP_M];
               rst.s_combx[i][RDP_Y]=0;
               rst.s_combx[i][RDP_M]=C_ONE;
               rst.s_combinestyle[i]=STYLE_ADD;
               if(dump) logd(" #droppedXYM+! ");
               return;
            }
        }

        if(rst.s_combx[i][RDP_M]==C_TEX0 ||
           rst.s_combx[i][RDP_M]==C_TEX1)
        {
           rst.s_combx[i][RDP_A]=rst.s_combx[i][RDP_M];
        }
        rst.s_combx[i][RDP_X]=0;
        rst.s_combx[i][RDP_Y]=0;
        rst.s_combx[i][RDP_M]=0;
        rst.s_combinestyle[i]=STYLE_CONST;
        if(dump) logd(" #droppedXYM! ");
    }
}

void com_unknown(int unknown)
{
    // no supported
    COM.passes=1;
    COM.col[0][0]=C_DUNNO;
    COM.com[0][0]=X_COLOR;
    COM.col[0][1]=C_ONE;
    COM.com[0][1]=X_COLOR;
    COM.txt[0]=0;
    COM.txt[1]=0;
    logd("\n+mode combine-???: u%i",unknown);
    if(unknown&2) logd(",A?");
    if(unknown&1) logd(",C?");

    if(showwire || showinfo || showtest)
    {
        // put a flashing color in these modes
        logd(" (used flashing)");
    }
    else
    {
        // come up with something
        if(rst.s_combinetexboth&1)
        {
            rst.col[0][0]=0;
            COM.com[0][0]=X_TEXTURE;
            COM.texenable=1;
            logd(" (used tex0)");
        }
        else
        {
            rst.col[0][0]=rst.s_combx[0][RDP_A];
            if(rst.col[0][0]>C_TEX0) rst.col[0][0]=C_ONE;
            COM.com[0][0]=X_COLOR;
            COM.texenable=0;
            logd(" (used color %s)",cname[rst.col[0][0]]);
        }
    }
}

// clear 3DFX mapping
void com_clearmapping(int a)
{
    int i;
    for(i=0;i<4;i++)
    {
        COM.col[i][a]=0;
        COM.com[i][a]=X_COLOR;
        if(a==0)
        {
            COM.txt[i]=0;
            COM.blend1[i]=rst.s_blend1;
            COM.blend2[i]=rst.s_blend2;
        }
    }
    if(a==0)
    {
        COM.env=0;
    }
    COM.passes=0;
}

void com_alpha1mapping(void)
{
    int i;
    for(i=0;i<4;i++)
    {
        COM.col[i][1]=C_ONE;
        COM.com[i][1]=X_COLOR;
    }
}

void com_copyalphan(int n)
{
    int i;
    for(i=1;i<n;i++)
    {
        COM.col[i][1]=COM.col[0][1];
        COM.com[i][1]=COM.com[0][1];
        // also override texture
        if(COM.com[0][1]!=X_ONE && COM.com[0][1]!=X_COLOR)
        {
            COM.txt[i]=COM.txt[0];
        }
    }
}

void com_debugcolorshade(int r,int g,int b)
{
    COM.passes=1;
    COM.col[0][0]=C_MUL(C_COMB,C_SHADE);
    COM.com[0][0]=X_COLOR;
    COM.col[0][1]=C_ONE;
    COM.com[0][1]=X_COLOR;
    COM.blend1[0]=X_ONE;
    COM.blend2[0]=X_ZERO;
    COM.txt[0]=0;
    setcolor(C_COMB,(r<<24)+(g<<16)+(b<<8)+255);
}

void com_debugcolor(int r,int g,int b)
{
    COM.passes=1;
    COM.col[0][0]=C_COMB;
    COM.com[0][0]=X_COLOR;
    COM.col[0][1]=C_ONE;
    COM.com[0][1]=X_COLOR;
    COM.blend1[0]=X_ONE;
    COM.blend2[0]=X_ZERO;
    COM.txt[0]=0;
    setcolor(C_COMB,(r<<24)+(g<<16)+(b<<8)+255);
}

void com_debugshade(void)
{
    COM.passes=1;
    COM.col[0][0]=C_SHADE;
    COM.com[0][0]=X_COLOR;
    COM.col[0][1]=C_ONE;
    COM.com[0][1]=X_COLOR;
    COM.blend1[0]=X_ONE;
    COM.blend2[0]=X_ZERO;
    COM.txt[0]=0;
    COM.env=1;
    COM.texenable=0;
}

void com_debugtexture(int t)
{
    COM.passes=1;
    COM.col[0][0]=C_COMB;
    COM.com[0][0]=X_TEXTURE;
    COM.col[0][1]=C_COMB;
    COM.com[0][1]=X_TEXTURE;
    COM.txt[0]=t;
    COM.blend1[0]=X_ONE;
    COM.blend2[0]=X_ZERO;
    COM.texenable=1<<t;
    setcolor(C_COMB,(64<<24)+(64<<16)+(64<<8)+255);
}

// find a single cycle mapping
int com_createmapping1(int a)
{
    int c=COM.passes;

    if(!rst.s_combinetex[a])
    { // no texture
        if(dump) logd(" #notxt(%i) ",rst.s_combinestyle[a]);
        switch(rst.s_combinestyle[a])
        {
        case STYLE_CONST:
            COM.col[c][a]=rst.s_combx[a][RDP_A];
            COM.com[c][a]=X_COLOR;
            c++;
            break;
        default:
            COM.col[c][a]=C_FULL; // special handling in mixcolors
            COM.com[c][a]=X_COLOR;
            c++;
            break;
        }
    }
    else if(rst.s_combinetex[a]==1 || rst.s_combinetex[a]==2)
    { // single cycle with one texture
        // texture is in X param (if not CONST in which case it's A)
        int tex,texa;
        if(dump) logd(" #1txt(%i) ",rst.s_combinestyle[a]);
        if(rst.s_combinetex[a]==1)
        { // TEX0
            COM.txt[c]=0;
            tex=C_TEX0;
            texa=C_TEX0A;
        }
        else
        { // TEX1
            COM.txt[c]=1;
            tex=C_TEX1;
            texa=C_TEX1A;
        }
        switch(rst.s_combinestyle[a])
        {
        case STYLE_BLEND:
            // color->TEX0,COLOR
            if(rst.s_combinestyle[a]==STYLE_BLEND &&
               ISCOLOR(rst.s_combx[a][RDP_A])     &&
               ISCOLOR(rst.s_combx[a][RDP_M]))
            {
                if(dump) logd(" #Color->TEX0,COLOR ");
                COM.col[c][a]=C_MUL(rst.s_combx[a][RDP_M],rst.s_combx[a][RDP_Y])|C_NEGATE;
                COM.com[c][a]=X_COLOR;
                c++;
                COM.col[c][a]=rst.s_combx[a][RDP_M];
                COM.com[c][a]=X_MUL;
                COM.blend1[c]=COM.blend1[c-1];
                COM.blend2[c]=X_ONE;
                c++;
                break;
            }
            // color->TEX0,TEX0A detect (opaque only)
            else if(rst.s_combinestyle[a]==STYLE_BLEND &&
               ISCOLOR(rst.s_combx[a][RDP_A])     &&
               rst.s_combx[a][RDP_M]==texa        )
            {
                if(dump) logd(" #Color->TEX0,TEXA ");
                COM.col[c][a]=rst.s_combx[a][RDP_A];
                COM.com[c][a]=X_TEXTUREBLEND;
                c++;
                break;
            }
            // color->color,TEX0
            else if(rst.s_combinestyle[a]==STYLE_BLEND &&
                    ISCOLOR(rst.s_combx[a][RDP_Y])     &&
                    ISCOLOR(rst.s_combx[a][RDP_X])     )
            {
                if(dump) logd(" #Color->Color");
                if(rst.s_combx[a][RDP_Y]==C_SHADE)
                {
                    COM.env=rst.s_combx[a][RDP_X];
                    COM.col[c][a]=rst.s_combx[a][RDP_Y];
                    COM.com[c][a]=X_TEXTUREENVCR;
                    if(COM.com[c][1]==X_TEXTUREENVCR) COM.com[c][1]=X_TEXTURE;

                }
                else
                {
                    COM.env=rst.s_combx[a][RDP_Y];
                    COM.col[c][a]=rst.s_combx[a][RDP_X];
                    COM.com[c][a]=X_TEXTUREENVC;
                    if(COM.com[c][1]==X_TEXTUREENVC) COM.com[c][1]=X_TEXTURE;
                }
                c++;
                break;
            }
            else return(1);
            break;
        case STYLE_CONST:
            COM.col[c][a]=0;
            COM.com[c][a]=X_TEXTURE;
            c++;
            break;
        case STYLE_MUL:
            COM.col[c][a]=rst.s_combx[a][RDP_M];
            COM.com[c][a]=X_MUL;
            c++;
            break;
        case STYLE_ADD:
            if(rst.s_combx[a][RDP_X]==C_TEX0 ||
               rst.s_combx[a][RDP_X]==C_TEX1)
            {
                COM.col[c][a]=rst.s_combx[a][RDP_A];
                COM.com[c][a]=X_ADD;
                c++;
            }
            else if(rst.s_combx[a][RDP_A]==C_TEX0 ||
                    rst.s_combx[a][RDP_A]==C_TEX1)
            {
                COM.col[c][a]=rst.s_combx[a][RDP_X];
                COM.com[c][a]=X_ADD;
                c++;
            }
            break;
        default:
            return(1);
        }
    }
    else if(rst.s_combinetex[a]==3)
    { // dual txt
        if(dump) logd(" #2txt(%i) ",rst.s_combinestyle[a]);
        if(rst.s_combinestyle[a]==STYLE_MUL &&
           !ISCOLOR(rst.s_combx[a][RDP_X]) &&
           !ISCOLOR(rst.s_combx[a][RDP_M]) )
        {
            // T1*T2
            COM.col[c+0][a]=C_ONE;
            COM.col[c+1][a]=C_ONE;
            COM.com[c+0][a]=X_TEXTURE;
            COM.com[c+1][a]=X_TEXTURE;
            COM.txt[c+0]=0;
            COM.txt[c+1]=1;
            COM.blend1[c+1]=X_OTHER;
            COM.blend2[c+1]=X_ZERO;
            c+=2;
        }
        else if(rst.s_combinestyle[a]==STYLE_BLEND &&
           ISCOLOR(rst.s_combx[a][RDP_M]) )
        {
            // [ T1->T2,C1 ]
            // pass1: T1*(1-C1)
            // pass2: T2*(  C1)
            // also handles case when C1=C_MUL(x,a) where *a was on cycle2 [C_SPECIALCASE]
            if(rst.s_combx[a][RDP_Y]==C_TEX0 &&
               rst.s_combx[a][RDP_X]==C_TEX1)
            {
                COM.txt[c+0]=0;
                COM.txt[c+1]=1;
            }
            else if(rst.s_combx[a][RDP_Y]==C_TEX1 &&
                    rst.s_combx[a][RDP_X]==C_TEX0)
            {
                COM.txt[c+0]=0;
                COM.txt[c+1]=1;
            }
            else return(1);

            if((rst.s_combx[a][RDP_M]&C_MULCASE) &&
              !(rst.s_combx[a][RDP_M]&C_SPECIALCASE))
            {
                logd(" #mulnotspecial! ");
                return(1);
            }

            COM.col[c+0][a]=rst.s_combx[a][RDP_M]|C_NEGATE;
            COM.col[c+1][a]=rst.s_combx[a][RDP_M]         ;
            COM.com[c+0][a]=X_MUL;
            COM.com[c+1][a]=X_MUL;
            COM.blend2[c+1]=X_ONE;
            COM.blend2[c+1]=X_ONE;

            c+=2;
        }
        else
        {
            return(1);
        }
    }

    COM.passes=c;
    return(0);
}

// find the second cycle mapping
int com_createmapping2(int a)
{
    int c=COM.passes;

    if(dump) logd(" #pass2(%i): ",a);

    if(!rst.s_combinetex[a])
    { // no texture
        if(dump) logd(" #notxt(%i) ",rst.s_combinestyle[a]);
        switch(rst.s_combinestyle[a])
        {
        case STYLE_MUL:
            COM.col[c][a&1]=rst.s_combx[a][RDP_M];
            //print("mul col=%i (%i) c=%i a=%i\n",rst.s_combx[a][RDP_M],COM.col[1][0],c,a);
            COM.com[c][a&1]=X_COLOR;
            COM.blend1[c]=X_OTHER;
            COM.blend2[c]=X_ZERO;
            c++;
            break;
        case STYLE_ADD:
            COM.col[c][a&1]=rst.s_combx[a][RDP_A];
            COM.com[c][a&1]=X_COLOR;
            COM.blend1[c]=X_ONE;
            COM.blend2[c]=X_ONE;
            c++;
            break;
        default:
            return(1);
        }
    }
    else if(rst.s_combinetex[a]==1 || rst.s_combinetex[a]==2)
    { // single cycle with one texture (highlight only supported)
        // texture is in X param (if not CONST in which case it's A)
        int tex,texa;
        if(dump) logd(" #1txt ");
        if(rst.s_combinetex[a]==1)
        { // TEX0
            COM.txt[c]=0;
            tex=C_TEX0;
            texa=C_TEX0A;
        }
        else
        { // TEX1
            COM.txt[c]=1;
            tex=C_TEX1;
            texa=C_TEX1A;
        }
        switch(rst.s_combinestyle[a])
        {
        case STYLE_BLEND:
            // COMB->ENV,TEX0
            if(rst.s_combinestyle[a]==STYLE_BLEND &&
               ISCOLOR(rst.s_combx[a][RDP_X])     &&
               rst.s_combx[a][RDP_Y]==C_COMB      &&
               ( rst.s_combx[a][RDP_M]==C_TEX0 ||
                 rst.s_combx[a][RDP_M]==C_TEX1 ) )
            {
                COM.col[c][0]=rst.s_combx[a][RDP_X];
                COM.com[c][0]=X_SUB;
                COM.blend1[c]=X_ONE;
                COM.blend2[c]=X_ONE;
                c++;
                break;
            }
            break;
        default:
            return(1);
        }
    }
    else if(rst.s_combinetex[a]==3)
    { // dual txt
        if(dump) logd(" #2txt-fail ");
        return(1);
    }

    COM.passes=c;
    return(0);
}

// try to create a mode for 3DFX based on N64 mode [final analyze]
// this is for single cycle modes
int com_createmapping(int a)
{
    int e;
    com_clearmapping(a);
    if(com_issingle(a))
    {
        if(dump) logd(" #1P%i ",a);
        e=com_createmapping1(a);
    }
    else
    {
        if(dump) logd(" #2P%i ",a);
        e =com_createmapping1(a);
        e+=com_createmapping2(a+2);
    }
    if(dump) logd(" #C%i ",COM.passes);
    return(e);
}

int combx_flames[16]={
C_TEX1,C_PRIM,C_PLODF,C_TEX0, C_TEX1,C_ONE ,C_PLODF,C_TEX0,
C_PRIM,C_ENV ,C_COMB ,C_ENV , C_COMB,0     ,C_PRIM ,0     };
// Cycle1: (TEX1-PRIM)*PLODF+TEX0 Cycle2: ENV->PRIM,COMB
// Cycle1: (TEX1-1)*PLODF+TEX0 Cycle2: COMB*PRIM

// modepatch
int com_patchedmodes(void)
{
    int c=0;
    if(!rst.dualtmu)
    {
        return(0);
    }

    // patch Zelda flames using Voodoo2 dual texture
    if(!memcmp(&rst.s_combx,&combx_flames,16*sizeof(int)))
    {
//        COM.env      =C_MUL(rst.s_combx[2][RDP_Y],C_ALP75);
//        COM.col[c][0]=C_MUL(rst.s_combx[2][RDP_X],C_ALP75);
        COM.env      =rst.s_combx[2][RDP_Y];
        COM.col[c][0]=rst.s_combx[2][RDP_X];
        COM.com[c][0]=X_TEXTUREENVC;
        COM.col[c][1]=rst.s_combx[3][RDP_M];
        COM.com[c][1]=X_MUL;
        COM.txt[0]   =0;
        COM.txt[1]   =1;
        COM.blend1[c]=X_ALPHA;
        COM.blend2[c]=X_INVOTHERALPHA;
        COM.dualtxt  =X_MUL; // X_MULADD

        COM.texenable=3;
        COM.passes=1;
        return(1);
    }
    return(0);
}

void com_new(dword x0,dword x1)
{
    int a,oldp,unknown=0;

    com_convertcombine(x0,x1);

    // clear second cycle if singlepass
    if(rst.s_combinecycles==1)
    {
        com_setcomb(2);
        com_setcomb(3);
    }

    com_calculatestyles();
    rst.s_combinetexbothbak=rst.s_combinetexboth;

    if(dump) dumpscombine("*+mode combine-00");

    if(rst.flat) com_flat();

    if(!com_patchedmodes())
    {
        // use generic mode compiler

        com_reorder();
        com_calculatestyles();

        com_removefullblend();
        com_calculatestyles();

        if(dump) dumpscombine("*+mode combine-01");

        com_removelod(); // destructive!
        com_calculatestyles();

        com_removetextex2(); // destructive!
        com_calculatestyles();

        com_removemuladd(); // destructive!
        com_calculatestyles();

        if(dump) dumpscombine("*+mode combine-02");

        com_combinemul2();
        com_calculatestyles();
        com_combinemul1();
        com_calculatestyles();
        com_combinemulone();
        com_calculatestyles();

        if(dump) dumpscombine("*+mode combine-03");

        com_joincycles();
        com_calculatestyles();

        if(dump) dumpscombine("*+mode combine-04");

        com_combinemul2();
        com_calculatestyles();
        com_combinemul1();
        com_calculatestyles();
        com_combinemulone();
        com_calculatestyles();

        if(dump) dumpscombine("+mode combine-91");

        // work on the color
        a=com_createmapping(0);
        if(a)
        {
            logd(" #fail1?? ");
            com_forcesingle(0);
            com_calculatestyles();
            if(dump) dumpscombine("+mode combine-92");
            a=com_createmapping(0);
        }
        if(a)
        {
            logd(" #fail2?? ");
            com_forcesimple(0);
            com_calculatestyles();
            if(dump) dumpscombine("+mode combine-93");
            a=com_createmapping(0);
        }
        if(a)
        {
            logd(" #fail ");
            unknown|=1;
        }
        logd(" p=%i ",COM.passes);

        if(dump) dumpscombine("+mode combine-99");

        if(dump) dumpscombine("!+mode combine-91");

        // work on alpha
        oldp=COM.passes; // save passes
        {
            if(!com_issingle(1))
            {
                com_forcesingle(1); // multicycle won't work
                com_calculatestyles();
                if(dump) logd(" #failA1?? ");
            }
            a=com_createmapping(1);
            if(a)
            {
                if(dump) logd(" #failA2?? ");
                com_forcesimple(1);
                com_calculatestyles();
                if(dump) dumpscombine("!+mode combine-92");
                a=com_createmapping(1);
            }
            if(a)
            {
                if(dump) logd(" #failA3?? ");
                com_alpha1mapping();
            }
        }
        logd(" p=%i ",COM.passes);
        COM.passes=oldp; // restore passes
        if(COM.passes>1)
        { // copy alpha pass 1->2
            com_copyalphan(COM.passes);
        }

        if(dump) dumpscombine("!+mode combine-99");
    //    if(dump) dump_combineresult();
    }

    COM.texenable=rst.s_combinetexboth;

    if(unknown) com_unknown(unknown);
}

void com_settest(void)
{
    if(showtest==0)
    {
        // normal
    }
    else if(showtest==1)
    {
        // show txt0
        if(rst.s_combinetexbothbak&1)
        {
            com_debugtexture(0);
        }
        else
        {
            com_debugcolor(64,64,64);
        }
    }
    else if(showtest==2)
    {
        // show txt1
        if(rst.s_combinetexbothbak&2)
        {
            com_debugtexture(1);
        }
        else
        {
            com_debugcolor(64,64,64);
        }
    }
    else if(showtest==3)
    {
        // show passcount
        if(COM.passes==0) com_debugcolorshade(64,64,64);
        if(COM.passes==1) com_debugcolorshade(0,0,192);
        if(COM.passes==2) com_debugcolorshade(0,128,128);
        if(COM.passes> 2) com_debugcolorshade(0,128,0);
    }
}

void change_combine(dword x0,dword x1,dword chg0,dword chg1)
{
    int ci;

    dump=st.dumpgfx;

    if(dump) logd("\n+mode combine----: %i cycles",rst.s_cycles);

    if(rst.s_cycles==4)
    { // fill
        if(dump) logd(" fill ");
        rst.s_c=&comfill;
        COM.texenable=0;
        return;
    }
    if(rst.s_cycles==3)
    { // copy
        if(dump) logd(" copy ");
        rst.s_c=&comcopy;
        COM.texenable=1;
        return;
    }

    if(rdp_n64combiner)
    {
        n64_combine(x0,x1);
        rst.s_c=&comn64;
        return;
    }

    // combinemode (caching)
    ci=mode_findcache(rst.combine0[0],rst.combine1[0],rst.other0[0],rst.other1[0]);
    if(ci<0)
    {
        ci=mode_newcache(rst.combine0[0],rst.combine1[0],rst.other0[0],rst.other1[0]);
        if(dump) logd(" combcachemiss ");
    }
    else
    {
        if(dump) logd(" combcachehit(%i) ",ci);
    }

    com_new(x0,x1);

    if(showtest) com_settest();

    /*
    if(rst.other0[0]==0xef08ac10 && rst.other1[0]==0x0f0a4000)
    {
        COM.passes=0;
    }
    */
}

void com_init(void)
{
    static int flip=0;
    //
    rst.s_c=&rst.combdummy;
    // fill
    comfill.passes=1;
    comfill.com[0][0]=X_COLOR;
    comfill.com[0][1]=X_COLOR;
    comfill.col[0][0]=C_FILL;
    comfill.col[0][1]=C_FILL;
    comfill.txt[0]=0;
    // copy
    comcopy.passes=1;
    comcopy.com[0][0]=X_TEXTURE;
    comcopy.com[0][1]=X_TEXTURE;
    comcopy.col[0][0]=0;
    comcopy.col[0][1]=0;
    comcopy.txt[0]=0;
    // setup static colors
    setcolor(C_ZERO ,0);
    setcolor(C_ONE  ,0xffffffff);
    setcolor(C_DUNNO,0xff00ffff+(flip<<24));
    setcolor(C_ALP50,0xffffff7f);
    setcolor(C_COL50,0x7f7f7fff);
    setcolor(C_ALP75,0xffffffaf);
    setcolor(C_ALL50,0x7f7f7f7f);
    setcolor(C_ALL25,0x3f3f3f3f);
    // clear any combine modes
    com_setzero(0);
    com_setzero(1);
    com_setzero(2);
    com_setzero(3);
    com_calculatestyles();
}

/****************************************************************************
/* Rendering State conversion - Other
*/

void dump_other(dword o0,dword o1)
{ // doesn't dump blend or redermode
    int a,c,cn,line;
    dword x;

    for(line=0;line<6;line++)
    {
        if(line==0)
        {
            logd("\n+mode other/misc1:");

            x=o0;

            a=(x>>4)&3;
            switch(a)
            {
            case 0: logd(" adither(pattern)"); break;
            case 1: logd(" adither(notpat)"); break;
            case 2: logd(" adither(noise)"); break;
            case 3: logd(" adither(disable)"); break;
            }

            a=(x>>6)&3;
            switch(a)
            {
            case 0: logd(" cdither(magicsq)"); break;
            case 1: logd(" cdither(bayer)"); break;
            case 2: logd(" cdither(noise)"); break;
            case 3: logd(" cdither(disable)"); break;
            }

            a=(x>>8)&1;
            switch(a)
            {
            case 0: logd(" combkey(0)"); break;
            case 1: logd(" combkey(1)"); break;
            }

            a=(x>>9)&7;
            switch(a)
            {
            case 0: logd(" tconv(conv)"); break;
            case 5: logd(" tconv(filtconv)"); break;
            case 6: logd(" tconv(filt)"); break;
            default: logd(" tconv(%i)",a); break;
            }
        }
        if(line==1)
        {
            logd("\n+mode other/misc2:");

            x=o0;

            a=(x>>12)&7;
            switch(a)
            {
            case 0: logd(" tfilt(point)"); break;
            case 2: logd(" tfilt(bilerp)"); break;
            case 3: logd(" tfilt(aver)"); break;
            default: logd(" tfilt(%i)",a); break;
            }

            a=(x>>14)&3;
            switch(a)
            {
            case 0: logd(" tlut(none)"); break;
            case 2: logd(" tlut(rgba16)"); break;
            case 3: logd(" tlut(ia16)"); break;
            default: logd(" tlut(%i)",a); break;
            }

            a=(x>>16)&1;
            switch(a)
            {
            case 0: logd(" tlod(tile)"); break;
            case 1: logd(" tlod(lod)"); break;
            }

            a=(x>>17)&3;
            switch(a)
            {
            case 0: logd(" tdet(clamp)"); break;
            case 1: logd(" tdet(sharpen)"); break;
            case 2: logd(" tdet(detail)"); break;
            default: logd(" tdet(%i)",a); break;
            }

            a=(x>>19)&1;
            logd(" tpersp(%i)",a);
        }
        if(line==2)
        {
            logd("\n+mode other/misc3:");

            x=o0;

            a=(x>>20)&3;
            switch(a)
            {
            case 0: logd(" cycle(1c)"); break;
            case 1: logd(" cycle(2c)"); break;
            case 2: logd(" cycle(copy)"); break;
            case 3: logd(" cycle(fill)"); break;
            }
            //rst.cycles=a;

            a=(x>>23)&1;
            switch(a)
            {
            case 0: logd(" pip(Npr)"); break;
            case 1: logd(" pip(1pr)"); break;
            }

            x=o1;

            a=(x>>0)&3;
            switch(a)
            {
            case 0: logd(" acmp(none)"); break;
            case 1: logd(" acmp(thresh)"); break;
            case 2: logd(" acmp(2)"); break;
            case 3: logd(" acmp(dither)"); break;
            }

            a=(x>>2)&1;
            switch(a)
            {
            case 0: logd(" zsrc(pixel)"); break;
            case 1: logd(" zsrc(prim)"); break;
            }
        }
        if(line==3)
        {
            logd("\n+mode other/rend1:");

            x=o1&(8191<<3);

            a=(x&0x08)?1:0;
            logd(" aa(%i)",a);

            a=(x&0x10)?1:0;
            logd(" zcmp(%i)",a);
            //rst.zcmp=a;

            a=(x&0x20)?1:0;
            logd(" zupd(%i)",a);
            //rst.zupd=a;

            a=(x&0x40)?1:0;
            logd(" crd(%i)",a);

            a=(x&0xc00);
            switch(a)
            {
            case 0x000: logd(" zmode(opa)"); break;
            case 0x400: logd(" zmode(inter)"); break;
            case 0x800: logd(" zmode(xlu)"); break;
            case 0xc00: logd(" zmode(dec)"); break;
            }
            //rst.zmode=a>>10;
        }
        if(line==4)
        {
            logd("\n+mode other/rend2:");

            x=o1&(8191<<3);

            a=(x&0x4000)?1:0;
            logd(" fbl(%i)",a);
            //rst.forcebl=a;

            a=(x&0x80)?1:0;
            logd(" clrC(%i)",a);

            a=(x&0x300);
            switch(a)
            {
            case 0x000: logd(" cvg(dst_clamp)"); break;
            case 0x100: logd(" cvg(dst_wrap)"); break;
            case 0x200: logd(" cvg(dst_full)"); break;
            case 0x300: logd(" cvg(dst_save)"); break;
            }

            a=(x&0x1000)?1:0;
            logd(" cvg*(%i)",a);

            a=(x&0x2000)?1:0;
            logd(" atst(%i)",a);
            //rst.alphatest=a;
        }
        if(line==5)
        {
            int a0;

            logd("\n+mode other/blend:");

            x=o1>>16;

            x&=0xffff;
            logd(" %04X",x);

            if(rst.s_cycles==1) x>>=2;
            x&=0x3333;
            logd(" %04X ",x);

            if(x==0x0011)
            {
                logd("NORMAL");
            }
            else if(x==0x0302)
            {
                logd("OVERWR");
            }
            else if(x==0x0010)
            {
                logd("ALPHA ");
            }
            else if(x==0x1310)
            {
                logd("DARKEN");
            }
            else
            {
                logd("???   ");
            }

            x=o0;

            cn=rst.s_cycles;
            if(cn>2) cn=1;
            for(c=0;c<2;c++)
            {
                if(c==0) x=(o1>>18);
                else     x=(o1>>16);
                x&=0x3333;

                logd("  c%i:%04X ",c,x);

                if(c==1 && x==a0)
                {
                    logd("Same ");
                    continue;
                }
                a0=x;

                a=(x>>12)&3;
                switch(a)
                {
                case 0: logd("CIN"); break;
                case 1: logd("CMEM"); break;
                case 2: logd("CBL"); break;
                case 3: logd("CFOG"); break;
                }
                logd("*");
                a=(x>>8)&3;
                switch(a)
                {
                case 0: logd("AIN"); break;
                case 1: logd("FOGSH"); break;
                case 2: logd("FOGPR"); break;
                case 3: logd("0"); break;
                }

                logd("+");

                a=(x>>4)&3;
                switch(a)
                {
                case 0: logd("CIN"); break;
                case 1: logd("CMEM"); break;
                case 2: logd("CBL"); break;
                case 3: logd("?3"); break;
                }
                logd("*");
                a=(x>>0)&3;
                switch(a)
                {
                case 0: logd("(1-AIN)"); break;
                case 1: logd("AMEM"); break;
                case 2: logd("1"); break;
                case 3: logd("0"); break;
                }
            }
        }
    }
}

void change_other(dword x0,dword x1,dword c0,dword c1)
{
    if(FIELD(c0,20,3))
    {
        rst.s_cycles=FIELD(x0,20,3)+1;
        // cyclecount affects most settings, force changes
        rst.modechange=2;
        c1=c0=0xffffffff;
    }

    if(c0&0x7000)
    {
        rst.s_txtfilt=(x0>>12)&7;
    }
    if(c1&0xc00)
    {
        rst.s_zmode=(x1>>10)&3; // opa,inter,xlu,dec
    }
    if(c1&0x1800)
    {
        rst.s_cvgmode=(x1&0x1800)>>11;
    }
    if(c0&0xC000)
    {
        rst.s_tluttype=(x0>>14)&3;
    }
    if(c1&4)
    {
        rst.s_zsrc=(x1>>2)&1;
    }
    if(c1&0x30)
    {
        if(rst.s_cycles>2)
        {
            rst.s_zcmp=0;
            rst.s_zupd=0;
        }
        else
        {
            rst.s_zcmp=(x1&0x10)?1:0;
            rst.s_zupd=(x1&0x20)?1:0;
        }
        rst.s_noz=(!rst.s_zupd && !rst.s_zcmp);
    }
    if(c1&(0x3003))
    {
        rst.s_alphatst=x1&3;
        if((x1&0x1000)) rst.s_alphatst=4;
        // ALPHA_CVG_SEL without CVG_X_ALPHA: the threshold compare sees
        // coverage, not the combined alpha. Wheel of Fortune's lit set has
        // shade alpha 0 and was discarded.
        else if((x1&3)==1 && (x1&0x2000)) rst.s_alphatst=5;
        //if(rst.s_alphatst && rst.s_cycles>2) rst.s_alphatst=4;
    }
    if(c1&0x4000)
    {
        rst.s_forcebl =(x1&0x4000)?1:0;
    }
    if(c1&0xffff0000)
    {
        dword x;
        int b1,b2;

        x=x1>>16;

        #if 1

        if(rst.s_cycles==1) x>>=2;
        x&=0x3333;

        rst.s_blendbits=x;

        if(!x || x==0x0302)
        {
            b1=X_ONE;
            b2=X_ZERO;
        }
        else if(x==0x0011)
        {
            b1=X_ONE;
            b2=X_ZERO;
        }
        else if(x==0x0010)
        {
            b1=X_ALPHA;
            b2=X_INVOTHERALPHA;
        }
        else if(x==0x0110)
        {
            // CIN*FOG_A+MEM*(1-FOG_A): the fog color's alpha fades the
            // pixel in (flushprims). Bomberman 64 draws a black screen
            // rectangle this way every frame, with fog alpha 0.
            b1=X_CONSTALPHA;
            b2=X_INVCONSTALPHA;
        }
        else if(x==0x1310)
        {
            b1=X_ZERO;
            b2=X_ONE; //X_INVOTHERALPHA;
        }
        else
        {
            b1=X_ONE;
            b2=X_ZERO;
        }

        #else

        if(rst.s_cycles==2)
        { // use second cycle code
            x<<=2;
        }
        x&=0xcccc;

        if(x==0x0044)                         a=RDP_BLEND_AA_OPAQUE;
        else if(x==0x0050)                    a=RDP_BLEND_ALPHAMIX;
        else if(x==0x0040)                    a=RDP_BLEND_ALPHAMIX2;
        else if(x==0x4c40)                    a=RDP_BLEND_DARKEN;
        else if(x==0x0055)                    a=RDP_BLEND_MULALPHA;
        else if(x==0x0C08 || x==0x080C || !x) a=RDP_BLEND_NORMAL;
        else                                  a=RDP_BLEND_DUNNO;

        switch(a)
        {
        case RDP_BLEND_MULALPHA:
            b1=X_ALPHA;
            b2=X_ZERO;
            break;
        case RDP_BLEND_NORMAL:
        case RDP_BLEND_AA_OPAQUE:
            b1=X_ONE;
            b2=X_ZERO;
            break;
        case RDP_BLEND_ALPHAMIX2:
            b1=X_ALPHA;
            b2=X_INVOTHERALPHA;
            break;
        case RDP_BLEND_ALPHAMIX:
            b1=X_ALPHA;
            b2=X_INVOTHERALPHA;
            break;
        case RDP_BLEND_DARKEN:
            b1=X_ZERO;
            b2=X_INVOTHERALPHA;
            break;
        default:
            b1=b2=X_ONE;
            break;
        }

        #endif

        rst.s_blend1=b1;
        rst.s_blend2=b2;
        rst.modechange=2; // force combinechange
    }
}

int findwirecolor(dword o0,dword o1,dword c0,dword c1)
{
    dword x;

    if(1)
    {
        x =rst.other0[0];
        x=(x<<3)|(x>>29);
        x^=rst.other1[0];
        x=(x<<3)|(x>>29);
        x^=rst.combine0[0];
        x=(x<<3)|(x>>29);
        x^=rst.combine1[0];
        x=(x<<3)|(x>>29);
        x=(x%14)+1; // 1..14
        return(x);
    }

    return 7;
}

void newmode(void) // setmode
{
    dword m0,m1;    // mode bits    (set by IFCHANGED)
    dword m0c,m1c;  // bits changed (set by IFCHANGED)

    if(st.dumpgfx)
    {
        logd("\n\n+newmode() flush %i prims",rst.prtabi);
    }

    flushprims();

    rst.flat=rst.setflat;
    rst.texturetile=rst.nexttexturetile;

    if(rst.firstmodechange)
    {
        rst.modechange=2;
        rst.firstmodechange=0;
    }

    if(st.dumpgfx)
    {
        logd("\n+mode change %i data: other %08X %08X combine %08X %08X",
            rst.modechange,
            rst.other0[0],rst.other1[0],
            rst.combine0[0],rst.combine1[0]);
    }

    setintensityfromalpha(C_PRIA,C_PRIM);
    setintensityfromalpha(C_ENVA,C_ENV);

    // othermode
    m0=rst.other0[0];
    m1=rst.other1[0];
    m0c=m0^rst.other0[1];
    m1c=m1^rst.other1[1];
    if(rst.modechange>1) m0c=m1c=0xffffffff;
    if(m0c|m1c)
    {
        rst.other0[1]=m0;
        rst.other1[1]=m1;
        change_other(m0,m1,m0c,m1c);
        if(st.dumpgfx) dump_other(m0,m1);
    }

    m0=rst.combine0[0];
    m1=rst.combine1[0];
    m0c=m0^rst.combine0[1];
    m1c=m1^rst.combine1[1];
    if(rst.modechange>1) m0c=m1c=0xffffffff;
    if(m0c|m1c)
    {
        rst.combine0[1]=m0;
        rst.combine1[1]=m1;
        change_combine(m0,m1,m0c,m1c);
        if(st.dumpgfx) dump_combine(m0,m1);
    }

    // prepare textures
    if(rst.txtchange)
    {
        if(COM.texenable&2) txt_prepare(1);
        if(COM.texenable&1) txt_prepare(0);
        rst.txtchange=0;
    }

    if(showinfo || showwire || st.dumpgfx)
    { // mode not supported, make wires
        rst.debugwirecolor=findwirecolor(rst.other0[0],rst.other1[0],rst.combine0[0],rst.combine1[0]);
        logd("\n+wirecolor %i",rst.debugwirecolor);
        if(!rst.debugwirecolor) rst.debugwirecolor=2;
    }
    else
    {
        rst.debugwirecolor=0;
    }

    rst.modechange=0;
}

/****************************************************************************
/* Frame init/deinit
*/

static void clear(int color)
{
    x_clear(1,1,0.03*color,0.03*color,0.03*color);
}

static void swap(void);

// shows the frame waiting in rst.pendingcbuf
void rdp_present(void)
{
    if(!rst.presentpending) return;
    rst.presentpending=0;
    swap();
    if(snaprequest) snap_take(); // the frame just finished (F10/F12)
    // the presented picture now stands for this color buffer; textures
    // the game later reads from it come from the framegrab
    rst.frontcbuf     =rst.pendingcbuf;
    rst.frontcbufwid  =rst.pendingcbufwid;
    rst.frontcbufbytes=rst.pendingcbufbytes;
    rst.frontcbufhig  =rst.pendingcbufhig;
    rst.frontframe++;
    if(cart.fbwrite) rdp_fbwrite();
}

static void swap(void)
{
    x_finish();

    st.doframesync=1;

    if(showinfo)
    {
        x_viewport(0,0,init.gfxwid-1,init.gfxhig-1);
        clear(5);
        x_viewport(0,init.gfxhig*0.5,init.gfxwid-1,init.gfxhig-1);
        clear(0);
        realdrawmode();
    }
    else
    {
        x_viewport(0,0,init.gfxwid-1,init.gfxhig-1);
        // keepframe=1: like the real frame buffer, the picture stays until the
        // game draws over it (Pokemon Stadium menus draw their backdrop once)
        if(cart.keepframe) x_clear(0,1,0,0,0);
        else clear(0);
        realdrawmode();
    }
}

static void opendisplay(void)
{
    print("Graphics initialized: %ix%i\n",init.gfxwid,init.gfxhig);

    x_init();
    //x_open(NULL,NULL,init.gfxwid,init.gfxhig,2,1);
      {
         extern void *hwndMain; // void* since windows.h not included
         x_open(NULL,hwndMain,init.gfxwid,init.gfxhig,2,1);
      }
    rst.fullscreen=1;

    if(!init.novoodoo2 && x_combine2(X_WHITE,X_WHITE,0)==0) rst.dualtmu=1;
    else rst.dualtmu=0;

    swap();
}

static void closedisplay(void)
{
    rst.combcacheused=0;
    rdp_freetexmem();

    x_deinit();
    rst.fullscreen=0;
}

/****************************************************************************
/* Public Entrypoints
*/

void rdp_segment(int seg,dword base)
{
    if(seg>15)
    {
        error("rdp: segment > 15\n");
        seg=0;
    }
    rst.segment[seg]=base;
}

/****************************************************************************
** Render to texture. A color buffer whose width isn't a screen's (320,
** 640) is drawn into an offscreen FBO in its own pixels, starting from the
** RDRAM picture, and written back to RDRAM when the game sets another color
** buffer or the frame ends, so textures read from it get what was drawn.
** Pokemon Stadium draws its Game Pak Check cards into 128 wide buffers.
*/
#define RTT_H 512 // FBO height; only the rows the scissor reached go back

// The screen's own width is never offscreen, whatever it is: Banjo-Tooie's
// frames are 304 wide (its VI width), and drawing them into the FBO left the
// screen black.
static int rtt_offscreen(void)
{
    int w=rst.bufwid[RDP_BUF_C],fmt=rst.buffmt[RDP_BUF_C],bpp=rst.bufbpp[RDP_BUF_C];
    if(w==init.viewportwid) return(0);
    if(w==320 || w==640 || w<8 || w>1024) return(0);
    // RGBA 16/32-bit, or 8-bit CI/I: Yoshi's Story builds its 336x256 CI8
    // level backgrounds from 16x16 tiles with no TLUT (the index bytes)
    return((fmt==0 && (bpp==2 || bpp==3)) || (bpp==1 && (fmt==2 || fmt==4)));
}

static void rtt_begin(void)
{
    int    w=rst.bufwid[RDP_BUF_C],bpp=rst.bufbpp[RDP_BUF_C],x,y;
    dword  addr=rst.bufbase[RDP_BUF_C]&0x1fffffff;
    byte  *img,*d;

    flushprims();
    img=malloc(w*RTT_H*4);
    if(!img) return;
    for(y=0,d=img;y<RTT_H;y++)
        for(x=0;x<w;x++,d+=4)
        {
            dword a=addr+(dword)(y*w+x)*(bpp==3?4:bpp==2?2:1),c;
            if(a+4>(dword)mem.ramsize) { d[0]=d[1]=d[2]=d[3]=0; continue; }
            if(bpp==1)
            {   // 8-bit: the byte as a gray level
                d[0]=d[1]=d[2]=mem.ram[a^3];
                d[3]=255;
            }
            else if(bpp==3)
            {
                c=*(dword *)(mem.ram+a);
                d[0]=(byte)(c>>24); d[1]=(byte)(c>>16); d[2]=(byte)(c>>8); d[3]=(byte)c;
            }
            else
            {
                c=*(word *)(mem.ram+(a^2));
                d[0]=(byte)(((c>>11)&31)*255/31);
                d[1]=(byte)(((c>> 6)&31)*255/31);
                d[2]=(byte)(((c>> 1)&31)*255/31);
                d[3]=(c&1)?255:0;
            }
        }
    if(!x_rtt_begin(w,RTT_H,img)) { free(img); return; }
    free(img);

    rst.rtt_on=1;
    rst.rtt_addr=addr;
    rst.rtt_w=w;
    rst.rtt_bpp=bpp;
    rst.rtt_maxy=0;
    rst.rtt_saved[0]=init.gfxwid;
    rst.rtt_saved[1]=init.gfxhig;
    rst.rtt_saved[2]=init.viewportwid;
    rst.rtt_saved[3]=init.viewporthig;
    init.gfxwid=init.viewportwid=w;   // N64 pixels map 1:1 onto the FBO
    init.gfxhig=init.viewporthig=RTT_H;
    rst.rectmode=-1;
    rdp_viewport(w*0.5f,RTT_H*0.5f,w*0.5f,RTT_H*0.5f);
    x_scissor(0,0,0,0,0);
    logd("\n+rtt begin %08X %ix%i bpp %i",addr,w,RTT_H,bpp);
}

static void rtt_end(void)
{
    int    w=rst.rtt_w,h,x,y,j;
    byte  *img,*s,*cover,*cv;

    if(!rst.rtt_on) return;
    flushprims();
    h=rst.rtt_maxy>0?rst.rtt_maxy:w*3/4;
    if(h>RTT_H) h=RTT_H;
    img=malloc(w*h*4);
    cover=malloc(w*h);
    if(img && cover)
    {
        // the renderer writes no alpha: the alpha (coverage) bit comes from
        // what drew each pixel (x_rtt_cover), untouched pixels keep theirs.
        // Pokemon Stadium's VS screen pictures are models on a cleared
        // (alpha 0) background, drawn over each cell's grid.
        x_rtt_read(h,img,cover);
        for(y=0,s=img,cv=cover;y<h;y++)
            for(x=0;x<w;x++,s+=4,cv++)
            {
                dword a=rst.rtt_addr+(dword)(y*w+x)*(rst.rtt_bpp==3?4:rst.rtt_bpp==2?2:1);
                if(a+4>(dword)mem.ramsize) continue;
                if(rst.rtt_bpp==1)
                {   // 8-bit: the red channel, where something was drawn
                    if(*cv) mem.ram[a^3]=s[0];
                }
                else if(rst.rtt_bpp==3)
                {
                    dword al=*cv==2?255:*cv==1?0:(*(dword *)(mem.ram+a)&255);
                    *(dword *)(mem.ram+a)=((dword)s[0]<<24)|((dword)s[1]<<16)|((dword)s[2]<<8)|al;
                }
                else
                {
                    word al=*cv==2?1:*cv==1?0:(*(word *)(mem.ram+(a^2))&1);
                    *(word *)(mem.ram+(a^2))=(word)(((s[0]>>3)<<11)|((s[1]>>3)<<6)|((s[2]>>3)<<1)|al);
                }
            }
    }
    free(img);
    free(cover);
    x_rtt_end();
    rst.rtt_on=0;
    init.gfxwid     =rst.rtt_saved[0];
    init.gfxhig     =rst.rtt_saved[1];
    init.viewportwid=rst.rtt_saved[2];
    init.viewporthig=rst.rtt_saved[3];
    rst.rectmode=-1;
    // the viewport is RSP state, not the color buffer's: the display list's
    // one again. Blast Corps sets it before the SETCIMG that ends its 64x64
    // offscreen draw; a default here (window pixels, 2x the screen's) drew
    // every other frame zoomed in.
    if(rst.gamevpset) rdp_viewport(rst.gamevp[0],rst.gamevp[1],rst.gamevp[2],rst.gamevp[3]);
    else rdp_viewport(init.gfxwid/2,init.gfxhig/2,init.gfxwid/2,init.gfxhig/2);
    x_scissor(0,0,0,0,0);
    // RDRAM now holds the picture: textures read from it are not color
    // buffer placeholders (cbufsource)
    for(j=0;j<8;j++)
        if((rst.lastcbufs[j]&0x1fffffff)==rst.rtt_addr) rst.lastcbufs[j]=0;
    logd("\n+rtt end %08X %i rows written back",rst.rtt_addr,h);
}

// a texture load of len bytes starting at TMEM slot i (8 bytes each; line:
// its TMEM line for LOADTILE, 0 for LOADBLOCK): the slots it covers no
// longer start loads of their own
static void tmem_loaded(int i,int len,int line)
{
    int j;
    rst.tmemlen [i]=len>0?len:8;
    rst.tmemline[i]=line;
    for(j=i+1;j<i+len/8 && j<512;j++) rst.tmemlen[j]=0;
}

int rdp_cmd(dword *cmd)
{
    int c=cmd[0]>>24,last;
    static int last0;

    last=last0;
    last0=c;

    if(rst.prtabi!=rst.last_prtabi)
    {
        flushprims();
    }

    if(rst.wordsleft>0)
    {
        // handle multiword commands (texrect)
        // the actual command code is ignored, except that F3DEX2's
        // RDPHALF_2 (F1) straight after the texrect means s,t=0 and this
        // word is dsdx/dtdy (GLideN64 halfTexRect). All-Star Baseball 2000
        // sends its RDPHALF_1 earlier; taking two words ate the next glyph's
        // SETTIMG. Not F3D's B3: there E4,B3,B2 is the normal texrect (s,t
        // in B3), and treating it as half broke Super Mario 64's text.
        if(rst.wordsleft==2 && c==0xf1)
        {
            rst.texrect.s0=0;
            rst.texrect.t0=0;
            rst.wordsleft=1;
        }
        if(rst.wordsleft==2)
        {
            rst.texrect.s0=(short)FIELD(cmd[1],16,16);
            rst.texrect.t0=(short)FIELD(cmd[1],0,16);
            rst.wordsleft=1;
        }
        else
        {
            rst.texrect.s1=(short)FIELD(cmd[1],16,16);
            rst.texrect.t1=(short)FIELD(cmd[1],0,16);
            rst.wordsleft=0;
            if(st.dumpgfx)
            {
                logd("\n# texrect (%f,%f)-(%f,%f) (%f,%f)+*(%f,%f)",
                    rst.texrect.x0,
                    rst.texrect.y0,
                    rst.texrect.x1,
                    rst.texrect.y1,
                    rst.texrect.s0,
                    rst.texrect.t0,
                    rst.texrect.s1,
                    rst.texrect.t1);
            }
            rdp_texrect(&rst.texrect);
        }
        return(rst.wordsleft);
    }
    else switch(c)
    {
    case 0xff: // RDP_SETCIMG
        // Drawing moves to another buffer while the last frame still waits
        // for its VI swap: show that frame now. GL has one back buffer, so
        // the new frame would draw over it and the later present would clear
        // it away (Mario 64's triple buffering: black and half-drawn frames).
        // Not for the z-buffer clear, which renders into the z-buffer.
        if(rst.presentpending && address(cmd[1])!=rst.pendingcbuf &&
           address(cmd[1])!=rst.bufbase[RDP_BUF_Z])
        {
            flushprims();
            rdp_present();
        }
        if(rst.rtt_on && (address(cmd[1])&0x1fffffff)!=rst.rtt_addr) rtt_end();
        setbuffer(RDP_BUF_C,cmd[0],cmd[1]);
        if(!rst.rtt_on && rtt_offscreen()) rtt_begin();
        // HLE: the screen size otherwise comes only from a full screen
        // fillrect (rdp_fillrect); Pokemon Stadium's 640x480 screens clear
        // with a texrect and were drawn at 2x (only their top left quarter
        // showed). Standard widths only: odd ones are offscreen targets.
        if(!st.lleos)
        {
            int w=rst.bufwid[RDP_BUF_C];
            if((w==320 || w==640) && w!=init.viewportwid)
            {
                init.viewportwid=w;
                init.viewporthig=w*3/4;
                print("rdp: new screen resolution %ix%i (color buffer)\n",w,w*3/4);
                rdp_viewport(init.gfxwid/2,init.gfxhig/2,init.gfxwid/2,init.gfxhig/2);
            }
        }
        break;
    case 0xfe: // RDP_SETZIMG
        setbuffer(RDP_BUF_Z,cmd[0],cmd[1]);
        break;
    case 0xfd: // RDP_SETTIMG
        setbuffer(RDP_BUF_TXT,cmd[0],cmd[1]);
        logd("\n+tile settimg bpp=%s fmt=%s wid=%i %08X",
            bpp[rst.bufbpp[RDP_BUF_TXT]],
            fmt[rst.buffmt[RDP_BUF_TXT]],
            rst.bufwid[RDP_BUF_TXT],
            rst.bufbase[RDP_BUF_TXT]);
        break;
    case 0xfc: // RDP_SETCOMBINE
        rst.combine0[0]=cmd[0];
        rst.combine1[0]=cmd[1];
        rst.modechange=1;
        break;
    case 0xfb: // RDP_SETENVCOLOR
        setcolor(C_ENV,cmd[1]);
        break;
    case 0xfa: // RDP_SETPRIMCOLOR
        setcolor(C_PRIM,cmd[1]);
        // copy lod factor
        setcolorintensity(C_PLODF,cmd[0]&255);
        // lod clamp not copied
        break;
    case 0xf9: // RDP_SETBLENDCOLOR
        setcolor(C_BLEND,cmd[1]);
        break;
    case 0xf8: // RDP_SETFOGCOLOR
        setcolor(C_FOG,cmd[1]);
        rdp_fogrange(rst.fogmin,rst.fogmax); // reload fog
        break;
    case 0xf7: // RDP_SETFILLCOLOR
        rst.rawfillcolor=cmd[1];
        setfillcolor(C_FILL,cmd[1]);
        if(st.dumpgfx) logd("\n+fillcolor %08X",cmd[1]);
        break;
    case 0xf6: // RDP_FILLRECT
        rst.texrect.x0=0.25*FIELD(cmd[0],12,12);
        rst.texrect.y0=0.25*FIELD(cmd[0],0,12);
        rst.texrect.x1=0.25*FIELD(cmd[1],12,12);
        rst.texrect.y1=0.25*FIELD(cmd[1],0,12);
        if(st.dumpgfx)
        {
            logd("# fillrect (%f,%f)-(%f,%f)",
                rst.texrect.x0,
                rst.texrect.y0,
                rst.texrect.x1,
                rst.texrect.y1);
        }
        rdp_fillrect(&rst.texrect);
        break;
    case 0xf5: // RDP_SETTILE
        {
            int ti=FIELD(cmd[1],24,3);
            Tile *t=rst.tile+ti;
            t->fmt     =FIELD(cmd[0],21,3);
            t->bpp     =FIELD(cmd[0],19,2);
            t->tmemrl  =8*FIELD(cmd[0],9,9);
            t->tmembase=8*FIELD(cmd[0],0,9);
            t->palette =FIELD(cmd[1],20,4);
            t->cmt     =FIELD(cmd[1],18,2);
            t->maskt   =FIELD(cmd[1],14,4);
            t->shiftt  =FIELD(cmd[1],10,4);
            t->cms     =FIELD(cmd[1],8,2);
            t->masks   =FIELD(cmd[1],4,4);
            t->shifts  =FIELD(cmd[1],0,4);
            t->settilemark=0;

            /*
            if(cart.iszelda && (rst.lastloadb&0xffff0000)==0x802b0000)
            {
                if(rst.lastloadb==0x802b1df0 ||
                   rst.lastloadb==0x802b21d0 ||
                   rst.lastloadb==0x802b23d0 ||
                   rst.lastloadb==0x802b25f0 ||
                   rst.lastloadb==0x802b29d0 ||
                   rst.lastloadb==0x802b2bd0)
                {
                    if(t->fmt==4 && t->bpp==1) t->bpp=0;
                }
            }
            */

            // force clamps
            if(!t->maskt && t->cmt==0) t->cmt=2;
            if(!t->masks && t->cms==0) t->cms=2;
            t->cmsset=t->cms;
            t->cmtset=t->cmt;
            // a SETTILE alone can change a drawn tile's wrap/mirror or
            // palette (All-Star Baseball 2001's grass switches tile 0
            // between MIRROR and WRAP on the same TMEM load)
            if(ti!=7) rst.modechange=rst.txtchange=1;
            //
            if(st.dumpgfx)
            {
                logd("\n+tile %i settile fmt=%s bpp=%s tmemrl=%i tmembase=%i pal=%i"
                     "\n+tile   cmt=%s maskt=%i shiftt=%i cms=%s masks=%i shifts=%i",
                    ti,
                    fmt[t->fmt],
                    bpp[t->bpp],
                    t->tmemrl,
                    t->tmembase,
                    t->palette,
                    cm[t->cmt],
                    t->maskt,
                    t->shiftt,
                    cm[t->cms],
                    t->masks,
                    t->shifts);
            }
        }
        break;
    case 0xf4: // RDP_LOADTILE
        {
            int i,x0,y0,x1,y1;
            int ti=FIELD(cmd[1],24,3);
            x0=FIELD(cmd[0],12,12)>>2;
            y0=FIELD(cmd[0],0,12) >>2;
            x1=FIELD(cmd[1],12,12)>>2;
            y1=FIELD(cmd[1],0,12) >>2;
            if(st.dumpgfx)
            {
                logd("\n+tile   loadtile (%08X->%04X) rl=%i (%i,%i)-(%i,%i)",
                    rst.bufbase[RDP_BUF_TXT],
                    rst.tile[ti].tmembase,
                    rst.bufwid[RDP_BUF_TXT],
                    x0,y0,x1,y1);
            }
            i=(rst.tile[ti].tmembase>>3)&511;
            rst.tmemsrc[i]=rst.bufbase[RDP_BUF_TXT];
            rst.tmemrl [i]=(rst.bufwid[RDP_BUF_TXT]<<rst.bufbpp[RDP_BUF_TXT])>>1;
            rst.tmemx0 [i]=(x0<<rst.bufbpp[RDP_BUF_TXT])>>1;
            rst.tmemy0 [i]=y0;
            tmem_loaded(i,(y1-y0+1)*rst.tile[ti].tmemrl,rst.tile[ti].tmemrl);
            rst.modechange=rst.txtchange=1;
            rst.lastloadb=0;
        }
        break;
    case 0xf3: // RDP_LOADBLOCK
        {
            int i,j,a,s;
            int ti=FIELD(cmd[1],24,3);
            int xx1=FIELD(cmd[1],12,12);
            int dxt=FIELD(cmd[1],0,12);
            // uls/ult are whole texels here (not 10.2 as in LOADTILE): the
            // block starts ult rows and uls texels into the texture image,
            // as GLideN64 gDPLoadBlock. F-Zero X loads text in row bands.
            int uls=FIELD(cmd[0],12,12);
            int ult=FIELD(cmd[0],0,12);
            dword start=rst.bufbase[RDP_BUF_TXT]+
                ((ult*rst.bufwid[RDP_BUF_TXT]+uls)<<rst.bufbpp[RDP_BUF_TXT]>>1);

            if(st.dumpgfx)
            {
                logd("\n+tile   loadblock (%08X->%04X) xxx=%i dxt=%i uls=%i ult=%i",
                    start,rst.tile[ti].tmembase,xx1,dxt,uls,ult);
            }
            i=(rst.tile[ti].tmembase>>3)&511;

            s=((xx1+1)<<rst.tile[ti].bpp)>>1;
            if(s==4096)
            {
                j=2;
            }
            else
            {
                j=1;
            }

            // set locations at START and MIDDLE of loaded block only
            // quake loads 2 similar size textures at once, other games
            // only load one texture at the time
            tmem_loaded(i,s,0);
            a=start;
            for(;;)
            {
                rst.tmemsrc[i]=a;
                rst.tmemx0 [i]=0;
                rst.tmemy0 [i]=0;
                if(!rst.tmemlen[i]) rst.tmemlen[i]=s/2; // the 4K block's middle

                if(dxt)
                {
                    // the data goes into TMEM as it is in RDRAM and the RDP
                    // reads rows at the tile's line (txt_rl). The row length
                    // from dxt is only a fallback: 79 and 80 words both give
                    // dxt 26, and Buck Bumble's 320 texel rows (80 words)
                    // came out 632 bytes long, every other row 4 texels off.
                    // Negative: a LOADBLOCK, not a LOADTILE image width.
                    int a;
                    a=(8*2048/dxt+6)&~7; // see util\koe2.c
                    rst.tmemrl[i]=-a;
                }
                else
                {
                    rst.tmemrl[i]=-1;
                }

                if(!--j) break;

                i+=s>>4;
                a+=s>>4;

                if(i>=511) break;
            }

            rst.modechange=rst.txtchange=1;
            rst.lastloadb=start;
        }
        break;
    case 0xf2: // RDP_SETTILESIZE
        {
            int ti=FIELD(cmd[1],24,3);
            int xs,ys;
            Tile *t=rst.tile+ti;

            t->x0full=FIELD(cmd[0],12,12);
            t->y0full=FIELD(cmd[0],0,12);
            // An upper-left past the lower-right is negative (the RDP's
            // s-sl wraps): Conker's eye tile starts at 0xFFE, half a texel
            // left of 0, and read as 1023.5 its width was -991 and it was
            // drawn untextured. The shift stays in x0full, the size starts
            // at texel 0.
            if(t->x0full>(int)FIELD(cmd[1],12,12))
            {
                t->x0full-=4096;
                cmd[0]&=~(0xfff<<12);
            }
            if(t->y0full>(int)FIELD(cmd[1],0,12))
            {
                t->y0full-=4096;
                cmd[0]&=~0xfff;
            }

            if(t->settilemark)
            {
                // already set, this is probably a offset call (x0y0 change);
                // but a masked tile drawn again at another size repeats and
                // clamps by that size (Smash's castle roofs draw one texture
                // as a 64x96 and a 32x160 tile)
                if(!(t->masks || t->maskt) ||
                   (FIELD(cmd[1],14,10)-FIELD(cmd[0],14,10)==t->x1-t->x0 &&
                    FIELD(cmd[1],2,10) -FIELD(cmd[0],2,10) ==t->y1-t->y0))
                    break;
            }
            t->settilemark=1;

            txt_masksize(t,cmd);

            xs=FIELD(cmd[1],14,10)-FIELD(cmd[0],14,10);
            ys=FIELD(cmd[1],2,10) -FIELD(cmd[0],2,10) ;
            if(!xs) xs=1;
            if(!ys) ys=1;
            if(xs*ys>=16384 && t->tmemrl>0 && t->tmembase<4096)
            {
                // tile taller than TMEM (a font strip wider than a load,
                // e.g. Beetle Adventure Racing): only the rows that fit
                // in TMEM exist, so clamp the height to those
                int rows=(4096-t->tmembase)/t->tmemrl;
                if(rows<1) rows=1;
                if(ys>=rows)
                {
                    ys=rows-1;
                    cmd[1]=(cmd[1]&~0xfff)|((FIELD(cmd[0],2,10)+ys)<<2);
                }
            }
            if(xs*ys>=16384)
            {
                // can't be right!?
                // keep the scrolling part, but
                // ignore new size
                logd("\n+tile %i settilesize (%i,%i)-(%i,%i) error! ",
                    ti,
                    FIELD(cmd[0],14,10),
                    FIELD(cmd[0],2,10),
                    FIELD(cmd[1],14,10),
                    FIELD(cmd[1],2,10));
                break;
            }

            t->x0=FIELD(cmd[0],14,10);
            t->y0=FIELD(cmd[0],2,10);
            t->x1=FIELD(cmd[1],14,10);
            t->y1=FIELD(cmd[1],2,10);
            // the texel at (ul,ul) is the first one in TMEM, also after a
            // LOADBLOCK (texture coordinates already subtract x0full/y0full)
            t->xs=t->x1-t->x0+1;
            t->ys=t->y1-t->y0+1;

            if(ti!=7)
            {
                rst.lastusedtile=ti;
                t->membase=rst.bufbase[RDP_BUF_TXT];
                t->memfmt =rst.buffmt[RDP_BUF_TXT];
                t->membpp =rst.bufbpp[RDP_BUF_TXT];
                rst.modechange=rst.txtchange=1;
            }
            if(st.dumpgfx)
            {
                logd("\n+tile %i settilesize (%i,%i)-(%i,%i) sz(%i,%i) base %08X",
                    ti,
                    t->x0,t->y0,
                    t->x1,t->y1,
                    t->xs,t->ys,
                    t->membase);
            }
        }
        break;
    case 0xf1: // RDP_RDPHALF_RDP
        logd("!skiprdp ");
        break;
    case 0xf0: // RDP_LOADTLUT
        {
            int ti=FIELD(cmd[1],24,3);
            int t,siz,base,pal,start;
            t=FIELD(cmd[1],24,3);
            siz=FIELD(cmd[1],14,10)+1; // texels
            start=FIELD(cmd[1],2,10)+1;
            pal=(rst.tile[ti].tmembase-2048)/(16*8);
            if(siz>256) rst.geyemode=1;
            if(rst.geyemode) siz/=4;
            base=rst.bufbase[RDP_BUF_TXT];

            if(start!=0)
            {
                int x0,y0,x1,y1;
                x0=FIELD(cmd[0],12,12)>>2;
                y0=FIELD(cmd[0],0,12) >>2;
                x1=FIELD(cmd[1],12,12)>>2;
                y1=FIELD(cmd[1],0,12) >>2;
                if(st.dumpgfx)
                {
                    logd("\n+tile loadtlut range (%i,%i)-(%i,%i)",
                        x0,y0,x1,y1);
                }
                base+=2*y0+2*x0;
                siz=(x1-x0)+1;
                if(siz>256) siz=256;
            }
            logd("\n+tile loadtlut buffer base %08X pal=%i size=%i start=%i",
                rst.bufbase[RDP_BUF_TXT],pal,siz);

            rst.tlut_base=base;
            rst.tlut_palbase=pal;
            rst.tlut_num=siz;
            // into the palette now, as the RDP copies it into TMEM: texture
            // conversion reloads only the last LOADTLUT, and Castlevania
            // loads palette 0 (text colors) and 1 (mask) in turn: palette 0
            // kept stale entries and the text background came out opaque
            if(pal>=0 && pal<16) txt_loadtlut(pal,siz,base);
        }
        break;
    case 0xef: // RDP_RDPSETOTHERMODE
        logd("OtherSet %08X %08X",cmd[0],cmd[1]);
        rst.other0[0]=cmd[0];
        rst.other1[0]=cmd[1];
        rst.modechange=1;
        break;
    case 0xee: // RDP_SETPRIMDEPTH: used by raw RDP lists (HLE depth is w)
        rst.primz=(float)((cmd[1]>>16)&0x7fff)/32768.0f;
        logd("primdepth %.4f",rst.primz);
        break;
    case 0xed: // RDP_SETSCISSOR: 10.2 screen coordinates, lower right exclusive
        {
            float xs=320,ys=240;
            if(init.viewportwid)
            {
                xs=init.viewportwid;
                if(init.viewporthig) ys=init.viewporthig;
            }
            flushprims();
            x_scissor(1,
                0.25f*FIELD(cmd[0],12,12)*init.gfxwid/xs,
                0.25f*FIELD(cmd[0], 0,12)*init.gfxhig/ys,
                0.25f*FIELD(cmd[1],12,12)*init.gfxwid/xs,
                0.25f*FIELD(cmd[1], 0,12)*init.gfxhig/ys);
            logd("scissor (%i,%i)-(%i,%i)",FIELD(cmd[0],12,12)/4,FIELD(cmd[0],0,12)/4,
                FIELD(cmd[1],12,12)/4,FIELD(cmd[1],0,12)/4);
            if(rst.rtt_on && (int)FIELD(cmd[1],0,12)/4>rst.rtt_maxy)
                rst.rtt_maxy=FIELD(cmd[1],0,12)/4; // rows to write back
            // the current color buffer is at least this high (cbufsource)
            rst.scissorhig=(FIELD(cmd[1],0,12)+3)/4;
            if(rst.lastcbufs[rst.lastcbufi]==rst.bufbase[RDP_BUF_C] &&
               rst.scissorhig>rst.lastcbufhig[rst.lastcbufi])
                rst.lastcbufhig[rst.lastcbufi]=rst.scissorhig;
        }
        break;
    // Known gap: the OpenGL combiner has no key center/scale inputs, no
    // chroma key and no YUV conversion, so these are ignored here
    // (rdp_soft.c feeds key to its combiner). K4/K5 are combiner inputs:
    // Conker's mouth is (SHADE-ENV)*K5+PRIM with K5=255, black without it.
    case 0xec: // RDP_SETCONVERT
        rst.k4=FIELD(cmd[1],9,9)/255.0f;
        rst.k5=FIELD(cmd[1],0,9)/255.0f;
        rst.modechange=2;
        logd(" K4 %.2f K5 %.2f (YUV conversion not emulated)",rst.k4,rst.k5);
        break;
    case 0xeb: // RDP_SETKEYR
        logd("!skiprdp key r (not emulated)");
        break;
    case 0xea: // RDP_SETKEYGB
        logd("!skiprdp key gb (not emulated)");
        break;
    case 0xe9: // RDP_RDPFULLSYNC
        st2.gfxdpsyncpending=1;
        break;
    case 0xe8: // RDP_RDPTILESYNC
    case 0xe6: // RDP_RDPLOADSYNC
    case 0xe7: // RDP_RDPPIPESYNC
        rst.modechange=1;
        break;
    case 0xe5: // RDP_TEXRECTFLIP
        rst.texrect.flip=1;
        rst.texrect.x0=0.25*FIELD(cmd[0],12,12);
        rst.texrect.y0=0.25*FIELD(cmd[0],0,12);
        rst.texrect.x1=0.25*FIELD(cmd[1],12,12);
        rst.texrect.y1=0.25*FIELD(cmd[1],0,12);
        rst.texrect.tile=FIELD(cmd[1],24,3);
        settexturetile(rst.texrect.tile); // its own tile, not G_TEXTURE's
        rst.wordsleft=2;
        rst.wordcmd=0xe5;
        if(st.dumpgfx)
        {
            logd("\n# texrectflip (%f,%f)-(%f,%f) ...",
                rst.texrect.x0,
                rst.texrect.y0,
                rst.texrect.x1,
                rst.texrect.y1);
        }
        return(2); // need 2 more commands
    case 0xe4: // RDP_TEXRECT
        // 2 bits of sub in these
        rst.texrect.flip=0;
        rst.texrect.x0=0.25*FIELD(cmd[0],12,12);
        rst.texrect.y0=0.25*FIELD(cmd[0],0,12);
        rst.texrect.x1=0.25*FIELD(cmd[1],12,12);
        rst.texrect.y1=0.25*FIELD(cmd[1],0,12);
        rst.texrect.tile=FIELD(cmd[1],24,3);
        settexturetile(rst.texrect.tile); // its own tile, not G_TEXTURE's
        rst.wordsleft=2;
        rst.wordcmd=0xe4;
        if(st.dumpgfx)
        {
            logd("\n# texrect (%f,%f)-(%f,%f) ...",
                rst.texrect.x0,
                rst.texrect.y0,
                rst.texrect.x1,
                rst.texrect.y1);
        }
        return(2); // need 2 more commands
    default:
        error("rdp: unknown command %02X!",c);
        return(-1); // unknown command
    }

//    logd("&mc%i",rst.modechange);

    return(0); // no more data needed
}

void rdp_opendisplay(void)
{
    if(rst.opened) return;
    rst.opened=1;

    logd("Opendisplay.\n");
    logd(NULL);
    opendisplay();
}

void rdp_closedisplay(void)
{
    if(!rst.opened) return;
    rst.opened=0;

    logd("Closedisplay.\n");
    logd(NULL);
    closedisplay();
}

void rdp_framestart(void)
{
    int i;

    if(rst.frameopen) return;
    rst.frameopen=1;

    rst.firstfillrect=1;

    rdp_opendisplay();
    x_scissor(0,0,0,0,0); // until the display list sets one

    rdp_viewport(init.gfxwid/2,init.gfxhig/2,init.gfxwid/2,init.gfxhig/2);

    rst.testcnt=0;

    for(i=0;i<MAXRDPVX;i++) { rdpvx[i]=rdpdummyvx+i; rdpdummyvx[i].zs=-1.0f; }

    if(rst.myframe<=0) rst.myframe=1;

    rst.view_x0=0;
    rst.view_y0=0;
    rst.view_x1=init.gfxwid-1;
    rst.view_y1=init.gfxhig-1;

    rst.txtloads=0;

    rst.fillrectcnt=0;

    rst.lastalphatst=-1;
    rst.lastzmode=-1;

    rst.nexttexturetile=rst.texturetile=rst.tritile=0;

    rst.starttimeus=timer_us(&st2.timer);

    rst.fogmin=rst.fogmax=0.0;
    rdp_fogrange(0.0,0.0);

    if(!(rst.myframe&15))
    {
        // clear combine cache every now and then
        rst.combcacheused=0;
    }

    // clear cache entries not used for 3 frames
    for(i=0;i<MAXTXT;i++)
    {
        if(rst.txt[rst.txtrefreshcnt].used_vidframe>rst.myframe-3)
        {
            rst.txt[rst.txtrefreshcnt].membase=0;
        }
    }

    if(st.dumpgfx)
    {
        // clear texture cache
        logd("Dump mode, clearing texture cache.\n");
        rdp_freetexmem();
    }

    com_init();

    clearprims();

    rst.tlut_lastbase=0xffffffff;

    rst.modechange=2; // full mode set
    rst.firstmodechange=1;

    if(showinfo) rst.debugwirecolor=rand();
    else rst.debugwirecolor=0;

    rst.tris=0;
}

void rdp_frameend(void)
{
    if(!rst.frameopen) return;
    rst.frameopen=0;

    st2.gfxdpsyncpending=1;
    flushprims();
    rtt_end(); // an offscreen buffer still open: write it back
    // debuginfo
    if(1)
    {
        // draw debug wireframe
        if(showinfo || showwire)
        {
            wireprims();
        }
        if(showinfo)
        {
            drawtextures();
        }

        /*
        // marker dots and misc stuff
        if(showinfo)
        {
            x_flush();
            debugdrawmode();
            x_flush();
            drawmarkers();
            x_flush();
            drawtextures();
            x_flush();
        }
        */
    }

    // show buffer on screen when the game asks the VI to show it
    // (rdp_viorigin, between tasks like Glide64's swap on VI), so frames
    // drawn as several tasks appear whole
    if(rst.tris>0)
    {
        dword cbuf=rst.bufbase[RDP_BUF_C];
        if(!(rst.presentpending && rst.pendingcbuf==cbuf))
        {
            rdp_present(); // another buffer's frame was never asked for
            rst.pendingage=0;
        }
        rst.presentpending  =1;
        rst.pendingcbuf     =cbuf;
        rst.pendingcbufwid  =rst.bufwid[RDP_BUF_C];
        rst.pendingcbufbytes=(1<<rst.bufbpp[RDP_BUF_C])>>1;
        if(rst.pendingcbufbytes<1) rst.pendingcbufbytes=1;
        rst.pendingcbufhig  =rst.lastcbufs[rst.lastcbufi]==cbuf?rst.lastcbufhig[rst.lastcbufi]:0;
        // a separate gfx thread owns the GL context: present right away
        if(st.gfxthread) rdp_present();
    }

    if(st.dumpinfo)
    {
        print("Frame%6i: %i modes, %i newtext, %i text, %i vtx, %i prim\n",
            st.frames,
            rst.cnt_setting,
            rst.cnt_texturegen,
            rst.cnt_texture,
            rst.vxtabi,
            rst.prtabi);
    }

    rst.cnt_setting=0;
    rst.cnt_texture=0;
    rst.cnt_texturegen=0;

    st.ops_fast=0;
    st.ops_slow=0;

    logd("\nSwap - Cputime:%08X <{([+*#*+])}>\n\n",(dword)st.cputime);

    rst.myframe++;
}

void rdp_swap(void)
{
    rst.swapflag=1;
}

// The game asks the VI to show the frame at origin (VI_ORIGIN in LLE mode,
// osViSwapBuffer in HLE mode): the frame drawn there is complete. Called
// between display lists on the emulation thread, which owns the GL context.
void rdp_viorigin(dword origin)
{    // the origin can skip a few lines into the buffer
    if(rst.presentpending &&
       (origin&0x1fffffff)-(rst.pendingcbuf&0x1fffffff)<0x2000)
        rdp_present();
}

/****************************************************************************
** Raw RDP command lists (DPC_START..DPC_END), sent by the game's own RSP
** microcode (libdragon's rdpq) instead of a display list UltraHLE knows.
** State and rectangle commands have the display-list encoding and go to
** rdp_cmd; triangles arrive as edge coefficients and become 2D polygons.
*/

// attribute n (0..3) of a shade or texture coefficient block: integer
// halves at words 0-1, fractions at 4-5; d/dx at 2-3,6-7; d/de at 8-9,12-13
static double rawcoef(const dword *g,int word,int n)
{
    int w=word+(n>>1),hi=!(n&1);
    int i=hi?(short)(g[w]>>16):(short)(g[w]&0xffff);
    int f=hi?(g[w+4]>>16):(g[w+4]&0xffff);
    return((double)(int)(((dword)i<<16)|(dword)f)/65536.0);
}

static int rawsext(dword v,int bits)
{
    int s=32-bits;
    return(((int)(v<<s))>>s);
}

// A triangle as the RDP gets it: major edge H from YH to YL, minor edges M
// (YH..YM) and L (YM..YL); XH/XM hold at YH's scanline, XL at YM. Shade and
// texture values start at XH on that scanline and change by d/de down the
// major edge and d/dx across. The corners where the edges meet make a
// polygon (a triangle, or up to 5 points for a trapezoid), drawn as a fan.
static void rawtri(const dword *w)
{
    int    op=(w[0]>>24)&0x3f;
    int    tile=(w[0]>>16)&7;
    double yl=rawsext(w[0],14)/4.0;
    double ym=rawsext(w[1]>>16,14)/4.0;
    double yh=rawsext(w[1],14)/4.0;
    double xl=(int)w[2]/65536.0,dxl=(int)w[3]/65536.0;
    double xh=(int)w[4]/65536.0,dxh=(int)w[5]/65536.0;
    double xm=(int)w[6]/65536.0,dxm=(int)w[7]/65536.0;
    double ys=floor(yh);
    const dword *sh=NULL,*tx=NULL,*zb=NULL;
    double pt[6][2];
    int    n=0,i,k=8;
    int    persp=(rst.other0[0]>>19)&1;
    int    zprim=(rst.other1[0]>>2)&1; // z source: SET_PRIM_DEPTH
    Vertex *vx[6];

    if(op&4) { sh=w+k; k+=16; }
    if(op&2) { tx=w+k; k+=16; }
    if(op&1) { zb=w+k; k+=4; }  // z, dz/dx, dz/de, dz/dy (s15.16)
    if(yl<=yh) return;

    // corners, clockwise from the top: major edge top and bottom, minor
    // edge top, middle and bottom
    // The minor side is edge M from YH to YM, then edge L, which starts
    // from its own XL at YM: the two needn't meet (near 90 degrees rdpq
    // gives an edge a quarter scanline tall with a slope of 18, and the M
    // end was 13 pixels from XL, cutting off half of Flappy Bird's bird).
    // A flat-topped triangle has no M part, a flat-bottomed one no L part.
    pt[n][0]=xh+dxh*(yh-ys); pt[n][1]=yh; n++;
    if(ym>yh)
    {
        double ye=(ym<yl)?ym:yl;
        pt[n][0]=xm+dxm*(yh-ys); pt[n][1]=yh; n++;
        pt[n][0]=xm+dxm*(ye-ys); pt[n][1]=ye; n++;
    }
    if(ym<yl)
    {
        double yb=(ym>yh)?ym:yh;
        pt[n][0]=xl+dxl*(yb-ym); pt[n][1]=yb; n++;
        pt[n][0]=xl+dxl*(yl-ym); pt[n][1]=yl; n++;
    }
    pt[n][0]=xh+dxh*(yl-ys); pt[n][1]=yl; n++;

    if(st.dumpgfx)
    {
        logd("\n# rawtri %02X lft=%i y %.2f %.2f %.2f xh %.3f/%.3f xm %.3f/%.3f xl %.3f/%.3f:",
            op,(w[0]>>23)&1,yh,ym,yl,xh,dxh,xm,dxm,xl,dxl);
        for(i=0;i<n;i++) logd(" (%.2f,%.2f)",pt[i][0],pt[i][1]);
    }

    if(tx) rdp_texture(1,tile,0);
    rst.tris++;
    // full screen as rectangles, but not rectangle mode 1: that has no
    // shade (drawprims_n64), and Rogue Squadron's TEXEL0*SHADE went black
    viewport(2);
    if(rst.modechange) newmode();

    // Perspective: the RDP interpolates s*w, t*w and w linearly on screen
    // and divides per pixel. GL does the same with clip w = 1/w (w is 1.0
    // at 0x7fff) and the divided s,t per vertex; dividing at the corners
    // and drawing with w=1 made the texture affine between them. Depth is
    // the RDP's own z (or the primitive's), passed apart from that w.
    for(i=0;i<n;i++)
    {
        double x=pt[i][0],y=pt[i][1];
        double dy=y-ys,dx=x-(xh+dxh*dy);
        double cw=1.0; // clip w
        Vertex *v=vx[i]=newvx();
        int c;
        if(tx && persp)
        {
            double ww=rawcoef(tx,0,2)+rawcoef(tx,8,2)*dy+rawcoef(tx,2,2)*dx;
            if(ww>0.0001) cw=32768.0/ww;
        }
        v->pos[0]=(float)((x*oxm+oxa)*cw);
        v->pos[1]=(float)((y*oym+oya)*cw);
        v->pos[2]=(float)cw;
        if(zprim) v->zs=rst.primz;
        else if(zb)
        {
            double z=((int)zb[0]+(int)zb[2]*dy+(int)zb[1]*dx)/65536.0/32768.0;
            v->zs=(float)(z<0?0:(z>1?1:z));
        }
        for(c=0;c<4;c++)
        {
            double a=sh?rawcoef(sh,0,c)+rawcoef(sh,8,c)*dy+rawcoef(sh,2,c)*dx:0.0;
            if(a<0) a=0;
            if(a>255) a=255;
            v->col[c]=(float)(a*(1.0/256.0));
        }
        if(tx)
        {
            double s=rawcoef(tx,0,0)+rawcoef(tx,8,0)*dy+rawcoef(tx,2,0)*dx;
            double t=rawcoef(tx,0,1)+rawcoef(tx,8,1)*dy+rawcoef(tx,2,1)*dx;
            if(persp) { s*=cw; t*=cw; } // s/w, t/w at this corner
            v->tex[0]=(float)s; // s10.5, as display-list vertices
            v->tex[1]=(float)t;
        }
        else v->tex[0]=v->tex[1]=0;
    }
    for(i=1;i+1<n;i++)
    {
        Primitive *p=newpr();
        p->wirecolor=15;
        p->c[0]=vx[0];
        p->c[1]=vx[i];
        p->c[2]=vx[i+1];
        st2.gfx_tris++;
    }
}

void rdp_rawcmd(const dword *w,int words)
{
    dword cmd[2];
    int   op=(w[0]>>24)&0x3f;

    if(st.graphicsenable<=0) return;
    if(!rst.frameopen) rdp_framestart();

    // the RDP ignores the top two bits; display lists have them set
    cmd[0]=(w[0]&0x3fffffff)|0xc0000000;
    cmd[1]=w[1];
    switch(op)
    {
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        rawtri(w);
        break;
    case 0x24: case 0x25: // texture rectangle: 128 bits
        if(words<4) break;
        rst.rectzon=(rst.other1[0]>>2)&1; // primitive depth
        rdp_texture(1,(w[1]>>24)&7,0);
        rdp_cmd(cmd);
        cmd[0]=0; cmd[1]=w[2]; rdp_cmd(cmd); // s,t
        cmd[0]=0; cmd[1]=w[3]; rdp_cmd(cmd); // dsdx,dtdy
        rst.rectzon=0;
        break;
    case 0x36: // fill rectangle
        rst.rectzon=(rst.other1[0]>>2)&1;
        rdp_cmd(cmd);
        rst.rectzon=0;
        break;
    case 0x29: // FULL_SYNC: the frame is done
        rdp_frameend();
        break;
    case 0x26: case 0x27: case 0x28: // syncs
        rdp_cmd(cmd);
        break;
    default:
        if(op>=0x2a) rdp_cmd(cmd); // 0x00 no-op and the rest are ignored
        break;
    }
}

// Show the framebuffer the VI scans out (origin, width in pixels, bytes per
// pixel 2 or 4), for pictures drawn in RDRAM by the software RDP
// (rdp_soft.c). Called per retrace on the emulation thread.
void rdp_showvi(dword origin,int width,int height,int bpp)
{
    static int   handle,texw,texh;
    static byte *buf;
    int x,y;

    if(width<=0 || height<=0 || width>1024 || height>1024) return;
    if(!rst.opened) rdp_opendisplay();
    if(!rst.fullscreen) return;
    if(width!=texw || height!=texh)
    {
        if(handle>0) x_freetexture(handle);
        handle=x_createtexture(X_RGBA8888|X_CLAMP|X_NOBILIN,width,height);
        free(buf);
        buf=(byte *)malloc(width*height*4);
        texw=width;
        texh=height;
    }
    if(handle<=0 || !buf) return;

    origin&=0xffffff;
    for(y=0;y<height;y++)
    {
        byte *d=buf+y*width*4;
        for(x=0;x<width;x++,d+=4)
        {
            dword a=origin+(dword)(y*width+x)*bpp;
            if(a+bpp>(dword)mem.ramsize) { d[0]=d[1]=d[2]=0; d[3]=255; continue; }
            if(bpp==4)
            {
                dword c=*(dword *)(mem.ram+a);
                d[0]=(byte)(c>>24); d[1]=(byte)(c>>16); d[2]=(byte)(c>>8);
            }
            else
            {
                int c=*(word *)(mem.ram+(a^2));
                d[0]=(byte)(((c>>11)&31)*255/31);
                d[1]=(byte)(((c>>6)&31)*255/31);
                d[2]=(byte)(((c>>1)&31)*255/31);
            }
            d[3]=255;
        }
    }
    x_loadtexturelevel(handle,0,(char *)buf);

    debugdrawmode();
    x_mask(X_ENABLE,X_DISABLE,X_DISABLE);
    x_combine(X_TEXTURE);
    x_blend(X_ONE,X_ZERO);
    x_texture(handle);
    x_begin(X_QUADS);
    x_vxtex(0,0); x_vxpos(-1.0f,+1.0f,1.0f);
    x_vxtex(0,1); x_vxpos(-1.0f,-1.0f,1.0f);
    x_vxtex(1,1); x_vxpos(+1.0f,-1.0f,1.0f);
    x_vxtex(1,0); x_vxpos(+1.0f,+1.0f,1.0f);
    x_end();
    realdrawmode();
    rst.modechange=2;
    newmode();

    rst.presentpending=1;
    rdp_present();
}

// per retrace: a frame no VI origin ever matched is shown anyway
void rdp_retrace(void)
{
    if(rst.presentpending && ++rst.pendingage>=3) rdp_present();
}

void rdp_copybackground(dword base,int wid,int hig)
{
    static int    xhandle[2];
    static char   buf[256*256][4];
    static dword  lastcrc;
    int i,x,y,t;
    float x0,y0,x1,y1;
    dword d,crc;

    if(!xhandle[0])
    {
        xhandle[0]=x_createtexture(X_RGBA5551|X_CLAMP,256,256);
        xhandle[1]=x_createtexture(X_RGBA5551|X_CLAMP,256,256);
    }
    crc=0;
    for(i=0;i<320*240*2;i+=9035)
    {
        crc^=mem_read32p(i+base);
        crc =(crc<<7)|(crc>>(32-7));
    }
    if(crc!=lastcrc)
    {
        lastcrc=crc;
        // update txt
        for(t=0;t<2;t++)
        {
            for(y=0;y<240;y++)
            {
                for(x=0;x<256;x++)
                {
                    d=base+(t*256+x)*2+y*wid*2;
                    d=mem_read16(d);
                    buf[x+y*256][0]=FIELD(d,11,5)*8;
                    buf[x+y*256][1]=FIELD(d, 6,5)*8;
                    buf[x+y*256][2]=FIELD(d, 1,5)*8;
                    buf[x+y*256][3]=255;
                }
            }
            x_loadtexturelevel(xhandle[t],0,(char *)buf);
        }
    }

    // draw
    x_mask(X_ENABLE,X_DISABLE,X_DISABLE);
    x_combine(X_TEXTURE);
    x_blend(X_ONE,X_ZERO);
    x_texture(xhandle[0]);

    x0=0.0;
    y0=0.0;
    x1=256.0;
    y1=256.0;
    x_begin(X_QUADS);
    x_vxtex(0,0); x_vxpos(x0*oxm+oxa,y0*oym+oya,1.0);
    x_vxtex(0,1); x_vxpos(x0*oxm+oxa,y1*oym+oya,1.0);
    x_vxtex(1,1); x_vxpos(x1*oxm+oxa,y1*oym+oya,1.0);
    x_vxtex(1,0); x_vxpos(x1*oxm+oxa,y0*oym+oya,1.0);
    x_end();

    x0=256.0;
    y0=0.0;
    x1=512.0;
    y1=256.0;
    x_texture(xhandle[1]);
    x_begin(X_QUADS);
    x_vxtex(0,0); x_vxpos(x0*oxm+oxa,y0*oym+oya,1.0);
    x_vxtex(0,1); x_vxpos(x0*oxm+oxa,y1*oym+oya,1.0);
    x_vxtex(1,1); x_vxpos(x1*oxm+oxa,y1*oym+oya,1.0);
    x_vxtex(1,0); x_vxpos(x1*oxm+oxa,y0*oym+oya,1.0);
    x_end();

    realdrawmode();
    rst.modechange=2;
    newmode();
}

// one texel of an S2DEX background, as RGBA8 (I/IA: alpha as the RDP reads
// it, I texels have alpha = intensity); 0 for an unsupported format
static int bg_texel(byte *d,dword base,int fmt,int siz,int pal,int imgw,int x,int y)
{
    dword c;
    int   i=y*imgw+x;
    switch(fmt*4+siz)
    {
    case 2*4+1: // CI8
        txt_paletteread(d,mem_read8(base+i));
        return(1);
    case 2*4+0: // CI4
        c=mem_read8(base+i/2);
        txt_paletteread(d,pal*16+((i&1)?(c&15):(c>>4)));
        return(1);
    case 0*4+2: // RGBA16
        c=mem_read16(base+i*2);
        d[0]=FIELD(c,11,5)*8+(FIELD(c,11,5)>>2);
        d[1]=FIELD(c, 6,5)*8+(FIELD(c, 6,5)>>2);
        d[2]=FIELD(c, 1,5)*8+(FIELD(c, 1,5)>>2);
        d[3]=(c&1)?255:0;
        return(1);
    case 0*4+3: // RGBA32
        c=mem_read32p(base+i*4);
        d[0]=c>>24; d[1]=c>>16; d[2]=c>>8; d[3]=c;
        return(1);
    case 4*4+0: // I4
        c=mem_read8(base+i/2);
        c=((i&1)?(c&15):(c>>4))*17;
        d[0]=d[1]=d[2]=d[3]=c;
        return(1);
    case 4*4+1: // I8
        d[0]=d[1]=d[2]=d[3]=mem_read8(base+i);
        return(1);
    case 3*4+0: // IA4: 3 bits intensity, 1 bit alpha
        c=mem_read8(base+i/2);
        c=(i&1)?(c&15):(c>>4);
        d[0]=d[1]=d[2]=(c>>1)*255/7;
        d[3]=(c&1)?255:0;
        return(1);
    case 3*4+1: // IA8
        c=mem_read8(base+i);
        d[0]=d[1]=d[2]=(c>>4)*17;
        d[3]=(c&15)*17;
        return(1);
    case 3*4+2: // IA16
        c=mem_read16(base+i*2);
        d[0]=d[1]=d[2]=c>>8;
        d[3]=c;
        return(1);
    }
    return(0);
}

// cycle 1 combiner input (sel from SETCOMBINE, which=0 a,1 b,2 c,3 d) for
// the constants a background has: TEXEL0, PRIM, ENV, 1, 0 (no shade)
static float bg_rgbin(int sel,int which,const float *tex,int ch)
{
    const float *prim=rst.colf[C_PRIM],*env=rst.colf[C_ENV];
    switch(sel)
    {
    case 1: return(tex[ch]);
    case 3: return(prim[ch]);
    case 5: return(env[ch]);
    case 6: return(which==0 || which==3?1.0f:0.0f); // a,d: 1; b,c: key center/scale
    case 8: return(which==2?tex[3]:0.0f);
    case 10: return(which==2?prim[3]:0.0f);
    case 12: return(which==2?env[3]:0.0f);
    }
    return(0.0f);
}

static float bg_alphain(int sel,int which,const float *tex)
{
    switch(sel)
    {
    case 1: return(tex[3]);
    case 3: return(rst.colf[C_PRIM][3]);
    case 5: return(rst.colf[C_ENV][3]);
    case 6: return(which==2?0.0f:1.0f); // c: prim lod fraction
    }
    return(0.0f);
}

// S2DEX background (G_BG_1CYC): an image of imgw x imgh texels in RDRAM,
// shown in the frame rectangle (screen coordinates) starting at texel
// (imgx,imgy), scalew/scaleh texels per screen pixel. CI images go through
// the palette G_OBJ_LOADTXTR loaded. The 1-cycle combiner (a-b)*c+d is
// evaluated per texel with TEXEL0, PRIM and ENV and baked into the texture:
// Kirby 64 fades its logos with PRIM*TEXEL0 and draws an I4 image as a mask
// (colour PRIM, alpha TEXEL0). It is alpha-blended when the render mode's
// blender is IN*A+MEM*(1-A), otherwise opaque. The image wraps round at its
// size when the frame reaches past it.
// copy (G_BG_COPY): copy mode, no combiner or blender: the texel as is,
// transparent ones dropped when alpha compare is on (othermode L bits 0-1).
// flip: bit 0 mirrors s, bit 1 mirrors t (Fast3D Sprite2D ScaleFlip).
static void bg_draw(dword base,int fmt,int siz,int pal,int imgw,int imgh,
                    float imgx,float imgy,float frx,float fry,float frw,float frh,
                    float scalew,float scaleh,int copy,int flip)
{
    static int  xhandle,xw,xh,xwrap;
    static byte *buf;
    int   x,y,ch,blend,wrap;
    float u0,v0,u1,v1,x1,y1;
    dword c0=rst.combine0[0],c1=rst.combine1[0],rm=rst.other1[0];
    int   ra=FIELD(c0,20,4),rb=FIELD(c1,28,4),rc=FIELD(c0,15,5),rd=FIELD(c1,15,3);
    int   aa=FIELD(c0,12,3),ab=FIELD(c1,12,3),ac=FIELD(c0, 9,3),ad=FIELD(c1, 9,3);

    if(imgw<=0 || imgh<=0 || imgw>1024 || imgh>1024) return;

    // A background read from a color buffer: GL renders frames without
    // writing them to RDRAM, so the image there is black (fbwrite=1 does put
    // the last frame back). Majora's Mask's motion blur blends a saved frame
    // over the scene, then copies the frame into it as an OVERWRITE
    // background, which here covered the whole screen in black. Dropped.
    ch=cbufsource(base);
    if(ch && !(ch==1 && cart.fbwrite))
    {
        logd("\n+bgimage %08X from a color buffer, skipped",base);
        return;
    }

    if(rst.modechange) newmode();  // the TLUT type comes from othermode
    flushprims();
    rst.tris++; // a frame with only this drawn is still shown (rdp_frameend)

    // The image wraps round when the frame shows past its edge: Yoshi's
    // Story scrolls its 336x256 level backgrounds from near the bottom right
    // corner. Clamped otherwise, which keeps a picture's edges clean.
    wrap=imgx+frw*scalew>imgw+0.5f || imgy+frh*scaleh>imgh+0.5f;
    if(!xhandle || xw!=imgw || xh!=imgh || xwrap!=wrap)
    {
        if(xhandle) x_freetexture(xhandle);
        xhandle=x_createtexture(wrap?X_RGBA8888:X_RGBA8888|X_CLAMP,imgw,imgh);
        xw=imgw;
        xh=imgh;
        xwrap=wrap;
        free(buf);
        buf=malloc(imgw*imgh*4);
    }
    if(xhandle<=0 || !buf) return;

    for(y=0;y<imgh;y++)
    {
        for(x=0;x<imgw;x++)
        {
            byte *d=buf+(x+y*imgw)*4;
            float tex[4],v;
            if(!bg_texel(d,base,fmt,siz,pal,imgw,x,y))
            {
                static int warned;
                if(!warned++) warning("rdp: S2DEX background format %i/%i not supported",fmt,siz);
                return;
            }
            if(copy) continue;
            for(ch=0;ch<4;ch++) tex[ch]=d[ch]*(1.0f/255.0f);
            for(ch=0;ch<3;ch++)
            {
                v=(bg_rgbin(ra,0,tex,ch)-bg_rgbin(rb,1,tex,ch))*bg_rgbin(rc,2,tex,ch)+bg_rgbin(rd,3,tex,ch);
                d[ch]=(byte)(v<=0.0f?0:v>=1.0f?255:v*255.0f+0.5f);
            }
            v=(bg_alphain(aa,0,tex)-bg_alphain(ab,1,tex))*bg_alphain(ac,2,tex)+bg_alphain(ad,3,tex);
            d[3]=(byte)(v<=0.0f?0:v>=1.0f?255:v*255.0f+0.5f);
        }
    }
    x_loadtexturelevel(xhandle,0,(char *)buf);

    // blender cycle 1 (render mode bits 31..18: P1 A1 M1 B1): IN*A_IN +
    // MEM*(1-A) is alpha blending; anything else is drawn opaque
    if(copy) blend=(rm&3)!=0;
    else blend=(FIELD(rm,30,2)==0 && FIELD(rm,26,2)==0 && FIELD(rm,22,2)==1 && FIELD(rm,18,2)==0);

    u0=imgx/imgw;
    v0=imgy/imgh;
    u1=(imgx+frw*scalew)/imgw;
    v1=(imgy+frh*scaleh)/imgh;
    if(flip&1) { float k=u0; u0=u1; u1=k; }
    if(flip&2) { float k=v0; v0=v1; v1=k; }
    x1=frx+frw;
    y1=fry+frh;
    logd("\n+bgimage %08X fmt %i/%i %ix%i frame (%.1f,%.1f)-(%.1f,%.1f) tex (%.3f,%.3f)-(%.3f,%.3f) comb %08X %08X rm %08X%s%s",
        base,fmt,siz,imgw,imgh,frx,fry,x1,y1,u0,v0,u1,v1,c0,c1,rm,copy?" copy":"",blend?" blend":"");

    viewport(1);
    x_mask(X_ENABLE,X_DISABLE,X_DISABLE);
    x_combine(X_TEXTURE);
    if(blend) x_blend(X_ALPHA,X_INVOTHERALPHA);
    else      x_blend(X_ONE,X_ZERO);
    // alpha compare (othermode L bit 0): texels at or below the blend color's
    // alpha are dropped. Yoshi's Story layers its level backgrounds this way;
    // drawn whole, the front layer hid the ones behind it.
    if(!copy && (rm&1))
        x_alphatest(rst.colf[C_BLEND][3]>0.0f?rst.colf[C_BLEND][3]:0.001f);
    else
        x_alphatest(1.0f);
    x_texture(xhandle);
    x_vxcolor4(1.0f,1.0f,1.0f,1.0f);
    x_begin(X_QUADS);
    x_vxtex(u0,v0); x_vxpos(frx*oxm+oxa,fry*oym+oya,1.0);
    x_vxtex(u0,v1); x_vxpos(frx*oxm+oxa,y1 *oym+oya,1.0);
    x_vxtex(u1,v1); x_vxpos(x1 *oxm+oxa,y1 *oym+oya,1.0);
    x_vxtex(u1,v0); x_vxpos(x1 *oxm+oxa,fry*oym+oya,1.0);
    x_end();

    realdrawmode();
    rst.modechange=2;
    newmode();
}

void rdp_bgimage(dword base,int fmt,int siz,int pal,int imgw,int imgh,
                 float imgx,float imgy,float frx,float fry,float frw,float frh,
                 float scalew,float scaleh)
{
    bg_draw(base,fmt,siz,pal,imgw,imgh,imgx,imgy,frx,fry,frw,frh,scalew,scaleh,0,0);
}

void rdp_bgimagecopy(dword base,int fmt,int siz,int pal,int imgw,int imgh,
                     float imgx,float imgy,float frx,float fry,float frw,float frh)
{
    bg_draw(base,fmt,siz,pal,imgw,imgh,imgx,imgy,frx,fry,frw,frh,1.0f,1.0f,1,0);
}

void rdp_spriteimage(dword base,int fmt,int siz,int imgw,int imgh,
                     float imgx,float frx,float fry,float frw,float frh,
                     float scalew,float scaleh,int flip)
{
    bg_draw(base,fmt,siz,0,imgw,imgh,imgx,0,frx,fry,frw,frh,scalew,scaleh,0,flip);
}

void rdp_flat(int flat)
{
    rst.setflat=flat;
    rst.modechange=1;
}

int  rdp_gfxactive(void)
{
    return(rst.opened&rst.fullscreen);
}

void rdp_togglefullscreen(void)
{
    rst.fullscreen^=1;
    x_fullscreen(rst.fullscreen);
    if(init.shutdownglide)
    {
        if(!rst.fullscreen)
        {
            rdp_closedisplay();
            if(st.graphicsenable>0) st.graphicsenable=-1;
        }
        else
        {
            if(st.graphicsenable<0) st.graphicsenable=1;
        }
    }
    // clear combine cache and texture cache (when enabling display)
    if(rst.fullscreen)
    {
    }
}

