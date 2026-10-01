#include "ultra.h"
#include "x.h"

//#define logd(x)

//#define LOGOPROJ

//#define DUMPVXTRI

#define EXTRAWIRETRIS

//#define SHOWBPP

#define MAXVX      64
#define TEXCACHE   256 // textures in cache
#define CACHEDELAY 16
#define CACHERAND  2  // randomization to delay

#define OVLZ   1.0

typedef struct
{
    dword   memaddr; // memaddress and contents crc for topleftmost pixel was loaded from (updated by createtexture)
    int     tmembase;
    int     tmemrl;  // rowlen
    int     memrl;  //memory rowlen
    int     fmt;
    int     bpp;
    int     xs,ys;
    int     cmt,maskt,shiftt;
    int     cms,masks,shifts;
    int     x0,y0;
    int     x1,y1;
    int     palette;
} Tile;

typedef struct
{
    Tile    tile;
    int     creation_vidframe;
    int     used_vidframe;
    int     debugtilepos;
    int     xhandle;
    int     xs,ys;
    int     cms,cmt;
} Texture;

typedef struct
{
    dword   data[4];
    float   col[4]; // setcolor2 writes 4 (with 3, a G_MW_LIGHTCOL alpha
                    // landed in dir[0]: Wheel of Fortune's lights turned)
    float   dir[4];
    float   xfdir[4];
} Light;

typedef struct
{
    // memory
    dword   segment[16];
    // DKR's DMA microcode uses separate bases and an append cursor.
    dword   dkr_mtxbase,dkr_vtxbase;
    int     dkr_vertexi,dkr_billboard;
    // matrices
    float   mtx[2][16][16];
    float   xform[16]; // active full transform
    int     mtxstackp[2];
    // lights
    int     lightnum;
    Light   light[8];
    float   lightmat[16];
    int     lightnumchanged;
    // F3DEX2 texture generation: the look-at vectors (G_MV_LIGHT slots 0
    // and 1), normalized; lookaton as GLideN64's gSPLookAt enables them
    float   lookat[2][3];
    int     lookaton;
    // fog
    float   fognear;
    float   fogfar;
    float   zmax;
    float   vp[4];          // viewport scale x,y and translation x,y (pixels)
    // misc
    int     projection;     // PROJ_*
    int     lastframehadbackground;
    int     framehadbackground;
    dword   loadrspdata;
    int     s2dex;       // S2DEX microcode running (task ucode or G_LOAD_UCODE)
    int     cbfd;        // Conker's F3DEXBG: F3DEX2 plus TRI4, its own lighting
    int     inlinedl;    // Cruis'n Exotica's patched F3DEX2: D5 runs an inline list
    // Conker's lighting (GLideN64 F3DEX2CBFD): the last light is the ambient,
    // the one before it directional, the others point lights. It scales the
    // vertex colors when the vertices load (g_cbfdlightvx).
    int     cbfdadv;      // "advanced" mode (G_LOAD_UCODE): normals, directional light
    int     cbfdlightnum;
    dword   cbfdnormals;  // per-vertex normals (s8 x,y; z is the vertex flag's low byte)
    float   cbfdmod[20];  // G_MW_COORD_MOD: point-light position = (xyz+mod[8..10])*mod[12..14]
    struct { float col[3],dir[3],pos[3],ca; } cbfdlight[12];
    float   objmtx[8];   // S2DEX uObjMtx: A,B,C,D, X,Y, BaseScaleX,Y
    dword   objstatus[4];// S2DEX status words (G_MW_GENSTAT, LOADTXTR, SELECT_DL)
    dword   selecthalf[2];// S2DEX RDPHALF_0 before G_SELECT_DL
    dword   sprite2d;    // Fast3D Sprite2D: uSprite of the last SPRITE2D_BASE (0: none)
    float   sprscalex,sprscaley; // its SCALEFLIP scale (u5.10) and flips
    int     sprflip;
    int     pdvx;        // Perfect Dark's Fast3D: 12-byte vertices with a color index
    dword   vtxcolorbase;// its vertex color table (command 07)
    // what has changed (checked at startdrawing())
    int     newsettings;
    int     newtexture;
    int     newmatrices;
    int     newvertices;
    int     newvertices_lo;
    int     newvertices_hi;
    // raw modebits (omode just sent to RDP after changes)
    dword   gmode,lastgmode;
    dword   omodel;
    dword   omodeh;
    // parsed geometry mode
    int     lightvx;
    int     flat;
    int     texgen;
    int     cull;
    // texture scaling
    int     texenable;
    float   scales;
    float   scalet;
    // counts
    int     cnt_triin; // input triangles
    int     cnt_tri;   // sent to rdp
    int     cnt_vtx;
    int     cnt_mtx;
    // misc
    int     cammoveset;
    float   cammove[3];

    int     errorcnt[256];

    int     ignore;
} GeomState;

#define PROJ_NONE  0 // set to this when matrices change
#define PROJ_PERSP 1
#define PROJ_DEBUG 2

#define COLOR_ZERO       0
#define COLOR_DUNNO      1
#define COLOR_SHADE      2
#define COLOR_PRIM       3
#define COLOR_ENV        4
#define COLOR_SHADE_ENV  5
#define COLOR_SHADE_PRIM 6
#define COLOR_ONE        7

#define COMBINE_COLOR   0
#define COMBINE_MUL     1
#define COMBINE_TEXTURE 2
#define COMBINE_PRO     3

// combine colors
#define CC_ZERO   0
#define CC_DUNNO  1
#define CC_TEXT   2
#define CC_PRIM   3
#define CC_SHADE  4
#define CC_ENV    5
#define CC_TEXTA  6

#define BLEND_DUNNO              0
#define BLEND_MULALPHA           1 // 0055
#define BLEND_AA_OPAQUE          2 // 0044
#define BLEND_NORMAL             3 // 0F0A or 0A0F
#define BLEND_ALPHAMIX           4 // 0050
#define BLEND_ALPHAMIX2          5 // 0040

#define TRISTORESIZE 4096
struct
{
    xt_pos  vx[3];
    int     cull;
    int     color; // 0=red, 1=green, 2=blue, 3=white
    int     RESERVED;
} tristore[TRISTORESIZE];
int tristorenum;

xt_data  g_data={0,0,0,0,0, 0,0,0, 0,0,0, -1.0f}; // zs: depth from w

xt_pos   g_pos;

int  opened;

GeomState gst;

dword stack[256];
dword stackp;
dword dlpnt;
int   dllimit;
int   errors;
dword cmd[8]; // current loaded command

// the last commands walked (address, word 0, word 1), printed to the log at
// the first unknown command of a list, so a rare bad jump can be traced
// without a dump running (BattleTanx level 2 ran into texture data)
#define DL_HIST 24
static dword dl_hist[DL_HIST][3];
static int   dl_histn;       // commands recorded in this list
static int   dl_histshown;   // this list's history already printed
static int   dl_histdumps;   // histories printed this session

float identity[16]={
 1.0,   0.0,   0.0,   0.0,
 0.0,   1.0,   0.0,   0.0,
 0.0,   0.0,   1.0,   0.0,
 0.0,   0.0,   0.0,   1.0,
};

float orthoproj[16]={
 1.0,   0.0,   0.0, -OVLZ,
 0.0,   1.0,   0.0,  OVLZ,
 0.0,   0.0,   0.0,   0.0,
 0.0,   0.0,   0.0,  OVLZ,
};

#define CH_OTHERL   0x1
#define CH_OTHERH   0x2
#define CH_GEOM     0x4
#define CH_COMBINE  0x8
#define CH_DUMP     0x100
void g_modechange(int what);
void g_mtxxform(void);

void startdebugdrawing(void);
void startdrawing(void);

void clear(int color);
void opendisplay(void);
void flushtextstore(void);
void flushtristore(void);
void framedone(void);
void reloadsettings(void);

void t_selecttexture(int t,int txt);

void setcolor(float *col,dword a);
void setcolor2(float *col,dword a);
void setnormal(float *col,dword a);
void setnormal2(float *col,dword a);

/***********************************************************************/
// logging

/***********************************************************************/
// utilities

void setcolor(float *col,dword a)
{
    col[0]=((a>>8 )&255)*(1.0/258.0);
    col[1]=((a>>16)&255)*(1.0/258.0);
    col[2]=((a>>24)&255)*(1.0/258.0);
    col[3]=((a>>0 )&255)*(1.0/258.0);
}

void setcolor2(float *col,dword a)
{
    col[0]=((a>>24)&255)*(1.0/258.0);
    col[1]=((a>>16)&255)*(1.0/258.0);
    col[2]=((a>>8 )&255)*(1.0/258.0);
    col[3]=((a>>0 )&255)*(1.0/258.0);
}

void setnormal(float *col,dword a)
{
    col[0]=((char)((a>>8 )&255))*(1.0/128.0);
    col[1]=((char)((a>>16)&255))*(1.0/128.0);
    col[2]=((char)((a>>24)&255))*(1.0/128.0);
    col[3]=((char)((a>>0 )&255))*(1.0/128.0);
}

void setnormal2(float *col,dword a)
{
    col[0]=((char)((a>>24)&255))*(1.0/128.0);
    col[1]=((char)((a>>16)&255))*(1.0/128.0);
    col[2]=((char)((a>>8 )&255))*(1.0/128.0);
    col[3]=((char)((a>>0 )&255))*(1.0/128.0);
}

void TransposeMatrix(float *m,float *m1)
{
    int x,y;
    for(y=0;y<4;y++)
    {
        for(x=0;x<4;x++)
        {
            m[x+y*4]=m1[y+x*4];
        }
    }
}

void MultMatrix(float *m,float *m1,float *m2)
{
    int x,y,i;
    float f;
    for(y=0;y<4;y++)
    {
        for(x=0;x<4;x++)
        {
            f=0;
            for(i=0;i<4;i++)
            {
                //f+=m1[i+y*4] * m2[x+i*4];
                f+=m1[i+y*4] * m2[x+i*4];
            }
            m[x+y*4]=f;
        }
    }
}

void FastMultMatrix(float *m,float *m1,float *m2)
{ // assume rightmost column is 0 0 0 1
    int x,y;
    float f;
    for(y=0;y<4;y++)
    {
        for(x=0;x<3;x++)
        {
            f=m1[0+y*4] * m2[x+0*4] +
              m1[1+y*4] * m2[x+1*4] +
              m1[2+y*4] * m2[x+2*4] +
                          m2[x+3*4] ;
            m[x+y*4]=f;
        }
        m[3+y*4]=0.0;
    }
    m[3+3*4]=1.0;
}

void InvertMatrix(float *dest,float *m)
{
    float *d,*a;
    d=(float *)dest;
    a=(float *)m;

    #define D(y,x) d[(x)*4+(y)]
    #define S(y,x) a[(x)*4+(y)]
    #define DET(a,b,c,d) (S(a,b)*S(c,d)-S(a,d)*S(c,b))

    D(0,0)=+DET(1,1,2,2);
    D(0,1)=-DET(0,1,2,2);
    D(0,2)=+DET(0,1,1,2);
    D(1,0)=-DET(1,0,2,2);
    D(1,1)=+DET(0,0,2,2);
    D(1,2)=-DET(0,0,1,2);
    D(2,0)=+DET(1,0,2,1);
    D(2,1)=-DET(0,0,2,1);
    D(2,2)=+DET(0,0,1,1);
    D(0,3)=-(S(0,3)*D(0,0)+S(1,3)*D(0,1)+S(2,3)*D(0,2));
    D(1,3)=-(S(0,3)*D(1,0)+S(1,3)*D(1,1)+S(2,3)*D(1,2));
    D(2,3)=-(S(0,3)*D(2,0)+S(1,3)*D(2,1)+S(2,3)*D(2,2));
    D(3,0)=0.0f;
    D(3,1)=0.0f;
    D(3,2)=0.0f;
    D(3,3)=1.0f;

    #undef DET
    #undef D
    #undef S
}

void InvertMatrixRotate(float *dest,float *m)
{
    float *d,*a;
    d=(float *)dest;
    a=(float *)m;

    #define D(y,x) d[(x)*4+(y)]
    #define S(y,x) a[(x)*4+(y)]
    #define DET(a,b,c,d) (S(a,b)*S(c,d)-S(a,d)*S(c,b))

    D(0,0)=+DET(1,1,2,2);
    D(0,1)=-DET(0,1,2,2);
    D(0,2)=+DET(0,1,1,2);
    D(1,0)=-DET(1,0,2,2);
    D(1,1)=+DET(0,0,2,2);
    D(1,2)=-DET(0,0,1,2);
    D(2,0)=+DET(1,0,2,1);
    D(2,1)=-DET(0,0,2,1);
    D(2,2)=+DET(0,0,1,1);

    #undef DET
    #undef D
    #undef S
}

void DumpMatrix(float *m)
{
    int x,y;
    if(!st.dumpgfx) return;
    for(y=0;y<4;y++)
    {
        logd("\n{ ");
        for(x=0;x<4;x++)
        {
            logd("%13.5f ",m[x+y*4]);
        }
        logd(" } ");
    }
}

void PrintMatrix(float *m)
{
    int x,y;
    for(y=0;y<4;y++)
    {
        print("{ ");
        for(x=0;x<4;x++)
        {
            print("%13.5f ",m[x+y*4]);
        }
        print(" }\n");
    }
}

void DumpMatrix2(float *m)
{
    int x,y;
    if(!st.dumpgfx) return;
    for(y=0;y<4;y++)
    {
        print("\n{ ");
        for(x=0;x<4;x++)
        {
            print("%13.5f ",m[x+y*4]);
        }
        print(" } ");
    }
}

void dlist_showa0matrix(void)
{
    float m[16];
    int   i;
    if(!st.dumpgfx) return;
    for(i=0;i<16;i++)
    {
        int x;
        x=mem_read32p(i*4+A0.d);
        m[i]=*(float *)&x;
    }
    print("\nguMtxF2L:");
    DumpMatrix2(m);
    print("\n");
}

/***********************************************************************/
// display list geometry commands

// G_BRANCH_Z (F3DEX 1.x): each loaded vertex's screen z, 0..1023 as the
// RSP computes it (z/w through the viewport's z scale and offset); -1 when
// it is at or behind the eye
static float g_vxzs[256];
static float g_vpzscale=511.0f,g_vpztrans=511.0f; // libultra's G_MAXZ/2

void g_viewport(dword pos)
{
    dword x;
    float xm,ym,xa,ya,zm;

    // scale and translation are signed s10.2 (GLideN64 gSPViewport reads
    // s16): Mario Party scrolls its board by moving the viewport off the
    // left edge (x translation 0xFE50 = -108), and read unsigned the
    // players were drawn 16000 pixels to the right
    x=mem_read32p(pos);
    xm=(short)(x>>16  )*0.25;
    ym=(short)(x&65535)*0.25;

    x=mem_read32p(pos+4);
    zm=(x>>16  )*1.00;

    x=mem_read32p(pos+8);
    xa=(short)(x>>16  )*0.25;
    ya=(short)(x&65535)*0.25;

    g_vpzscale=(float)(short)(mem_read32p(pos+4)>>16);
    g_vpztrans=(float)(short)(mem_read32p(pos+12)>>16);

    logd("\n!viewport *(%f,%f)+(%f,%f) z(%f) ",xm,ym,xa,ya,zm);

    gst.zmax=zm;
    gst.vp[0]=xm; gst.vp[1]=ym; gst.vp[2]=xa; gst.vp[3]=ya;
    rdp_gameviewport(xm,ym,xa,ya);
}

void g_lightnum(int num)
{
    if(num<0) num=0;
    if(num>7) num=7;
    logd(" Lightnum=%i",num);
    gst.lightnum=num;
    gst.lightnumchanged=1;
}

void g_loadlight(int li,dword pos)
{
    Light *l=gst.light+li;
    int i;
    l->data[0]=mem_read32p(pos+0);
    l->data[1]=mem_read32p(pos+4);
    l->data[2]=mem_read32p(pos+8);
    l->data[3]=mem_read32p(pos+12);
    setcolor2(l->col,l->data[0]);
    setnormal2(l->dir,l->data[2]);
    logd("\n!light %i color %.2f %.2f %.2f dir %.2f %.2f %.2f %.2f (%i lights) [ ",
        li,
        l->col[0],l->col[1],l->col[2],
        l->dir[0],l->dir[1],l->dir[2],l->dir[3],
        gst.lightnum);
    for(i=0;i<4;i++) logd("%08X ",l->data[i]);
    logd("]");
}

// Conker's G_MV_LIGHT: 48 bytes a light; color at 0, s8 direction at 8,
// attenuation at 12 (/16), s16 position at 32
void g_cbfdloadlight(int li,dword pos)
{
    dword w;
    float x,y,z,len;
    if(li<0 || li>=12) return;
    w=mem_read32p(pos+0);
    gst.cbfdlight[li].col[0]=((w>>24)&255)/255.0f;
    gst.cbfdlight[li].col[1]=((w>>16)&255)/255.0f;
    gst.cbfdlight[li].col[2]=((w>> 8)&255)/255.0f;
    w=mem_read32p(pos+8);
    x=(float)(signed char)(w>>24);
    y=(float)(signed char)(w>>16);
    z=(float)(signed char)(w>> 8);
    len=(float)sqrt(x*x+y*y+z*z);
    if(len>0) { x/=len; y/=len; z/=len; }
    gst.cbfdlight[li].dir[0]=x;
    gst.cbfdlight[li].dir[1]=y;
    gst.cbfdlight[li].dir[2]=z;
    w=mem_read32p(pos+12);
    gst.cbfdlight[li].ca=((w>>24)&255)/16.0f;
    w=mem_read32p(pos+32);
    gst.cbfdlight[li].pos[0]=(float)(short)(w>>16);
    gst.cbfdlight[li].pos[1]=(float)(short)(w&0xffff);
    w=mem_read32p(pos+36);
    gst.cbfdlight[li].pos[2]=(float)(short)(w>>16);
    logd("\n!cbfd light %i color %.2f %.2f %.2f dir %.2f %.2f %.2f pos %.0f %.0f %.0f ca %.2f",li,
        gst.cbfdlight[li].col[0],gst.cbfdlight[li].col[1],gst.cbfdlight[li].col[2],x,y,z,
        gst.cbfdlight[li].pos[0],gst.cbfdlight[li].pos[1],gst.cbfdlight[li].pos[2],gst.cbfdlight[li].ca);
}

// Conker's G_MW_COORD_MOD
void g_cbfdcoordmod(dword w0,dword w1)
{
    int    idx=(w0>>1)&3,pos=w0&0x30;
    float *mod=gst.cbfdmod;
    if(w0&8) return;
    if(pos==0x00)
    {
        mod[0+idx]=(float)(short)(w1>>16);
        mod[1+idx]=(float)(short)(w1&0xffff);
    }
    else if(pos==0x10)
    {
        mod[4+idx]=(w1>>16)/65536.0f;
        mod[5+idx]=(w1&0xffff)/65536.0f;
        mod[12+idx]=mod[0+idx]+mod[4+idx];
        mod[13+idx]=mod[1+idx]+mod[5+idx];
    }
    else if(pos==0x20)
    {
        mod[8+idx]=(float)(short)(w1>>16);
        mod[9+idx]=(float)(short)(w1&0xffff);
    }
}

// Conker's light directions in model space (the modelview's rotation
// applied inversely), for the vertices loaded next
static void g_cbfdxformlights(float dirs[12][3])
{
    float m[16],x,y,z,len;
    int   i;
    InvertMatrixRotate(m,gst.mtx[0][gst.mtxstackp[0]]);
    for(i=0;i<12;i++)
    {
        float *d=gst.cbfdlight[i].dir;
        x=d[0]*m[4*0+0]+d[1]*m[4*1+0]+d[2]*m[4*2+0];
        y=d[0]*m[4*0+1]+d[1]*m[4*1+1]+d[2]*m[4*2+1];
        z=d[0]*m[4*0+2]+d[1]*m[4*1+2]+d[2]*m[4*2+2];
        len=(float)sqrt(x*x+y*y+z*z);
        if(len>0) { x/=len; y/=len; z/=len; }
        dirs[i][0]=x; dirs[i][1]=y; dirs[i][2]=z;
    }
}

// lights one Conker vertex (slot i, model-space xyz, flag) into its color
// (GLideN64 gSPLightVertexCBFD_basic/_advanced, RT64 lightVerticesCBFD)
static dword g_cbfdlightvx(dword icol,int i,float x,float y,float z,int flag,float dirs[12][3])
{
    int    n=gst.cbfdlightnum,l;
    float *mod=gst.cbfdmod;
    float  r,g,b,px,py,pz,nor[3]={0,0,0},in;

    if(n>11) n=11;
    if((short)flag<0) return(icol);
    px=(x+mod[8])*mod[12];
    py=(y+mod[9])*mod[13];
    pz=(z+mod[10])*mod[14];
    // ambient: the last light
    r=gst.cbfdlight[n].col[0];
    g=gst.cbfdlight[n].col[1];
    b=gst.cbfdlight[n].col[2];
    l=n-1;
    if(gst.cbfdadv)
    {
        dword a=gst.cbfdnormals+(i<<1);
        dword w=mem_read32p(a&~3);
        nor[0]=(signed char)(w>>(24-8*(a&3)))/127.0f;
        a++;
        w=mem_read32p(a&~3);
        nor[1]=(signed char)(w>>(24-8*(a&3)))/127.0f;
        nor[2]=(signed char)(flag&0xff)/127.0f;
        // the one before the ambient is directional
        if(l>=0)
        {
            in=nor[0]*dirs[l][0]+nor[1]*dirs[l][1]+nor[2]*dirs[l][2];
            if(in>1) in=1;
            if(in>0)
            {
                r+=gst.cbfdlight[l].col[0]*in;
                g+=gst.cbfdlight[l].col[1]*in;
                b+=gst.cbfdlight[l].col[2]*in;
            }
        }
    }
    // the others are point lights
    for(l=n-2;l>=0;l--)
    {
        float vx=px-gst.cbfdlight[l].pos[0];
        float vy=py-gst.cbfdlight[l].pos[1];
        float vz=pz-gst.cbfdlight[l].pos[2];
        float len=2.0f*(vx*vx+vy*vy+vz*vz)/65536.0f;
        in=len>0?gst.cbfdlight[l].ca/len:1.0f;
        if(in>1) in=1;
        if(gst.cbfdadv && (gst.gmode&0x00400000)) // G_POINT_LIGHTING
        {
            float d=nor[0]*dirs[l][0]+nor[1]*dirs[l][1]+nor[2]*dirs[l][2];
            in*=d<1?d:1;
        }
        if(in>0)
        {
            r+=gst.cbfdlight[l].col[0]*in;
            g+=gst.cbfdlight[l].col[1]*in;
            b+=gst.cbfdlight[l].col[2]*in;
        }
    }
    if(r>1) r=1;
    if(g>1) g=1;
    if(b>1) b=1;
    return(((dword)(r*((icol>>24)&255))<<24)|
           ((dword)(g*((icol>>16)&255))<<16)|
           ((dword)(b*((icol>> 8)&255))<< 8)|(icol&255));
}

void g_xformlights(void)
{
    Light *l;
    int    i;
    float  m[16];

    logd("\n!light xform (%i lights)",gst.lightnum);

    // calc inverse modelview (only rotate part)
    InvertMatrixRotate(m,gst.mtx[0][gst.mtxstackp[0]]);
    /*
    DumpMatrix(gst.mtx[0][gst.mtxstackp[0]]);
    DumpMatrix(m);
    */

    for(i=0;i<gst.lightnum;i++)
    {
        l=&gst.light[i];

        if(0)
        {
            // copy light
            l->xfdir[0]=l->dir[0];
            l->xfdir[1]=l->dir[1];
            l->xfdir[2]=l->dir[2];
        }
        else
        {
            float inv;
            // transform lights with inverse modelview and normalize
            l->xfdir[0]=l->dir[0]*m[4*0+0]+l->dir[1]*m[4*1+0]+l->dir[2]*m[4*2+0];
            l->xfdir[1]=l->dir[0]*m[4*0+1]+l->dir[1]*m[4*1+1]+l->dir[2]*m[4*2+1];
            l->xfdir[2]=l->dir[0]*m[4*0+2]+l->dir[1]*m[4*1+2]+l->dir[2]*m[4*2+2];
            inv=sqrt(l->xfdir[0]*l->xfdir[0]+
                     l->xfdir[1]*l->xfdir[1]+
                     l->xfdir[2]*l->xfdir[2]);
            // a zero direction (Aerogauge's light 1) stays zero, not NaN
            inv=inv>0 ? 1.0/inv : 0;
            l->xfdir[0]*=inv;
            l->xfdir[1]*=inv;
            l->xfdir[2]*=inv;
            logd("\n!light %i: %.2f %.2f %.2f -> %.2f %.2f %.2f",
                i,
                l->dir[0],l->dir[1],l->dir[2],
                l->xfdir[0],l->xfdir[1],l->xfdir[2]);
        }
    }
}

void g_initvx(int i)
{
    Vertex *v;
    float  nor[4];

    v=rdpvx[i];

    if(gst.lightvx || gst.texgen)
    {
        // get normal
        nor[0]=((char)((v->icol>>24)&255))*(1.0/128.0);
        nor[1]=((char)((v->icol>>16)&255))*(1.0/128.0);
        nor[2]=((char)((v->icol>>8 )&255))*(1.0/128.0);
    }

    // Conker's vertices were lit into their color as they loaded (g_loadvtx)
    if(gst.lightvx && !gst.cbfd)
    {
        float  cr,cg,cb,sc;
        Light *l;
        int    j;
        // ambient
        l=&gst.light[gst.lightnum];
        cr=l->col[0];
        cg=l->col[1];
        cb=l->col[2];
        // directional
        for(j=0;j<gst.lightnum;j++)
        {
            l=&gst.light[j];
            sc=l->xfdir[0]*nor[0]+
               l->xfdir[1]*nor[1]+
               l->xfdir[2]*nor[2];
            if(sc<0) sc=0;
            cr+=l->col[0]*sc;
            cg+=l->col[1]*sc;
            cb+=l->col[2]*sc;
        }
        // clamp
        if(cr>0.99) cr=0.99;
        if(cg>0.99) cg=0.99;
        if(cb>0.99) cb=0.99;
        // set color
        v->col[0]=cr;
        v->col[1]=cg;
        v->col[2]=cb;
        // set alpha
        v->col[3]=(((v->icol    )&255))*(1.0/256.0);
//        v->col[3]=1.0;
    }
    else
    {
        // get color
        v->col[0]=(((v->icol>>24)&255))*(1.0/256.0);
        v->col[1]=(((v->icol>>16)&255))*(1.0/256.0);
        v->col[2]=(((v->icol>> 8)&255))*(1.0/256.0);
        // get alpha
        v->col[3]=(((v->icol    )&255))*(1.0/256.0);
    }

    if(gst.texenable)
    {
        v->tex[0]*=gst.scales;
        v->tex[1]*=gst.scalet;
    }

    rdpvxflag[i]|=VX_INITDONE;
}

void g_loadvtx(dword addr,int v0,int vn)
{
    int     i,flag;
    int     itex,ipos1,ipos2;
    float   x,y,z,l;
    Vertex *v;
    float  *m=gst.xform;
    float   cbfddirs[12][3];
    int     cbfdlit=gst.cbfd && gst.lightvx;
    int     ortho;

    st2.gfx_vxin+=4;

    if(gst.newmatrices)
    {
        g_mtxxform();
    }
    if(gst.lightvx && gst.lightnumchanged)
    {
        gst.lightnumchanged=0;
        g_xformlights();
    }
    if(cbfdlit) g_cbfdxformlights(cbfddirs);
    ortho=m[3+0*4]==0.0f && m[3+1*4]==0.0f && m[3+2*4]==0.0f;

    rdp_newvtx(v0,vn);

    for(i=v0;i<v0+vn;i++)
    {
        v=rdpvx[i];
        flag=0;

        // read raw data & extract color
        ipos1  =mem_read32p(addr+0);
        ipos2  =mem_read32p(addr+4);
        itex   =mem_read32p(addr+8);
        if(gst.pdvx)
        { // Perfect Dark (GLideN64 gSPCIVertex): x y z, a flag byte, the
          // color's byte offset in the table of command 07, s t. The table
          // entry is the usual color or normal word.
            dword ca=gst.vtxcolorbase+(ipos2&0xff);
            int   sh=(ca&3)*8;
            v->icol=mem_read32p(ca&~3);
            if(sh) v->icol=(v->icol<<sh)|(mem_read32p((ca&~3)+4)>>(32-sh));
            addr+=12;
        }
        else
        {
            v->icol=mem_read32p(addr+12);
            addr+=16;
        }
        if(cbfdlit)
            v->icol=g_cbfdlightvx(v->icol,i,(float)(short)(ipos1>>16),
                (float)(short)(ipos1&0xffff),(float)(short)(ipos2>>16),ipos2&0xffff,cbfddirs);

        if(st.dumpgfx)
        {
            float fu,fv;
            fu=(itex>>16)/32.0;
            fv=((itex<<16)>>16)/32.0;
            logd("\nlvx[%02i: xyz %04X %04X %04X fl %04X uv %04X %04X rgba %08X uvf %7.2f %7.2f]",
                i,
                (ipos1>>16)&0xffff,
                ipos1&0xffff,
                (ipos2>>16)&0xffff,
                ipos2&0xffff,
                (itex>>16)&0xffff,
                itex&0xffff,
                v->icol,
                fu,fv);
        }

        // extract texture coordinates
        if(gst.texgen && cart.dlist_zelda==1)
        { // F3DEX2 (GLideN64 gSPProcessVertex): the normal in the
          // modelview's space, measured along the look-at vectors (the
          // camera's right and up) or along x and y without them. Army Men
          // Sarge's Heroes puts a plastic shine on its soldiers this way; with
          // the vertex's own coordinates the shine covered the whole figure.
            const float *mv=gst.mtx[0][gst.mtxstackp[0]];
            float n[3],w[3],gx,gy,len;
            n[0]=(float)(char)(v->icol>>24);
            n[1]=(float)(char)(v->icol>>16);
            n[2]=(float)(char)(v->icol>>8);
            w[0]=n[0]*mv[0+0*4]+n[1]*mv[0+1*4]+n[2]*mv[0+2*4];
            w[1]=n[0]*mv[1+0*4]+n[1]*mv[1+1*4]+n[2]*mv[1+2*4];
            w[2]=n[0]*mv[2+0*4]+n[1]*mv[2+1*4]+n[2]*mv[2+2*4];
            len=sqrtf(w[0]*w[0]+w[1]*w[1]+w[2]*w[2]);
            if(len>0.0f) { w[0]/=len; w[1]/=len; w[2]/=len; }
            if(gst.lookaton)
            {
                gx=w[0]*gst.lookat[0][0]+w[1]*gst.lookat[0][1]+w[2]*gst.lookat[0][2];
                gy=w[0]*gst.lookat[1][0]+w[1]*gst.lookat[1][1]+w[2]*gst.lookat[1][2];
            }
            else
            {
                gx=w[0];
                gy=w[1];
            }
            if(gst.texgen&2)
            { // G_TEXTURE_GEN_LINEAR
                if(gx<-1.0f) gx=-1.0f;
                if(gx> 1.0f) gx= 1.0f;
                if(gy<-1.0f) gy=-1.0f;
                if(gy> 1.0f) gy= 1.0f;
                v->tex[0]=acosf(-gx)*(32768.0f/3.14159265f);
                v->tex[1]=acosf(-gy)*(32768.0f/3.14159265f);
            }
            else
            {
                v->tex[0]=(gx+1.0f)*16384.0f;
                v->tex[1]=(gy+1.0f)*16384.0f;
            }
        }
        else if(gst.texgen==1)
        {
            float  nor[4];
            float s,t,u;
            // texgen generation for highlights, pretty slow but seldom used
            // get normal
            nor[0]=((char)((v->icol>>24)&255))*(1.0/128.0);
            nor[1]=((char)((v->icol>>16)&255))*(1.0/128.0);
            nor[2]=((char)((v->icol>>8 )&255))*(1.0/128.0);
            // calc rotated normal
            s=nor[0]*m[0+0*4]+nor[1]*m[0+1*4]+nor[2]*m[0+2*4];
            t=nor[0]*m[1+0*4]+nor[1]*m[1+1*4]+nor[2]*m[1+2*4];
            u=nor[0]*m[2+0*4]+nor[1]*m[2+1*4]+nor[2]*m[2+2*4];
            l=0.4/sqrt(s*s+t*t+u*u);
            s=(0.4+s*l);
            t=(0.4+t*l);
            s=s*32768.0;
            t=t*32768.0;
            v->tex[0]=s;
            v->tex[1]=t;
        }
        else
        {
            v->tex[0]=(float)((itex    )>>16);
            v->tex[1]=(float)((itex<<16)>>16);
        }

        // extract position
        x=+(float)((ipos1    )>>16);
        y=+(float)((ipos1<<16)>>16);
        z=+(float)((ipos2    )>>16);

        // transform
        v->pos[0]=x*m[0+0*4]+y*m[0+1*4]+z*m[0+2*4]+m[0+3*4];
        v->pos[1]=x*m[1+0*4]+y*m[1+1*4]+z*m[1+2*4]+m[1+3*4];
        v->pos[2]=x*m[3+0*4]+y*m[3+1*4]+z*m[3+2*4]+m[3+3*4]; // from W!!
        {
            float zc=x*m[2+0*4]+y*m[2+1*4]+z*m[2+2*4]+m[2+3*4];
            if(i<256)
                g_vxzs[i]=v->pos[2]>0.0f?zc/v->pos[2]*g_vpzscale+g_vpztrans:-1.0f;
            // Orthographic projection: w is the same for every vertex, so
            // depth from w can't order anything. Use the real z/w instead.
            // Harvest Moon 64's title draws its layers out of order at
            // z 32..88 and relies on the z buffer; at equal depth the sky
            // and grass (drawn later) covered the logo, sign and dog.
            if(ortho && v->pos[2]>0.0f)
            {
                float zs=zc/v->pos[2]*0.5f+0.5f;
                v->zs=zs<0.0f?0.0f:(zs>1.0f?1.0f:zs);
            }
            // Perspective: the RDP's depth too, z/w through the viewport
            // (0..G_MAXZ). Depth from w is in each projection's own units:
            // Road Rash 64 draws its scene in passes whose projections differ
            // by a scale (near planes 4, 10 and 100), the same picture with
            // w ten times apart, and the road covered the riders.
            else if(v->pos[2]>0.0f)
            {
                // Viewport Z has ten fractional bits (GLideN64 gSPViewport).
                // Dividing by G_MAXZ instead shifts the CPU-visible depth.
                float zs=(zc/v->pos[2]*g_vpzscale+g_vpztrans)*(1.0f/1024.0f);
                v->zs=zs<0.0f?0.0f:(zs>1.0f?1.0f:zs);
            }
        }

        // clipcheck
        if(v->pos[0]<-v->pos[2]) flag|=VX_CLIPX1;
        if(v->pos[0]>+v->pos[2]) flag|=VX_CLIPX2;
        if(v->pos[1]<-v->pos[2]) flag|=VX_CLIPY1;
        if(v->pos[1]>+v->pos[2]) flag|=VX_CLIPY2;

        if(st.dumpgfx)
        {
            logd(" screen %.3f %.3f %.3f clip %02X ",
                v->pos[0],
                v->pos[1],
                v->pos[2],
                flag);
        }

        rdpvxflag[i]=flag;
    }
}

void g_loadvtx_diddly(dword addr,int v0,int vn)
{
    int     i,flag;
    int     itex,ipos1,ipos2;
    float   x,y,z;
    Vertex *v;
    float  *m=gst.xform;

    st2.gfx_vxin+=4;

    if(gst.newmatrices)
    {
        g_mtxxform();
    }
    if(gst.lightvx && gst.lightnumchanged)
    {
        gst.lightnumchanged=0;
        g_xformlights();
    }

    rdp_newvtx(v0,vn);

    for(i=v0;i<v0+vn;i++)
    {
        v=rdpvx[i];
        flag=0;

        // read raw data & extract color
        ipos1  =(mem_read16(addr+0)<<16)|mem_read16(addr+2);
        ipos2  = mem_read16(addr+4)<<16;
        itex   =0;
        v->icol=(mem_read16(addr+6)<<16)|mem_read16(addr+8);
        addr+=10;

        if(st.dumpgfx)
        {
            logd("\nlvx[%02i: xyz %04X %04X %04X rgba %08X diddly]",
                i,
                (ipos1>>16)&0xffff,
                ipos1&0xffff,
                (ipos2>>16)&0xffff,
                v->icol);
        }

        // extract position
        x=+(float)((ipos1    )>>16);
        y=+(float)((ipos1<<16)>>16);
        z=+(float)((ipos2    )>>16);

        // transform
        v->pos[0]=x*m[0+0*4]+y*m[0+1*4]+z*m[0+2*4]+m[0+3*4];
        v->pos[1]=x*m[1+0*4]+y*m[1+1*4]+z*m[1+2*4]+m[1+3*4];
        v->pos[2]=x*m[3+0*4]+y*m[3+1*4]+z*m[3+2*4]+m[3+3*4]; // from W!!

        if(gst.dkr_billboard)
        {
            v->pos[0]+=rdpvx[0]->pos[0];
            v->pos[1]+=rdpvx[0]->pos[1];
            v->pos[2]+=rdpvx[0]->pos[2];
        }

        // clipcheck
        if(v->pos[0]<-v->pos[2]) flag|=VX_CLIPX1;
        if(v->pos[0]>+v->pos[2]) flag|=VX_CLIPX2;
        if(v->pos[1]<-v->pos[2]) flag|=VX_CLIPY1;
        if(v->pos[1]>+v->pos[2]) flag|=VX_CLIPY2;

        if(st.dumpgfx)
        {
            logd(" screen %.3f %.3f %.3f clip %02X ",
                v->pos[0],
                v->pos[1],
                v->pos[2],
                flag);
        }

        rdpvxflag[i]=flag;
    }
}

int g_culltri(int *vxind)
{
    float xd1,yd1,zd1;
    float xd2,yd2,zd2;
    float bx,by,bz;
    float x,y,z,d;
    if(1)
    {
        bx =rdpvx[vxind[0]]->pos[0];
        by =rdpvx[vxind[0]]->pos[1];
        bz =rdpvx[vxind[0]]->pos[2];
        xd1=rdpvx[vxind[1]]->pos[0];
        yd1=rdpvx[vxind[1]]->pos[1];
        zd1=rdpvx[vxind[1]]->pos[2];
        xd2=rdpvx[vxind[2]]->pos[0];
        yd2=rdpvx[vxind[2]]->pos[1];
        zd2=rdpvx[vxind[2]]->pos[2];
        x=(yd1*zd2)-(yd2*zd1);
        y=(zd1*xd2)-(zd2*xd1);
        z=(xd1*yd2)-(xd2*yd1);
        d=x*bx+y*by+z*bz;
    }
    else
    {
        bx =rdpvx[vxind[0]]->pos[0];
        by =rdpvx[vxind[0]]->pos[1];
        bz =rdpvx[vxind[0]]->pos[2];
        xd1=rdpvx[vxind[1]]->pos[0]-bx;
        yd1=rdpvx[vxind[1]]->pos[1]-by;
        zd1=rdpvx[vxind[1]]->pos[2]-bz;
        xd2=rdpvx[vxind[2]]->pos[0]-bx;
        yd2=rdpvx[vxind[2]]->pos[1]-by;
        zd2=rdpvx[vxind[2]]->pos[2]-bz;
        x=(yd1*zd2)-(yd2*zd1);
        y=(zd1*xd2)-(zd2*xd1);
        z=(xd1*yd2)-(xd2*yd1);
        d=x*bx+y*by+z*bz;
    }
    if((gst.cull&1) && d<=0) return(1);
    if((gst.cull&2) && d>=0) return(1);
    return(0);
}

void g_tri(int flag,int *vxind)
{
    int clipmask;

    st2.gfx_trisin++;

    if(st.dumpgfx)
    {
        logd(" (%i,%i,%i)",vxind[0],vxind[1],vxind[2]);
    }
    // check visibility
    clipmask =VX_CLIPALL;
    clipmask&=rdpvxflag[vxind[0]];
    clipmask&=rdpvxflag[vxind[1]];
    clipmask&=rdpvxflag[vxind[2]];
    if(clipmask)
    {
        if(st.dumpgfx) logd(" Cl%02X",clipmask);
        return;
    }

    // check culling
    if(gst.cull && g_culltri(vxind))
    {
        if(st.dumpgfx) logd(" Cull");
        return;
    }

    // init vertices (if not inited)
    if(!(rdpvxflag[vxind[0]]&VX_INITDONE)) g_initvx(vxind[0]);
    if(!(rdpvxflag[vxind[1]]&VX_INITDONE)) g_initvx(vxind[1]);
    if(!(rdpvxflag[vxind[2]]&VX_INITDONE)) g_initvx(vxind[2]);

    // send triangle to drawpipe
    rdp_tri(vxind);
    gst.cnt_tri++;
}

int g_culldl(int v0,int vn)
{
    int i,m=VX_CLIPALL;
    for(i=v0;i<vn;i++) m&=rdpvxflag[i];
    if(m) return(1); // hidden
    else return(0);
}

/***********************************************************************/
// display list matrix commands

void g_resetmtx(void)
{
    gst.mtxstackp[0]=0;
    gst.mtxstackp[1]=0;
    memcpy(gst.mtx[0][0],identity,16*sizeof(float));
    memcpy(gst.mtx[1][0],identity,16*sizeof(float));
}

float testmat[16]={
 1.0,   0.0,   0.0,   0.0,
 0.0,  -1.0,   0.0,   0.0,
 0.0,   0.0,   1.0,   1.0,
-1.0,   1.0,   0.0, 500.0,
};

void g_mtxxform(void)
{
    if(!gst.newmatrices) return;
    gst.newmatrices=0;

    if(gst.cammoveset)
    {
        gst.mtx[0][gst.mtxstackp[0]][3*4+0]+=gst.cammove[0];
        gst.mtx[0][gst.mtxstackp[0]][3*4+1]+=gst.cammove[1];
        gst.mtx[0][gst.mtxstackp[0]][3*4+2]+=gst.cammove[2];
    }

    if(cart.dlist_diddlyvx)
    {
        memcpy(gst.xform,gst.mtx[0][gst.mtxstackp[0]],sizeof(gst.xform));
    }
    else
    {
        MultMatrix(gst.xform,
                   gst.mtx[0][gst.mtxstackp[0]],  // modelview
                   gst.mtx[1][gst.mtxstackp[1]]); // projection
    }

    if(gst.cammoveset)
    {
        gst.mtx[0][gst.mtxstackp[0]][3*4+0]-=gst.cammove[0];
        gst.mtx[0][gst.mtxstackp[0]][3*4+1]-=gst.cammove[1];
        gst.mtx[0][gst.mtxstackp[0]][3*4+2]-=gst.cammove[2];
    }

    if(st.dumpgfx)
    {
        if(!cart.dlist_diddlyvx)
        {
            logd("\n{ Active Projection Matrix [%i]:",gst.mtxstackp[1]);
            DumpMatrix(gst.mtx[1][gst.mtxstackp[1]]);
            logd("\n{ Active Modelview Matrix [%i]:",gst.mtxstackp[0]);
            DumpMatrix(gst.mtx[0][gst.mtxstackp[0]]);
        }
        logd("\n{ Active Xform:");
        DumpMatrix(gst.xform);
    }
}

// G_MW_MATRIX (gSPInsertMatrix): overwrite half of two elements of the
// combined matrix, which the RSP keeps as s15.16 with the 16 integer parts
// at 0x00..0x1F and the 16 fractions at 0x20..0x3F. The high 16 bits go to
// element n, the low to n+1 (GLideN64 gSPInsertMatrix). HAL's engine (Smash
// Bros, Kirby 64) scales characters this way; skipped, they were drawn thin.
// The next G_MTX recomputes xform from the stacks, as the RSP does.
void g_insertmtx(int where,dword x)
{
    int i,n;
    if((where&3) || where>=0x40) return;
    g_mtxxform(); // xform up to date first
    n=(where&0x1f)>>1;
    for(i=0;i<2;i++)
    {
        int32_t e=(int32_t)(gst.xform[n+i]*65536.0f);
        dword   v=i?(x&0xffff):(x>>16);
        if(where<0x20) e=(int32_t)(((dword)v<<16)|((dword)e&0xffff));
        else           e=(int32_t)(((dword)e&0xffff0000)|v);
        gst.xform[n+i]=e*(1.0f/65536.0f);
    }
    if(st.dumpgfx)
    {
        logd(" insertmatrix @%02X=%08X",where,x);
        DumpMatrix(gst.xform);
    }
}

void g_loadmtx(dword pos,int proj,int load,int push)
{
    ushort sh[32];
    float  m[16],m2[16];
    int    mi[16];
    int    i,x;

    proj=(proj!=0);
    if(load) logd(" Load:"); else logd(" Mul:");
    if(proj) logd("Prj ");  else logd("Mod ");
    if(push) logd("Push ");

    gst.newmatrices=1;
    gst.lightnumchanged=1;

    for(i=0;i<32;i+=2)
    {
        x=mem_read32p(pos+i*2);
        sh[i+0]=x>>16;
        sh[i+1]=x;
    }

    for(i=0;i<16;i++)
    {
        mi[i]=((int)sh[i]<<16) | (int)sh[i+16];
        m[i]=(float)(mi[i])*(1.0/65536.0);
    }

    /*
    if(gst.mtxstackp[proj]>1)
    {
        int x,y;
        print("4x %08X\n",pos);
        for(y=0;y<3;y++) for(x=0;x<3;x++)
        {
            m[x+y*4]*=4.0;
        }
    }
    */

    if(st.dumpgfx)
    {
        if(load) logd("\nLoad matrix(%i):",proj);
        else     logd("\nMul matrix(%i):",proj);
        if(push) logd(" (push)");
        DumpMatrix(m);
    }
    /*
    if(proj)
    {
        print("-proj-%08X-\n",pos);
        PrintMatrix(m);
    }
    */

    if(push)
    {
        gst.mtxstackp[proj]++;
        if(gst.mtxstackp[proj]>15)
        {
            gst.mtxstackp[proj]--;
            error("dlist: matrix stack overflow");
        }
        memcpy(gst.mtx[proj]+gst.mtxstackp[proj],
               gst.mtx[proj]+gst.mtxstackp[proj]-1,16*sizeof(float));
    }

    if(load)
    {
        memcpy(gst.mtx[proj]+gst.mtxstackp[proj],m,16*sizeof(float));
    }
    else
    {
        memcpy(m2,gst.mtx[proj][gst.mtxstackp[proj]],16*sizeof(float));
        MultMatrix(gst.mtx[proj][gst.mtxstackp[proj]],m,m2);
    }
}

// G_MV_MATRIX (gSPForceMatrix): the game hands over the combined matrix
// itself, 64 bytes of s15.16 like a G_MTX, and the G_MW_FORCEMTX after it
// keeps the RSP from recomputing it. Star Wars Episode I Racer builds every
// object's matrix on the CPU; its projection on the stack has no camera
// position, so from the stacks the scene was drawn off screen.
// The next G_MTX or G_POPMTX recomputes xform from the stacks, as the RSP does.
void g_forcemtx(dword pos)
{
    ushort sh[32];
    int    i,x;

    for(i=0;i<32;i+=2)
    {
        x=mem_read32p(pos+i*2);
        sh[i+0]=x>>16;
        sh[i+1]=x;
    }
    for(i=0;i<16;i++)
    {
        gst.xform[i]=(float)(((int)sh[i]<<16) | (int)sh[i+16])*(1.0/65536.0);
    }
    gst.newmatrices=0;

    if(st.dumpgfx)
    {
        logd(" forcematrix\n{ Forced Xform:");
        DumpMatrix(gst.xform);
    }
}

void g_popmtx(int num)
{
    int proj=0;
    if(st.dumpgfx) logd("(stackp=%i) ",gst.mtxstackp[0]);
    gst.newmatrices=1;
    gst.mtxstackp[proj]--;
    if(gst.mtxstackp[proj]<0)
    {
        gst.mtxstackp[proj]=0;
        error("dlist: matrix stack underflow");
    }
}

/***********************************************************************/

static struct
{
    int   cmd;
    char *name;
} cmdnames[]={
/* RDP commands: */
{0xff,"G_SETCIMG"},
{0xfe,"G_SETZIMG"},
{0xfd,"G_SETTIMG"},
{0xfc,"G_SETCOMBINE"},
{0xfb,"G_SETENVCOLOR"},
{0xfa,"G_SETPRIMCOLOR"},
{0xf9,"G_SETBLENDCOLOR"},
{0xf8,"G_SETFOGCOLOR"},
{0xf7,"G_SETFILLCOLOR"},
{0xf6,"G_FILLRECT"},
{0xf5,"G_SETTILE"},
{0xf4,"G_LOADTILE"},
{0xf3,"G_LOADBLOCK"},
{0xf2,"G_SETTILESIZE"},
{0xf1,"G_RDPHALF_RDP"},
{0xf0,"G_LOADTLUT"},
{0xef,"G_RDPSETOTHERMODE"},
{0xee,"G_SETPRIMDEPTH"},
{0xed,"G_SETSCISSOR"},
{0xec,"G_SETCONVERT"},
{0xeb,"G_SETKEYR"},
{0xea,"G_SETKEYGB"},
{0xe9,"G_RDPFULLSYNC"},
{0xe8,"G_RDPTILESYNC"},
{0xe7,"G_RDPPIPESYNC"},
{0xe6,"G_RDPLOADSYNC"},
{0xe5,"G_TEXRECTFLIP"},
{0xe4,"G_TEXRECT"},
/* ZELDA RSP commands: */
{0xe3,"Zelda_SETOTHERMODEH"},
{0xe2,"Zelda_SETOTHERMODEL"},
{0xe1,"Zelda_LOADRSPDATA?"},          // LOAD RSP-DATA TO 0x000
{0xe0,"Zelda_???118c"},
{0xdf,"Zelda_ENDDL"},
{0xde,"Zelda_DL"},
{0xdd,"Zelda_LOADRSPCODE?"},          // LOAD RSP-CODE TO 0x080
{0xdc,"Zelda_MOVEMEM"},
{0xdb,"Zelda_MOVEWORD"},
{0xda,"Zelda_LOADMTX"},
{0xd9,"Zelda_SETGEOMETRYMODE"},
{0xd8,"Zelda_POPMTX"},
{0xd7,"Zelda_TEXTURE"},
{0xd6,"Zelda_???11b8"},
{0xd5,"Zelda_???118c"},
{0xd4,"Zelda_???118c"},
{0xd3,"Zelda_???118c"},
{0xd2,"Zelda_???1188"},
{0xd1,"Zelda_???1078"},
{0xd0,"Zelda_???1188"},
/* RDP triangles */
{0xcf,"RAWTRI_SHADE_TXTR_ZBUF"},
{0xce,"RAWTRI_SHADE_TXTR"},
{0xcd,"RAWTRI_SHADE_ZBUF"},
{0xcc,"RAWTRI_SHADE"},
{0xcb,"RAWTRI_TXTR_ZBUF"},
{0xca,"RAWTRI_TXTR"},
{0xc9,"RAWTRI_FILL_ZBUF"},
{0xc8,"RAWTRI_FILL"},
{0xc7,"RAWTRI_EDGE_SHADE_TXTR_ZBUF"},
{0xc6,"RAWTRI_EDGE_SHADE_TXTR"},
{0xc5,"RAWTRI_EDGE_SHADE_ZBUF"},
{0xc4,"RAWTRI_EDGE_SHADE"},
{0xc3,"RAWTRI_EDGE_TXTR_ZBUF"},
{0xc2,"RAWTRI_EDGE_TXTR"},
{0xc1,"RAWTRI_EDGE_FILL_ZBUF"},
{0xc0,"G_NOOP"},
/* IMMEDIATE commands: */
{0xbf,"Zelda_?16f (tri1?)"},
{0xbe,"Zelda_?1018"},
{0xbd,"Zelda_?0"},
{0xbc,"Zelda_?1000"},
{0xbb,"Zelda_?97"},
{0xba,"Zelda_?f80"},
/* IMMEDIATE commands: */
{0xbf,"G_TRI1"},
{0xbe,"G_CULLDL"},
{0xbd,"G_POPMTX"},
{0xbc,"G_MOVEWORD"},
{0xbb,"G_TEXTURE"},
{0xba,"G_SETOTHERMODE_H"},
{0xb9,"G_SETOTHERMODE_L"},
{0xb8,"G_ENDDL"},
{0xb7,"G_SETGEOMETRYMODE"},
{0xb6,"G_CLEARGEOMETRYMODE"},
{0xb5,"G_LINE3D"},
{0xb4,"G_RDPHALF_1"},
{0xb3,"G_RDPHALF_2"},
{0xb2,"G_RDPHALF_CONT"},
{0xb1,"G_TRI2 (WAVE)"},
/* Lowops Zelda */
{0x0A,"Zelda_BACKGROUND"},
{0x09,"Zelda_???"},
{0x08,"Zelda_???118c"},
{0x07,"Zelda_TRI3"},
{0x06,"Zelda_TRI2"},
{0x05,"Zelda_TRI1"},
{0x04,"Zelda_DLINMEM"},
{0x03,"Zelda_CULLDL"},
{0x02,"Zelda_???1c74"},
{0x01,"Zelda_LOADVTX"},
{0x00,"Zelda_0"},
/* DMA */
{0x09,"G_SPRITE2D"},
{0x07,"G_DLINMEM"},
//{0x08,"G_RESERVED3"},
//{0x07,"G_RESERVED2"},
{0x06,"G_DL"},
{0x05,"G_DMATRI"},
{0x04,"G_VTX"},
{0x03,"G_MOVEMEM"},
//{0x02,"G_RESERVED0"},
{0x01,"G_MTX"},
{0x00,"G_SPNOOP"},
0,NULL};

void dumpcmd(dword addr,dword *cmd)
{
    int j;
    int c=cmd[0]>>24;

    logd("%08X: %08X %08X CMD ",dlpnt,cmd[0],cmd[1]);
    if(cart.iszelda)
    {
        for(j=0;cmdnames[j].name;j++)
        {
            if(cmdnames[j].cmd==c) break;
        }
    }
    else
    {
        for(j=0;cmdnames[j].name;j++)
        {
            if(cmdnames[j].cmd==c && *cmdnames[j].name!='Z') break;
        }
    }
    if(!cmdnames[j].name) logd("??? ");
    else logd("%s ",cmdnames[j].name);
}

/***********************************************************************/

static __inline dword address(dword address)
{ // segment convert to physical address: the RSP uses the low 4 bits of
  // the segment byte and 24 bits of the sum (GLideN64 RSP_SegmentToPhysical).
  // Gauntlet Legends passes its TLB addresses (E0xxxxxx) and segment bases.
    int seg=(address>>24)&0x0f;
    return( ((address&0xffffff) + gst.segment[seg]) & 0xffffff );
}

/***********************************************************************/

void dump_geom(dword x)
{
    logd("\n*mode geometry: %08X ",x);

    logd(" texgen(%i)",gst.texgen);
    logd(" flat(%i)",gst.flat);
    logd(" cull(%i)",gst.cull);
    logd(" light(%i)",gst.lightvx);
}

void change_geom(dword x)
{
    if(cart.dlist_zelda==1)
    {
        gst.cull   =(x>>9)&3;
        gst.lightvx=(x&0x20000)?1:0; // 10000
        gst.texgen =(x&0xc0000)>>18; // G_TEXTURE_GEN, G_TEXTURE_GEN_LINEAR
        gst.flat   =(x&0x80000)?1:0; //��
        if(!(x&4)) gst.flat=0;
        rdp_flat(gst.flat);
    }
    else
    {
        gst.cull   =(x&0x03000)>>12;
        gst.lightvx=(x&0x20000)?1:0;
        gst.texgen =(x&0xc0000)>>18;
        gst.flat   =0;
    }
    if(gst.lastframehadbackground && !gst.framehadbackground)
    {
        gst.lightvx=0;
    }
}

/***********************************************************************/
// command helpers

void c_dlbranch(dword a,int branch,int limit)
{
    dword addr=address(a);
    if(limit) dllimit=limit+1;
    logd(" Displaylist at %08X (stackp %i, limit %i)",addr,stackp,limit);
    if(branch)
    { // branch
        logd(" (branch)");
        dlpnt=addr;
    }
    else
    { // call
        stack[stackp++]=dlpnt;
        dlpnt=addr;
    }
}

// Cruis'n Exotica's F3DEX2 (same version string as the stock 2.08) is patched:
// D5 (G_SPECIAL_1, a no-op in the stock one) copies (w0&0xffffff) commands
// from the physical address in w1, no segment, into DMEM and runs them before
// the list goes on. Nearly all of the game's geometry is in such lists. A D5
// inside one replaces what was left of it.
void c_dlinline(dword w0,dword w1)
{
    int   count=w0&0xffffff;
    dword addr=w1&0xffffff;
    logd(" Inline list at %08X, %i commands",addr,count);
    if(!count) return;
    if(dllimit<=0) stack[stackp++]=dlpnt;
    dlpnt=addr;
    dllimit=count+1;
}

void c_dlend(void)
{
    dlpnt=stack[--stackp];
    logd(" Displaylist end (stackp %i)\n",stackp);
}

// G_MODIFYVTX (F3DEX B2, F3DEX2 02; GLideN64 gSPModifyVertex): overwrite
// one field of loaded vertex (w0&0xffff)/2. The vertex is initialized
// (scaled) first. Wipeout 64 sets its texture coordinates this way: without
// it they flickered between stale values.
// The ST goes in as it is: the microcode writes the word into its vertex
// buffer, where a loaded vertex's ST is already scaled (GLideN64 divides by
// the scale it applies later). Top Gear Overdrive gives 0..64 for its 64
// texel menu tiles at G_TEXTURE scale 0.5, Cruis'n USA 0..31.5 for 32 texel
// pieces; scaled again, the changed corners sampled half way and every
// tile was sheared. Chef's Luv Shack's 0..127 for 64 texels is halved by
// its texture perspective being off (rdp.c tri_texhalf), not by the scale.
// A G_TEXTURE scale of 0 does not matter here either (Harvest Moon 64's
// ground tiles).
void c_modifyvtx2(int i,int where,dword val)
{
    Vertex *v;

    if(i<0 || i>=MAXRDPVX) return;
    if(!(rdpvxflag[i]&VX_INITDONE)) g_initvx(i);
    // triangles queued so far keep the vertex as it was (All-Star Baseball
    // 2001 changes ST between triangles sharing a vertex: grass and crowd
    // stretched toward the new coordinate)
    rdp_dupvtx(i);
    v=rdpvx[i];
    switch(where)
    {
    case 0x10: // G_MWO_POINT_RGBA
        v->icol=val;
        v->col[0]=((val>>24)&255)*(1.0/256.0);
        v->col[1]=((val>>16)&255)*(1.0/256.0);
        v->col[2]=((val>> 8)&255)*(1.0/256.0);
        v->col[3]=((val    )&255)*(1.0/256.0);
        logd(" modifyvtx %i rgba %08X",i,val);
        break;
    case 0x14: // G_MWO_POINT_ST (s10.5)
        v->tex[0]=(float)(short)(val>>16);
        v->tex[1]=(float)(short)val;
        logd(" modifyvtx %i st %.2f %.2f",i,v->tex[0]/32,v->tex[1]/32);
        break;
    case 0x18: // G_MWO_POINT_XYSCREEN (s13.2 screen pixels)
        // back through the viewport to clip space at the vertex's own w
        // (GLideN64 gSPModifyVertex). South Park: Chef's Luv Shack places
        // every sprite this way: skipped, all of them sat at the centre.
        if(gst.vp[0]==0 || gst.vp[1]==0) break;
        v->pos[0]= ((short)(val>>16)*0.25f-gst.vp[2])/gst.vp[0]*v->pos[2];
        v->pos[1]=-((short)val      *0.25f-gst.vp[3])/gst.vp[1]*v->pos[2];
        rdpvxflag[i]&=~(VX_CLIPX1|VX_CLIPX2|VX_CLIPY1|VX_CLIPY2);
        logd(" modifyvtx %i xyscreen %.2f %.2f",i,(short)(val>>16)*0.25f,(short)val*0.25f);
        break;
    case 0x1C: // G_MWO_POINT_ZSCREEN (upper half: screen z, 0x7FFF = far)
        {
            float z=(short)(val>>16)*(1.0f/32768.0f);
            v->zs=z<0?0:(z>1?1:z);
            logd(" modifyvtx %i zscreen %.4f",i,v->zs);
        }
        break;
    default:
        {
            static int warned;
            if(!warned++) warning("dlist: G_MODIFYVTX %02X not supported",where);
            logd(" modifyvtx %i where %02X (skipped)",i,where);
        }
        break;
    }
}

void c_modifyvtx(void)
{
    c_modifyvtx2((cmd[0]&0xffff)>>1,(cmd[0]>>16)&0xff,cmd[1]);
}

void c_dmavtx(dword a,int v0,int vn)
{
    dword addr=address(a);

    if(cart.dlist_diddlyvx) addr+=gst.dkr_vtxbase;

    logd(" Vertex %02i..%02i at %08X",v0,v0+vn-1,addr);
    if(v0<0 || vn<1 || v0+vn>MAXVX || vn>MAXVX)
    {
        logd(" ERROR v0=%i vn=%i ",v0,vn);
        error("dlist: invalid vertex load");
        return;
    }

    if(cart.dlist_diddlyvx) g_loadvtx_diddly(addr,v0,vn);
    else g_loadvtx(addr,v0,vn);

    gst.cnt_vtx+=vn;
    if(!gst.newvertices)
    {
        gst.newvertices=1;
        gst.newvertices_lo=v0;
        gst.newvertices_hi=vn+v0-1;
    }
    else
    {
        int a;
        a=v0;
        if(a<gst.newvertices_lo) gst.newvertices_lo=a;
        if(a>gst.newvertices_hi) gst.newvertices_hi=a;
        a=v0+vn-1;
        if(a<gst.newvertices_lo) gst.newvertices_lo=a;
        if(a>gst.newvertices_hi) gst.newvertices_hi=a;
    }
}

void c_dmatri_diddly(dword a,int vn)
{
    dword addr=address(a);
    int min=999,max=-999;

    gst.texenable=1;
    gst.scales=1.0;
    gst.scalet=1.0;

    logd(" Triangles %i at %08X",vn,addr);
    while(vn-->0)
    {
        dword iw;
        int vxind[3];
        iw=mem_read32(addr+0);
        vxind[2]=(iw>>0)&255;
        vxind[1]=(iw>>8)&255;
        vxind[0]=(iw>>16)&255;
        if(vxind[0]>=MAXRDPVX) return;
        if(vxind[1]>=MAXRDPVX) return;
        if(vxind[2]>=MAXRDPVX) return;
        logd("\ntri[ %i %i %i ]",vxind[0],vxind[1],vxind[2]);
        // setup texcoords
        iw=mem_read32(addr+4);
        rdpvx[vxind[0]]->tex[0]=(short)(iw>>16);
        rdpvx[vxind[0]]->tex[1]=(short)(iw);
        iw=mem_read32(addr+8);
        rdpvx[vxind[1]]->tex[0]=(short)(iw>>16);
        rdpvx[vxind[1]]->tex[1]=(short)(iw);
        iw=mem_read32(addr+12);
        rdpvx[vxind[2]]->tex[0]=(short)(iw>>16);
        rdpvx[vxind[2]]->tex[1]=(short)(iw);
        // draw
        g_tri(0,vxind);
        addr+=16;
/*
        {
            int i;
            for(i=0;i<3;i++)
            {
                if(vxind[i]<min) min=vxind[i];
                if(vxind[i]>max) max=vxind[i];
            }
        }
*/
    }
//    logd("\nG_VTX-DMATRI-Range %i..%i",min,max);
}

void c_dmamtx(dword a,int proj,int load,int push)
{
    dword addr=address(a);
    logd(" {Matrix} at %08X ",addr);
    g_loadmtx(addr,proj,load,push);
    gst.cnt_mtx++;
}

void c_dmamtx_diddly(dword a,int ind)
{
    dword addr=address(a)+gst.dkr_mtxbase;
    logd(" {Matrix} at %08X ind %i ",addr,ind);
    gst.mtxstackp[0]=ind;
    g_loadmtx(addr,0,1,0);
    gst.cnt_mtx++;
}

void c_setgeommode(int mode,dword bits)
{
    if(mode==2)   gst.gmode&=bits;  // 2=AND
    else if(mode) gst.gmode|=bits;  // 1=OR
    else          gst.gmode&=~bits; // 2=AND NOT

    if(mode!=2)
    {
        change_geom(gst.gmode);
        if(st.dumpgfx) dump_geom(gst.gmode);
    }
}

void c_setothermodemask(int hi,dword *cmd,int mask);

void c_setothermode(int hi,dword *cmd)
{
    int pos,bits,mask,data;

    pos=(cmd[0]>>8)&31;
    bits=(cmd[0])&31;
    mask=((1<<bits)-1)<<pos;
    c_setothermodemask(hi,cmd,mask);
}

// F3DEX2 packs the field as (32-shift-len, len-1) instead of (shift, len)
void c_setothermode2(int hi,dword *cmd)
{
    int pos,bits,mask;

    bits=(cmd[0]&0xff)+1;
    pos=32-((cmd[0]>>8)&0xff)-bits;
    if(bits>=32 || pos<0) mask=-1;
    else mask=((1<<bits)-1)<<pos;
    c_setothermodemask(hi,cmd,mask);
}

void c_setothermodemask(int hi,dword *cmd,int mask)
{
    int data;

    data=cmd[1];

    if(hi)
    {
        gst.omodeh&=~mask;
        gst.omodeh|=data;
    }
    else
    {
        gst.omodel&=~mask;
        gst.omodel|=data;
    }

    // generate RDP othermode change command
    cmd[0]=0xef000000+(gst.omodeh&0xffffff);
    cmd[1]=gst.omodel;
    rdp_cmd(cmd);
}

void c_movemem(int ind,dword a)
{
    dword addr=address(a);
    int i;

    logd(" Movemem[%04X] <- %08X",ind,a);

    if(ind>=0x86 && ind<=0x94)
    {
        i=(ind-0x86)/2;
        g_loadlight(i,addr);
    }
    else if(ind==0x080)
    { // viewport
        g_viewport(addr);
    }

    logd("\ndata(%08X): ",addr);
    for(i=0;i<4;i++)
    {
        logd("%08X ",mem_read32p(addr+i*4));
    }
}

void c_movemem_zelda(int ind,dword a)
{
    dword addr=address(a);
    int i;

    logd(" Movemem[%04X] <- %08X",ind,a);

    if(gst.cbfd && (cmd[0]&0xff)==10)
    { // Conker's G_MV_LIGHT: 48 bytes a light, the first two are look-at
        int n=((cmd[0]>>5)&0x3fff)/48;
        if(n>=2) g_cbfdloadlight(n-2,addr);
    }
    else if(gst.cbfd && (cmd[0]&0xff)==14)
    { // Conker's G_MV_NORMALES
        gst.cbfdnormals=addr;
        logd(" normals at %08X",addr);
    }
    else if((cmd[0]&0xff)==14)
    { // G_MV_MATRIX
        g_forcemtx(addr);
    }
    else if((cmd[0]&0xff)==10 && (ind==0x0800 || ind==0x0803))
    { // G_MV_LIGHT slots 0 and 1: the look-at vectors for texture
      // generation (direction at +8, as in a light)
        int   n=ind==0x0803;
        dword d=mem_read32p(addr+8);
        float x=(float)(char)(d>>24);
        float y=(float)(char)(d>>16);
        float z=(float)(char)(d>>8);
        float len=sqrtf(x*x+y*y+z*z);
        if(len>0.0f) { x/=len; y/=len; z/=len; }
        gst.lookat[n][0]=x;
        gst.lookat[n][1]=y;
        gst.lookat[n][2]=z;
        gst.lookaton=!n || x!=0.0f || y!=0.0f; // GLideN64 gSPLookAt
        logd(" lookat %i: %.2f %.2f %.2f",n,x,y,z);
    }
    else if(ind>=0x0806 && ind<=0x0806+3*8)
    {
        i=(ind-0x0806)/3;
        g_loadlight(i,addr);
    }
    else if(ind==0x0800 && (cmd[0]&0xffffff)==0x080008)
    { // viewport
        g_viewport(addr);
    }

    logd("\ndata(%08X): ",addr);
    for(i=0;i<4;i++)
    {
        logd("%08X ",mem_read32p(addr+i*4));
    }
}

void c_moveword(int bank,int index,dword x)
{
    logd(" Mem[%i][%02X]=%08X",bank,index,x);
    if(cart.dlist_diddlyvx && bank==0x0a)
    {
        gst.mtxstackp[0]=(x>>6)&3;
        gst.newmatrices=1;
        return;
    }
    if(cart.dlist_diddlyvx && bank==0x02)
    {
        gst.dkr_billboard=x&1;
        return;
    }
    if(bank==0xa)
    { // G_MW_LIGHTCOL: lights are 24 bytes apart in F3DEX2, 32 in F3D;
      // the word at +4 is the copy of the color (same value)
        int stride=(cart.dlist_zelda==1)?24:32;
        if(index%stride==0 && index/stride<8)
        {
            Light *l=gst.light+index/stride;
            l->data[0]=x;
            setcolor2(l->col,x);
            logd(" Lightcol[%i]=%08X",index/stride,x);
        }
        return;
    }
    if(bank==0x0 && !cart.dlist_diddlyvx)
    { // G_MW_MATRIX; index is the byte offset in both F3D and F3DEX2
        g_insertmtx(index,x);
        return;
    }
    index>>=2;
    if(bank==0x6)
    {
        logd(" Segment[%i]=%08X",index,x);
        gst.segment[index]=x;
        rdp_segment(index,x);
    }
    else if(bank==0x8 && index==0)
    {
        int a,b,min,max;
        float scalen,scalef;
        // fog
        a=(x>>24)&255;
        b=(x>>8)&255;

        min=b-a;
        max=b+a;
        gst.fognear=min/256.0;
        gst.fogfar=max/256.0;

        logd(" Fogrange %.3f..%.3f zmax=%f",gst.fognear,gst.fogfar,gst.zmax);
        if(cart.iszelda)
        {
            scalen=1024.0;
            scalef=4096.0;
        }
        else
        {
            scalen=16384;
            scalef=16384;
        }
        rdp_fogrange(gst.fognear*scalen,gst.fogfar*scalef);
    }
    else if(bank==0x2 && index==0)
    {
        int lightnum;
        if(cart.dlist_zelda==1)
        { // F3DEX2: NUML(n) = n*24
            lightnum=(cmd[1]&0xfff)/24;
        }
        else
        {
            lightnum=(cmd[1]&0xfff)/32-1;
        }
        g_lightnum(lightnum);
    }
    else
    {
        logd(" !skipmoveword");
    }
}

void c_texture(dword *cmd)
{
    int bowtie,level,tile,on,s,t;
    bowtie=FIELD(cmd[0],16,8);
    level=FIELD(cmd[0],11,3);
    tile=FIELD(cmd[0],8,3);
    on=FIELD(cmd[0],0,8);
    s=FIELD(cmd[1],16,16);
    t=FIELD(cmd[1],0,16);
    gst.scales=s*(1.0/65536.0);
    gst.scalet=t*(1.0/65536.0);
    gst.texenable=on;
    logd(" texture on=%i tile=%i s=%4.2f t=%4.2f",
        on,tile,gst.scales,gst.scalet);
    rdp_texture(on,tile,level);
}

void c_zeldabackground(dword addr)
{
    dword wid,hig,base;
    static int saved=0;
    wid=mem_read32p(addr)/4;
    hig=mem_read32p(addr+8)/4;
    base=mem_read32p(addr+16);
    base=address(base);
    logd("\n+background: %08X size(%i,%i)",base,wid,hig);
//    print("Background: %08X size(%i,%i)\n",base,wid,hig);

    /*
    if(!saved)
    {
        FILE *f1;
        int i;
        dword x;
        f1=fopen("bg.dat","wb");
        for(i=0;i<320*240*4;i+=4)
        {
            x=mem_read32p(i+base);
            x=FLIP32(x);
            fwrite(&x,1,4,f1);
        }
        fclose(f1);
        saved=1;
    }
    */

    rdp_copybackground(base,wid,hig);
}

/***********************************************************************/

void dlisterror(void)
{
    int c=(cmd[0]>>24);
    errors++;
    gst.errorcnt[c]++;
    if(gst.errorcnt[c]<10)
    {
        error("dlist: unknown command %08X %08X at %08X",cmd[0],cmd[1],dlpnt);
    }
    if(!dl_histshown && dl_histdumps<4)
    {
        int i,n=dl_histn<DL_HIST?dl_histn:DL_HIST;
        dl_histshown=1;
        dl_histdumps++;
        print("dlist: the %i commands up to the unknown one (stack depth %i):\n",n,stackp);
        for(i=dl_histn-n;i<dl_histn;i++)
        {
            dword *h=dl_hist[i%DL_HIST];
            print("  %08X: %08X %08X\n",h[0],h[1],h[2]);
        }
        for(i=1;i<(int)stackp && i<8;i++) print("  return %i: %08X\n",i,stack[i]);
        for(i=0;i<16;i++) if(gst.segment[i]) print("  segment %X = %08X\n",i,gst.segment[i]);
    }
}

// consecutive G_NOOPs; any other command resets it. Only a run of them
// means the list ran into unwritten memory (Paper Mario uses single NOPs)
static int zerocnt;

static int  rsp_cmd_s2dex1(int c);
static void c_loaducode(void);

// Fast3D Sprite2D (GLideN64 gSPSprite2DBase): 09 SPRITE2D_BASE points at a
// uSprite (image, TLUT, stride, size, format, offset in the image); the BE
// (SCALEFLIP) and BD (DRAW, at a screen position) that follow it are not
// CULLDL and POPMTX. The image is read from RDRAM like an S2DEX background.
// Wipeout 64 draws its intro screens this way.
static void c_sprite2dbase(void)
{
    dword a=address(cmd[1]);
    dword tlut=mem_read32p(a+4);
    gst.sprite2d=a;
    gst.sprscalex=gst.sprscaley=1.0f;
    gst.sprflip=0;
    logd(" sprite2d base %08X",a);
    if(tlut) rdp_settlut(0,256,address(tlut));
}

static void c_sprite2ddraw(void)
{
    dword a    =gst.sprite2d;
    dword image=address(mem_read32p(a+0));
    int   stride=(short)mem_read16(a+8);
    int   imagew=(short)mem_read16(a+10);
    int   imageh=(short)mem_read16(a+12);
    int   fmt  =mem_read8(a+14);
    int   siz  =mem_read8(a+15);
    int   imagex=(short)mem_read16(a+16);
    int   imagey=(short)mem_read16(a+18);
    float frx  =(short)(cmd[1]>>16)/4.0f;
    float fry  =(short)cmd[1]/4.0f;

    logd(" sprite2d draw %.2f,%.2f %ix%i of %08X stride %i at %i,%i fmt %i/%i",
        frx,fry,imagew,imageh,image,stride,imagex,imagey,fmt,siz);
    if(stride<=0 || imagew<=0 || imageh<=0) return;
    // only the sprite's rows: start the image at row imagey
    image+=((imagey*stride)<<siz)>>1;
    rdp_spriteimage(image,fmt,siz,stride,imageh,(float)imagex,frx,fry,
        imagew/gst.sprscalex,imageh/gst.sprscaley,gst.sprscalex,gst.sprscaley,gst.sprflip);
}

void rsp_cmd_basic(int c)
{
    if(gst.s2dex && rsp_cmd_s2dex1(c)) return;
    if(gst.sprite2d)
    {
        if(c==0xBE) // SPRITE2D_SCALEFLIP
        {
            gst.sprscalex=(cmd[1]>>16)/1024.0f;
            gst.sprscaley=(cmd[1]&0xffff)/1024.0f;
            if(gst.sprscalex<=0) gst.sprscalex=1.0f;
            if(gst.sprscaley<=0) gst.sprscaley=1.0f;
            gst.sprflip=(((cmd[0]>>8)&0xff)?1:0)|((cmd[0]&0xff)?2:0);
            logd(" sprite2d scaleflip %.3f,%.3f flip %i",gst.sprscalex,gst.sprscaley,gst.sprflip);
            return;
        }
        if(c==0xBD) { c_sprite2ddraw(); return; } // SPRITE2D_DRAW
        gst.sprite2d=0;
    }
    switch(c)
    {
    case 0x09: // SPRITE2D_BASE (Fast3D)
        c_sprite2dbase();
        break;
    case 0xB2: // G_MODIFYVTX (F3DEX 1.x; Fast3D's RDPHALF_CONT is only
               // ever consumed as texrect data by the RDP path)
        if(!cart.dlist_wavevx) c_modifyvtx();
        break;
    case 0xB4: // RDPHALF_1: G_LOAD_UCODE's data address
        gst.loadrspdata=address(cmd[1]);
        logd(" data at %08X",gst.loadrspdata);
        break;
    case 0xAF: // G_LOAD_UCODE (F3DEX 1.x)
        c_loaducode();
        break;
    case 0x00:
        {
            zerocnt++;
            if(zerocnt>4)
            {
                zerocnt=0;
                warning("dlist: unexpected stream of nops, aborting list.");
                c_dlend();
            }
        }
        break;
//------------------------------------------ vertices
    case 0x05: // DMA triangles
        {
            int vn;
            cart.dlist_diddlyvx=1;
            vn=((cmd[0]>>20)&15)+1;
            c_dmatri_diddly(cmd[1],vn);
            gst.dkr_vertexi=0;
        }
        break;
//------------------------------------------ vertices
    case 0x04: // DMA vertex vtx
        {
            int v0,vn;
            if(cart.dlist_diddlyvx)
            {
                if(!(cmd[0]&0x10000)) gst.dkr_vertexi=0;
                else if(gst.dkr_billboard) gst.dkr_vertexi=1;
                v0=gst.dkr_vertexi+((cmd[0]>>9)&31);
                vn=((cmd[0]>>19)&31)+1;
                gst.dkr_vertexi+=vn;
            }
            else if(!cart.dlist_wavevx)
            {
                v0=((cmd[0]>>16)&0xff)>>1;
                vn=((cmd[0]>> 8)&0xff)>>2;
                if(!vn) cart.dlist_wavevx=1;
            }
            else
            {
                // Fast3D: (n-1)<<20 | v0<<16 | 16n.
                // Wave Race 64 (US) ships a Fast3D with the same version
                // string but n<<9 | (v0*5)<<16 | 16n-1, and all triangle
                // indices *5 instead of *10 (GLideN64's F3DBETA). The two
                // encodings never satisfy each other's length check.
                if(!cart.dlist_wrusvx &&
                   (cmd[0]&0xffff)!=16*(((cmd[0]>>20)&0xf)+1) &&
                   ((cmd[0]&0x1ff)+1)==16*((cmd[0]>>9)&0x7f))
                {
                    cart.dlist_wrusvx=1;
                    logd(" [Wave Race vertex format]");
                }
                if(cart.dlist_wrusvx)
                {
                    v0=((cmd[0]>>16)&0xff)/5;
                    vn=((cmd[0]>>9)&0x7f);
                }
                else
                {
                    v0=((cmd[0]>>16)&0xf);
                    vn=((cmd[0]>>20)&0xf)+1;
                }
            }
            c_dmavtx(cmd[1],v0,vn);
        }
        break;
//------------------------------------------ matrices
    case 0x01: // DMA matrix
        {
            int pos,load,push,proj,a;
            pos=address(cmd[1]);
            if(cart.dlist_diddlyvx)
            {
                a=((cmd[0]>>20)&0xf);
                proj=!(a&0x8);
                load=!(a&0x4);
                push=0;
                c_dmamtx_diddly(cmd[1],a/4);
            }
            else
            {
                a=(cmd[0]>>16)&0xf;
                proj=(a&0x1);
                load=(a&0x2);
                push=(a&0x4);
                c_dmamtx(pos,proj,load,push);
            }
        }
        break;
    case 0xBD: // POPMATRIX
        g_popmtx(cmd[1]);
        if(st.dumpgfx) logd("{matrix}");
        break;
//------------------------------------------ drawing
    case 0xB5: // QUAD (mariokart)
        {
            int vxind2[4],i;
            int vxind[4];
            if(cart.dlist_wavevx)
            {
                int div=cart.dlist_wrusvx?5:10;
                for(i=0;i<4;i++)
                {
                    vxind2[i]=( (cmd[1]>>(24-i*8)) &255)/div;
                }
            }
            else
            { // waverace
                for(i=0;i<4;i++)
                {
                    vxind2[i]=( (cmd[1]>>(24-i*8)) &255) >> 1;
                }
            }

            vxind[0]=vxind2[3];
            vxind[1]=vxind2[2];
            vxind[2]=vxind2[1];
            vxind[3]=vxind2[0];

            g_tri(0,vxind);
            vxind[1]=vxind[2];
            vxind[2]=vxind[3];
            g_tri(0,vxind);
        }
        break;
    case 0xBF: // TRI1
        {
            int vxind[3],i;
            if(cart.dlist_diddlyvx)
            {
                gst.dkr_mtxbase=cmd[0]&0xffffff;
                gst.dkr_vtxbase=cmd[1]&0xffffff;
                break;
            }
            if(cart.dlist_wavevx)
            {
                int div=cart.dlist_wrusvx?5:10;
                for(i=0;i<3;i++) vxind[i]=( (cmd[1]>>(i*8)) &255)/div;
            }
            else
            { // waverace
                for(i=0;i<3;i++) vxind[i]=( (cmd[1]>>(i*8)) &255) >> 1;
            }
            g_tri(0,vxind);
        }
        break;
    case 0xB1: // TRI2
        {
            int vxind[3];
            if(cart.dlist_wrusvx)
            { // wave race: two triangles, indices *5
                int i;
                for(i=0;i<3;i++) vxind[i]=( (cmd[0]>>(i*8)) &255)/5;
                g_tri(0,vxind);
                for(i=0;i<3;i++) vxind[i]=( (cmd[1]>>(i*8)) &255)/5;
                g_tri(0,vxind);
            }
            else if(cart.dlist_geyevx || (cmd[1]>>24))
            { // goldeneye
                cart.dlist_geyevx=1;

                vxind[1]=((cmd[1]>> 0)&15);
                vxind[0]=((cmd[1]>> 4)&15);
                if(vxind[0]|vxind[1])
                {
                    vxind[2]=((cmd[0]>> 0)&15);
                    g_tri(0,vxind);
                }

                vxind[1]=((cmd[1]>> 8)&15);
                vxind[0]=((cmd[1]>>12)&15);
                if(vxind[0]|vxind[1])
                {
                    vxind[2]=((cmd[0]>> 4)&15);
                    g_tri(0,vxind);
                }

                vxind[1]=((cmd[1]>>16)&15);
                vxind[0]=((cmd[1]>>20)&15);
                if(vxind[0]|vxind[1])
                {
                    vxind[2]=((cmd[0]>> 8)&15);
                    g_tri(0,vxind);
                }

                vxind[1]=((cmd[1]>>24)&15);
                vxind[0]=((cmd[1]>>28)&15);
                if(vxind[0]|vxind[1])
                {
                    vxind[2]=((cmd[0]>>12)&15);
                    g_tri(0,vxind);
                }
            }
            else
            {
                vxind[0]=((cmd[0]>> 0)&127) >> 1;
                vxind[1]=((cmd[0]>> 8)&127) >> 1;
                vxind[2]=((cmd[0]>>16)&127) >> 1;
                g_tri(0,vxind);
                vxind[0]=((cmd[1]>> 0)&127) >> 1;
                vxind[1]=((cmd[1]>> 8)&127) >> 1;
                vxind[2]=((cmd[1]>>16)&127) >> 1;
                g_tri(0,vxind);
            }
        }
        break;
//------------------------------------------ textures
    case 0xBB: // TEXTURE
        c_texture(cmd);
        break;
//-------------------------------------- displaylists
    case 0x06: // DL DMA displaylist
        c_dlbranch(cmd[1],cmd[0]&0x10000,0);
        break;
    case 0xB8: // DLEND
        c_dlend();
        break;
    case 0x07: // DLINMEM
        if(gst.pdvx)
        { // Perfect Dark: the table its vertices' color indices point into.
          // Followed as a list, the colors were read as commands (hundreds
          // of "unknown command" a frame, and the list was dropped)
            gst.vtxcolorbase=address(cmd[1]);
            logd(" vertex color base %08X",gst.vtxcolorbase);
            break;
        }
        {
            c_dlbranch(address(cmd[1]),0,(cmd[0]>>16)&255);
        } break;
//-------------------------------------- settings
    case 0xB6: // Geometrymode
    case 0xB7: // Geometrymode
        c_setgeommode(c==0xb7,cmd[1]);
        break;
    case 0xB9: // Othermode_l
        c_setothermode(0,cmd);
        break;
    case 0xBA: // Othermode_H
        c_setothermode(1,cmd);
        break;
    case 0x03: // MOVEMEM
        {
            int v0;
            v0=(cmd[0]>>16)&0xff;
            c_movemem(v0,cmd[1]);
        }
        break;
    case 0xBC: // MOVEWORD
        {
            int ind=(cmd[0]>>8)&255;
            int m=(cmd[0]>>0)&15;
            // G_MW_POINTS: Fast3D's gSPModifyVertex, a word written into the
            // RSP's vertex buffer (40 bytes a vertex, a 16-bit offset; the
            // fields are G_MODIFYVTX's). Cruis'n USA loads its quads with one
            // placeholder ST and gives each triangle its own this way:
            // skipped, its logo, track pictures and name plates were smears
            // of a single texel row or column.
            if(m==0x0c && !cart.dlist_diddlyvx)
            {
                int off=(cmd[0]>>8)&0xffff;
                logd(" Mem[12][%04X]=%08X",off,cmd[1]);
                c_modifyvtx2(off/40,off%40,cmd[1]);
                break;
            }
            c_moveword(m,ind,cmd[1]);
        }
        break;
    case 0xB0: // G_BRANCH_Z (F3DEX 1.x; GLideN64 gSPBranchLessZ)
        // a vertex nearer than the depth in w1's top half: branch to the
        // display list RDPHALF_1 gave (the model's near, detailed version).
        // Skipped, SF Rush drew every car with its far model and 16x8
        // textures.
        if(!cart.dlist_wavevx)
        {
            int   vi=(cmd[0]>>1)&0x7ff;
            float zs=vi<256?g_vxzs[vi]:-1.0f;
            logd(" branch_z vtx %i z %.1f <= %i ?",vi,zs,cmd[1]>>16);
            if(zs<0.0f || zs<=(float)(cmd[1]>>16)) c_dlbranch(gst.loadrspdata,1,0);
        }
        break;
    default:
        if((c>0x10 && c<0xa0) || !c) dlisterror();
        logd("!skip");
        break;
    }
}

static int ucode_findtext(dword addr,int size,char *text);

// S2DEX (2D sprite and background microcode), which F3DEX2 games switch to
// with G_LOAD_UCODE. Kirby 64 draws its Nintendo and HAL logos with
// G_OBJ_LOADTXTR (the palette) and G_BG_1CYC, its HUD with G_BG_COPY.
// Returns 0 for commands the F3DEX2 table handles (RDP ones, DL/ENDDL; a
// 0x0A outside S2DEX still goes to c_zeldabackground). Sprites aren't drawn.
// S2DEX background sizes are even, as GLideN64's _loadBGImage (imageW -
// imageW%2): Worms Armageddon gives its 320x240 screens as 321x241, and a
// 321 texel row stride sheared them left, wrapping round the right edge
#define BGSIZE(a) ((mem_read16(a)/4)&~1) // u10.2

// G_BG_1CYC (uObjScaleBg)
static void c_bg1cyc(dword a)
{
    dword image=address(mem_read32p(a+16));
    logd(" bg1cyc at %08X",a);
    rdp_bgimage(image,mem_read8(a+22),mem_read8(a+23),mem_read16(a+24),
        BGSIZE(a+2),BGSIZE(a+10),                                // imageW,H
        mem_read16(a+0)/32.0f,mem_read16(a+8)/32.0f,             // imageX,Y (u10.5)
        (short)mem_read16(a+4)/4.0f,(short)mem_read16(a+12)/4.0f, // frameX,Y (s10.2)
        mem_read16(a+6)/4.0f,mem_read16(a+14)/4.0f,              // frameW,H (u10.2)
        mem_read16(a+28)/1024.0f,mem_read16(a+30)/1024.0f);      // scaleW,H (u5.10)
    // not gst.framehadbackground: change_geom's Zelda hack then
    // turns lighting off until the next background, and Kirby's
    // gameplay (HUD drawn last) showed its normals as colours
}

// G_BG_COPY (uObjBg): 1:1, copy mode. Kirby 64's HUD bar is a 320x48 one;
// c_zeldabackground drew 512x256 of RDRAM
static void c_bgcopy(dword a)
{
    dword image=address(mem_read32p(a+16));
    logd(" bgcopy at %08X",a);
    rdp_bgimagecopy(image,mem_read8(a+22),mem_read8(a+23),mem_read16(a+24),
        BGSIZE(a+2),BGSIZE(a+10),                                // imageW,H
        mem_read16(a+0)/32.0f,mem_read16(a+8)/32.0f,             // imageX,Y (u10.5)
        (short)mem_read16(a+4)/4.0f,(short)mem_read16(a+12)/4.0f, // frameX,Y (s10.2)
        mem_read16(a+6)/4.0f,mem_read16(a+14)/4.0f);             // frameW,H (u10.2)
}

static void rdp_cmd2(dword w0,dword w1)
{
    dword c[2];
    c[0]=w0;
    c[1]=w1;
    rdp_cmd(c);
}

// G_OBJ_LOADTXTR (uObjTxtr), as the RDP commands GLideN64's gSPObjLoadTxtr
// sends: blocks and tiles load as 8-bit texels into load tile 7 (GLideN64
// now uses 16-bit ones: the same bytes). A load is skipped when the status
// word sid already has flag under mask, which the load then sets.
static void c_objloadtxtr(dword a)
{
    dword type =mem_read32p(a);
    dword image=mem_read32p(a+4);
    int   tmem =mem_read16(a+8);
    int   p1   =mem_read16(a+10);
    int   p2   =mem_read16(a+12);
    int   sid  =mem_read16(a+14);
    dword flag =mem_read32p(a+16);
    dword mask =mem_read32p(a+20);

    if(sid>12)
    {
        logd(" objloadtxtr sid %i?",sid);
        return;
    }
    if((gst.objstatus[sid>>2]&mask)==flag)
    {
        logd(" objloadtxtr already loaded (sid %i)",sid);
        return;
    }
    gst.objstatus[sid>>2]=(gst.objstatus[sid>>2]&~mask)|(flag&mask);

    switch(type)
    {
    case 0x00001033: // G_OBJLT_TXTRBLOCK: tsize, tline
        logd(" objloadtxtr block %08X tmem %i tsize %i tline %i",image,tmem,p1,p2);
        rdp_cmd2(0xfd080000,image);
        rdp_cmd2(0xf5080000|(tmem&0x1ff),0x07000000);
        rdp_cmd2(0xf3000000,0x07000000|((((p1+1)<<3)-1)<<12)|(p2&0xfff));
        break;
    case 0x00fc1034: // G_OBJLT_TXTRTILE: twidth, theight
        logd(" objloadtxtr tile %08X tmem %i twidth %i theight %i",image,tmem,p1,p2);
        rdp_cmd2(0xfd080000|((((p1+1)<<1)-1)&0xfff),image);
        rdp_cmd2(0xf5080000|((((p1+1)>>2)&0x1ff)<<9)|(tmem&0x1ff),0x07000000);
        rdp_cmd2(0xf4000000,0x07000000|(((((p1+1)<<1)-1)<<2)<<12)|((((p2+1)>>2)-1)<<2));
        break;
    case 0x00000030: // G_OBJLT_TLUT: phead, pnum
        logd(" objloadtxtr tlut %08X head %i num %i",address(image),tmem,p1+1);
        rdp_settlut((tmem-256)/16,p1+1,address(image));
        break;
    default:
        logd(" objloadtxtr type %08X?",type);
        break;
    }
}

// G_OBJ_MOVEMEM of a uObjMtx (A-D s15.16, X,Y s10.2, BaseScale u5.10) or
// of its uObjSubMtx tail (X,Y, BaseScale)
static void c_objmatrix(dword a,int sub)
{
    float *m=gst.objmtx;
    int    i;
    if(!sub)
    {
        for(i=0;i<4;i++) m[i]=(int)mem_read32p(a+i*4)/65536.0f;
        a+=16;
    }
    m[4]=(short)mem_read16(a+0)/4.0f;
    m[5]=(short)mem_read16(a+2)/4.0f;
    m[6]=mem_read16(a+4)/1024.0f;
    m[7]=mem_read16(a+6)/1024.0f;
    if(m[6]<=0) m[6]=1;
    if(m[7]<=0) m[7]=1;
    logd(" objmtx %.3f %.3f %.3f %.3f  %.2f,%.2f  %.3f,%.3f",
        m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7]);
}

#define OBJ_RECT   0 // G_OBJ_RECTANGLE: screen = obj
#define OBJ_RECT_R 1 // G_OBJ_RECTANGLE_R: obj/BaseScale + (X,Y)
#define OBJ_SPRITE 2 // G_OBJ_SPRITE: full 2D matrix

// draw a uObjSprite (GLideN64 gSPObjSprite/gSPObjRectangle): its texture is
// already in TMEM at imageAdrs; render tile 0 is set up here, clamped.
static void c_objsprite(dword a,int mode)
{
    float objx  =(short)mem_read16(a+0)/4.0f;
    float scalew=mem_read16(a+2)/1024.0f;
    float imagew=mem_read16(a+4)/32.0f;
    float objy  =(short)mem_read16(a+8)/4.0f;
    float scaleh=mem_read16(a+10)/1024.0f;
    float imageh=mem_read16(a+12)/32.0f;
    int   stride=mem_read16(a+16);
    int   tmem  =mem_read16(a+18);
    int   fmt   =mem_read8(a+20);
    int   siz   =mem_read8(a+21);
    int   pal   =mem_read8(a+22);
    int   flags =mem_read8(a+23);
    float *m=gst.objmtx;
    float x[4],y[4],s[4],t[4];
    float x0,y0,x1,y1,s0,t0,s1,t1;
    int   i,w=(int)imagew,h=(int)imageh;

    logd(" obj%s %.2f,%.2f %ix%i scale %.3f,%.3f fmt %i/%i pal %i tmem %i flags %02X",
        mode==OBJ_SPRITE?"sprite":mode==OBJ_RECT_R?"rectr":"rect",
        objx,objy,w,h,scalew,scaleh,fmt,siz,pal,tmem,flags);
    if(w<=0 || h<=0 || scalew<=0 || scaleh<=0) return;

    rdp_cmd2(0xf5000000|((fmt&7)<<21)|((siz&3)<<19)|((stride&0x1ff)<<9)|(tmem&0x1ff),
             ((pal&15)<<20)|(2<<18)|(2<<8)); // tile 0, clamp s and t
    rdp_cmd2(0xf2000000,((w-1)<<14)|((h-1)<<2));
    rdp_texture(1,0,0);

    x0=objx; x1=objx+imagew/scalew;
    y0=objy; y1=objy+imageh/scaleh;
    if(mode==OBJ_RECT_R)
    {
        x0=x0/m[6]+m[4]; x1=x1/m[6]+m[4];
        y0=y0/m[7]+m[5]; y1=y1/m[7]+m[5];
    }
    x[0]=x0; y[0]=y0; // ul
    x[1]=x1; y[1]=y0; // ur
    x[2]=x1; y[2]=y1; // lr
    x[3]=x0; y[3]=y1; // ll
    if(mode==OBJ_SPRITE)
    {
        for(i=0;i<4;i++)
        {
            float xx=x[i],yy=y[i];
            x[i]=m[0]*xx+m[1]*yy+m[4];
            y[i]=m[2]*xx+m[3]*yy+m[5];
        }
    }

    s0=0; s1=imagew*32;
    t0=0; t1=imageh*32;
    if(flags&0x01) { float k=s0; s0=s1; s1=k; } // G_OBJ_FLAG_FLIPS
    if(flags&0x10) { float k=t0; t0=t1; t1=k; } // G_OBJ_FLAG_FLIPT
    s[0]=s0; t[0]=t0;
    s[1]=s1; t[1]=t0;
    s[2]=s1; t[2]=t1;
    s[3]=s0; t[3]=t1;
    rdp_texquad(x,y,s,t);
}

// G_LOAD_UCODE: data (with its version string) from RDPHALF_1
static void c_loaducode(void)
{
    char text[64];
    int  s2dex;
    ucode_findtext(gst.loadrspdata,(cmd[0]&0xffff)+1,text);
    s2dex=(strstr(text,"S2DEX")!=NULL);
    logd(" load ucode %08X data %08X: %s",address(cmd[1]),gst.loadrspdata,*text?text:"?");
    if(s2dex!=gst.s2dex)
    {
        static int told;
        if(!told++) print("dlist: display list switches microcode to %s\n",*text?text:"(no version string)");
    }
    gst.s2dex=s2dex;
}

// S2DEX 1.x (F3DEX 1 opcodes), which Yoshi's Story switches to with
// G_LOAD_UCODE (AF) for everything but its screen clear. Commands shared
// with F3DEX (DL, ENDDL, MOVEWORD, othermode, RDPHALF_1, LOAD_UCODE) go on
// to rsp_cmd_basic.
// G_OBJ_MOVEMEM (both versions, GLideN64 S2DEX_Obj_MoveMem): the low 16
// bits of w0 are 0 uObjMtx, 2 uObjSubMtx, 8 viewport
static void c_objmovemem(void)
{
    switch(cmd[0]&0xffff)
    {
    case 0: c_objmatrix(address(cmd[1]),0); break;
    case 2: c_objmatrix(address(cmd[1]),1); break;
    case 8: g_viewport(address(cmd[1])); break;
    default: logd(" objmovemem %i?",cmd[0]&0xffff); break;
    }
}

// G_SELECT_DL after its RDPHALF_0 (gst.selecthalf): calls or branches to
// the list only when status word sid doesn't already have flag under mask
static void c_selectdl(void)
{
    dword addr=(gst.selecthalf[0]&0xffff)|((cmd[0]&0xffff)<<16);
    int   sid =(gst.selecthalf[0]>>18)&0xff;
    dword flag=gst.selecthalf[1],mask=cmd[1];
    logd(" selectdl %08X sid %i flag %08X mask %08X",addr,sid,flag,mask);
    if(sid>=4 || (gst.objstatus[sid]&mask)==flag) return;
    gst.objstatus[sid]=(gst.objstatus[sid]&~mask)|(flag&mask);
    c_dlbranch(addr,(cmd[0]>>16)&0xff,0); // G_DL_PUSH 0, G_DL_NOPUSH 1
}

// both versions' G_MW_GENSTAT (MOVEWORD index 8): status word offset/4
static void c_setstatus(int offset,dword value)
{
    logd(" setstatus %i = %08X",offset,value);
    if(offset>=0 && offset<16) gst.objstatus[offset>>2]=value;
}

// S2DEX 1.x (F3DEX 1 opcodes), which Yoshi's Story switches to with
// G_LOAD_UCODE (AF) for everything but its screen clear. Commands shared
// with F3DEX (DL, ENDDL, MOVEWORD, othermode, RDPHALF_1, LOAD_UCODE) go on
// to rsp_cmd_basic.
static int rsp_cmd_s2dex1(int c)
{
    dword a=address(cmd[1]);
    switch(c)
    {
    case 0x01: c_bg1cyc(a); return(1);                      // G_BG_1CYC
    case 0x02: c_bgcopy(a); return(1);                      // G_BG_COPY
    case 0x03: c_objsprite(a,OBJ_RECT); return(1);          // G_OBJ_RECTANGLE
    case 0x04: c_objsprite(a,OBJ_SPRITE); return(1);        // G_OBJ_SPRITE
    case 0x05: c_objmovemem(); return(1);                   // G_OBJ_MOVEMEM
    case 0xb0: c_selectdl(); return(1);                     // G_SELECT_DL
    case 0xb1: logd(" objrendermode %02X",cmd[1]); return(1); // G_OBJ_RENDERMODE
    case 0xb2: c_objsprite(a,OBJ_RECT_R); return(1);        // G_OBJ_RECTANGLE_R
    case 0xc1: c_objloadtxtr(a); return(1);                 // G_OBJ_LOADTXTR
    case 0xc2: c_objloadtxtr(a); c_objsprite(a+24,OBJ_SPRITE); return(1); // G_OBJ_LDTX_SPRITE
    case 0xc3: c_objloadtxtr(a); c_objsprite(a+24,OBJ_RECT);   return(1); // G_OBJ_LDTX_RECT
    case 0xc4: c_objloadtxtr(a); c_objsprite(a+24,OBJ_RECT_R); return(1); // G_OBJ_LDTX_RECT_R
    case 0xbc: // G_MOVEWORD
        if((cmd[0]&0xff)!=8) return(0);
        c_setstatus((cmd[0]>>8)&0xffff,cmd[1]);
        return(1);
    }
    return(0);
}

// S2DEX2 (F3DEX2 opcodes; GLideN64 S2DEX2_Init). Other commands (DL, ENDDL,
// othermode, RDPHALF_1/2, LOAD_UCODE, other MOVEWORDs) go on to the F3DEX2
// table.
static int rsp_cmd_s2dex(int c)
{
    dword a=address(cmd[1]);
    switch(c)
    {
    case 0x01: c_objsprite(a,OBJ_RECT); return(1);          // G_OBJ_RECTANGLE
    case 0x02: c_objsprite(a,OBJ_SPRITE); return(1);        // G_OBJ_SPRITE
    case 0x04: c_selectdl(); return(1);                     // G_SELECT_DL
    case 0x05: c_objloadtxtr(a); return(1);                 // G_OBJ_LOADTXTR
    case 0x06: c_objloadtxtr(a); c_objsprite(a+24,OBJ_SPRITE); return(1); // G_OBJ_LDTX_SPRITE
    case 0x07: c_objloadtxtr(a); c_objsprite(a+24,OBJ_RECT);   return(1); // G_OBJ_LDTX_RECT
    case 0x08: c_objloadtxtr(a); c_objsprite(a+24,OBJ_RECT_R); return(1); // G_OBJ_LDTX_RECT_R
    case 0x09: c_bg1cyc(a); return(1);                      // G_BG_1CYC (uObjScaleBg)
    case 0x0a: c_bgcopy(a); return(1);                      // G_BG_COPY (uObjBg)
    case 0x0b: logd(" objrendermode %02X",cmd[1]); return(1); // G_OBJ_RENDERMODE
    case 0xda: c_objsprite(a,OBJ_RECT_R); return(1);        // G_OBJ_RECTANGLE_R (not G_MTX)
    case 0xdc: c_objmovemem(); return(1);                   // G_OBJ_MOVEMEM (not G_MOVEMEM)
    case 0xdb: // G_MOVEWORD
        if(((cmd[0]>>16)&0xff)!=8) return(0);
        c_setstatus(cmd[0]&0xffff,cmd[1]);
        return(1);
    }
    return(0);
}

void rsp_cmd_zelda(int c)
{
    if(gst.s2dex && rsp_cmd_s2dex(c)) return;
    if(gst.cbfd && (c&0xf0)==0x10)
    { // Conker's TRI4 (GLideN64 F3DEX2CBFD_Tri4): four triangles, 5-bit
      // indices, the third one split over both words
        int t[12],i;
        t[0] =(cmd[0]>>23)&31; t[1] =(cmd[0]>>18)&31;
        t[2] =(((cmd[0]>>15)&7)<<2)|((cmd[1]>>30)&3);
        t[3] =(cmd[0]>>10)&31; t[4] =(cmd[0]>> 5)&31; t[5] =(cmd[0]>> 0)&31;
        t[6] =(cmd[1]>>25)&31; t[7] =(cmd[1]>>20)&31; t[8] =(cmd[1]>>15)&31;
        t[9] =(cmd[1]>>10)&31; t[10]=(cmd[1]>> 5)&31; t[11]=(cmd[1]>> 0)&31;
        for(i=0;i<12;i+=3)
        {
            int r[3];
            // unused slots repeat one index: nothing to draw
            if(t[i]==t[i+1] && t[i]==t[i+2]) continue;
            // wound the other way from TRI1/TRI2: in this order back-face
            // culling removed the ground, walls and parts of Conker's head
            r[0]=t[i]; r[1]=t[i+2]; r[2]=t[i+1];
            g_tri(0,r);
        }
        return;
    }
    switch(c)
    {
    case 0x02: // G_MODIFYVTX
        c_modifyvtx();
        break;
//------------------------------------------ vertices
    case 0x00:
        {
            zerocnt++;
            if(zerocnt>4)
            {
                zerocnt=0;
                warning("dlist: unexpected stream of nops, aborting list.");
                c_dlend();
            }
        }
        break;
    case 0x01: // loadvtx
        {
            int v0,vn;
            v0=0;
            vn=((cmd[0]&0xff000)>>12);
            v0=((cmd[0]&0x000ff)>>1)-vn;
            c_dmavtx(cmd[1],v0,vn);
        }
        break;
    case 0x03: // culling
        {
            int v0,vn;
            v0=(cmd[0]&0xff)>>1;
            vn=1+((cmd[1]&0xff)>>1);
            if(g_culldl(v0,vn))
            {
                if(st.dumpgfx) logd("\n+culling result: hidden ");
                c_dlend();
            }
            else
            {
                if(st.dumpgfx) logd("\n+culling result: visible ");
            }
        }
        break;
//------------------------------------------ matrices
    case 0xda: // LOADMTX
        {
            int pos,load,push,proj,a;
            a=cmd[0]&15;
            proj= (a&4);
            load= (a&2);
            push=!(a&1);
            pos=address(cmd[1]);
            c_dmamtx(pos,proj,load,push);
        }
        break;
    case 0xd8: // POPMTX
        g_popmtx(cmd[1]);
        break;
//------------------------------------------ drawing
    case 0x05: // ZELDA_TRI1
        {
            int vxind[3];
            vxind[0]=((cmd[0]>> 0)&127) >> 1;
            vxind[1]=((cmd[0]>> 8)&127) >> 1;
            vxind[2]=((cmd[0]>>16)&127) >> 1;
            g_tri(0,vxind);
        }
        break;
    case 0x06: // ZELDA_TRI2
        {
            int vxind[3];
            vxind[0]=((cmd[0]>> 0)&127) >> 1;
            vxind[1]=((cmd[0]>> 8)&127) >> 1;
            vxind[2]=((cmd[0]>>16)&127) >> 1;
            g_tri(0,vxind);
            vxind[0]=((cmd[1]>> 0)&127) >> 1;
            vxind[1]=((cmd[1]>> 8)&127) >> 1;
            vxind[2]=((cmd[1]>>16)&127) >> 1;
            g_tri(0,vxind);
        }
        break;
    case 0x07: // ZELDA_TRI3 // as TRI2, but no uvscale?
        {
            int vxind[3];
            vxind[0]=((cmd[0]>> 0)&127) >> 1;
            vxind[1]=((cmd[0]>> 8)&127) >> 1;
            vxind[2]=((cmd[0]>>16)&127) >> 1;
            g_tri(0,vxind);
            vxind[0]=((cmd[1]>> 0)&127) >> 1;
            vxind[1]=((cmd[1]>> 8)&127) >> 1;
            vxind[2]=((cmd[1]>>16)&127) >> 1;
            g_tri(0,vxind);
        }
        break;
//------------------------------------------ textures
    case 0xd7: // TEXTURE
        c_texture(cmd);
        break;
//------------------------------------------ displaylists
    case 0xde: // ZELDA_DL
        c_dlbranch(cmd[1],cmd[0]&0x10000,0);
        break;
    case 0x0a: // ZELDA_BACKGROUND
        c_zeldabackground(address(cmd[1]));
        gst.framehadbackground=1;
        break;
    case 0xdf: // ENDDL
        c_dlend();
        break;
    case 0xe1:
        gst.loadrspdata=address(cmd[1]);
        logd(" data at %08X\n",gst.loadrspdata);
        break;
    case 0xdd: // G_LOAD_UCODE: data (with its version string) from RDPHALF_1
        if(gst.cbfd) { gst.cbfdadv=1; break; } // Conker: switches its lighting mode, no load
        c_loaducode();
        break;
    case 0x04:
        {
            c_dlbranch(gst.loadrspdata,0,0);
        } break;
    case 0xd5:
        if(gst.inlinedl) c_dlinline(cmd[0],cmd[1]);
        else logd("!skipz");
        break;
//------------------------------------------ settings
    case 0xd9: // SETGEOMETRYMODE
        c_setgeommode(2,cmd[0]&0xffffff); // and
        c_setgeommode(1,cmd[1]&0xffffff); // or
        break;
    case 0xdc: // MOVEMEM
        {
            int v0;
            v0=(cmd[0]>>8)&0xffff;
            c_movemem_zelda(v0,cmd[1]);
        }
        break;
    case 0xdb: // MOVEWORD
        {
            int ind=(cmd[0])&255;
            int m=(cmd[0]>>16)&255;
            // Conker's G_MW_COORD_MOD (10) is light-position data only;
            // masked to 4 bits it read as G_MW_MATRIX and bent its models
            if(m==0x10 && gst.cbfd) { g_cbfdcoordmod(cmd[0],cmd[1]); break; }
            // its light count is in bytes, 48 a light
            if(m==0x02 && gst.cbfd) { gst.cbfdlightnum=cmd[1]/48; break; }
            c_moveword(m,ind,cmd[1]);
        }
        break;
    case 0xE2: // Othermode_l
        c_setothermode2(0,cmd);
        break;
    case 0xE3: // Othermode_H
        c_setothermode2(1,cmd);
        break;
    default:
        if((c>0x10 && c<0xa0) || !c) dlisterror();
        logd("!skipz");
        break;
    }
}

/***********************************************************************/

// an abandoned list still ends its frame and reports DP done, as its
// FULLSYNC would have: the game's scheduler waits for that interrupt, and
// BattleTanx froze (audio still running) after "too many errors"
void dlist_abort(void)
{
    rdp_frameend();
    st2.gfxdpsyncpending=1;
}

/***********************************************************************
** Graphics microcode detection
**
** Nintendo's graphics microcodes carry a version string in their data
** segment: "RSP SW Version: 2.0D, ..." for Fast3D, "RSP Gfx ucode F3DEX
** fifo 1.23 ..." / "F3DEX2" / "F3DZEX" / ... for the later ones. The
** command set is chosen from it, so it no longer depends on the game title.
** Custom microcodes without such a string return UCODE_UNKNOWN and keep the
** old title-based choice plus command autodetection.
*/

#define UCODE_UNKNOWN 0
#define UCODE_F3D     1 // Fast3D:           rsp_cmd_basic, F3D vertex/tri packing
#define UCODE_F3DEX   2 // F3DEX/LX/LP 1.x:  rsp_cmd_basic, F3DEX packing
#define UCODE_F3DEX2  3 // F3DEX2/F3DZEX...: rsp_cmd_zelda

static int ucodebyte(dword addr)
{
    dword x=mem_read32p(addr&~3);
    return(((byte *)&x)[3-(addr&3)]);
}

// The string is "RSP Gfx ucode <name> [fifo|xbus] <major>.<minor>...", e.g.
// "F3DEX fifo 1.23", "F3DLX.Rej 1.21", "F3DEX.NoN fifo 2.08", "S2DEX xbus
// 2.04", "F3DZEX.NoN fifo 2.06H". Major version 2 is the F3DEX2 command set
// (whatever the name: F3DEX, F3DLX, L3DEX, S2DEX, F3DZEX, F3DAM, F3DFLX...);
// 0.9x/1.x is F3DEX 1. Other designs (ZSort...) stay unknown.
static int ucode_classify(char *text)
{
    char *p;
    if(!strncmp(text,"RSP SW Version",14)) return(UCODE_F3D);
    if(strncmp(text,"RSP Gfx ucode ",14)) return(UCODE_UNKNOWN);
    p=text+14;
    if(strncmp(p,"F3D",3) && strncmp(p,"L3D",3) && strncmp(p,"S2D",3)) return(UCODE_UNKNOWN);
    while(*p && *p!=' ') p++;                        // skip the name
    while(*p && !(p[0]>='0' && p[0]<='9' && p[1]=='.')) p++; // find "N."
    if(!*p) return(UCODE_UNKNOWN);
    return(*p=='2'?UCODE_F3DEX2:UCODE_F3DEX);
}

// the "RSP ..." version string in microcode data (text[64]) and its class
static int ucode_findtext(dword addr,int size,char *text)
{
    int i,j,c,ucode=UCODE_UNKNOWN;
    *text=0;
    for(i=0;i+3<size && ucode==UCODE_UNKNOWN;i++)
    {
        if(ucodebyte(addr+i)!='R' || ucodebyte(addr+i+1)!='S' || ucodebyte(addr+i+2)!='P') continue;
        for(j=0;j<63 && i+j<size;j++)
        {
            c=ucodebyte(addr+i+j);
            if(c<32 || c>126) break;
            text[j]=(char)c;
        }
        text[j]=0;
        ucode=ucode_classify(text);
    }
    return(ucode);
}

static int dlist_detectucode(OSTask_t *task)
{
    static dword lastaddr=0xffffffff;
    static dword lastcrc;
    static int   lastsize;
    static int   lastucode;
    static int   lasts2dex;
    static int   lastcbfd;
    static int   lastinlinedl;
    dword addr=task->m_ucode_data;
    int   size=task->ucode_data_size;
    char  text[64];
    int   i,ucode;
    dword crc=2166136261u;

    if(size<=0 || size>0x1000) size=0x1000;
    // Games can replace microcode at the same RDRAM address (and a new
    // ROM can reuse the previous ROM's address). Validate its contents.
    for(i=0;i<size;i++)
    {
        crc^=ucodebyte(addr+i);
        crc*=16777619u;
    }
    gst.s2dex=lasts2dex;
    gst.cbfd=lastcbfd;
    gst.inlinedl=lastinlinedl;
    gst.cbfdadv=0; // each task starts in the basic lighting mode
    if(addr==lastaddr && size==lastsize && crc==lastcrc) return(lastucode);

    ucode=ucode_findtext(addr,size,text);
    lasts2dex=gst.s2dex=(strstr(text,"S2DEX")!=NULL);
    lastcbfd=gst.cbfd=(strstr(text,"F3DEXBG")!=NULL); // Conker's Bad Fur Day
    // Cruis'n Exotica's D5 handler, found by its code (the string is stock):
    // sll s3,s3,8; srl s3,s3,5; sub gp,r0,s3. ucode_size is 0 in its tasks.
    lastinlinedl=0;
    if(ucode==UCODE_F3DEX2)
    {
        for(i=0;i<0x1000-8;i+=4)
        {
            if(mem_read32p(task->m_ucode+i)==0x00139A00 &&
               mem_read32p(task->m_ucode+i+4)==0x00139942 &&
               mem_read32p(task->m_ucode+i+8)==0x0013E022) { lastinlinedl=1; break; }
        }
    }
    gst.inlinedl=lastinlinedl;
    if(gst.inlinedl) print("dlist: microcode runs inline lists (D5)\n");

    print("dlist: microcode %s -> %s\n",*text?text:"(no version string)",
        ucode==UCODE_F3D   ?"Fast3D":
        ucode==UCODE_F3DEX ?"F3DEX":
        ucode==UCODE_F3DEX2?"F3DEX2":"unknown, using title/autodetect");
    lastaddr=addr;
    lastcrc=crc;
    lastsize=size;
    lastucode=ucode;
    return(ucode);
}

// A list the game is still writing (Gauntlet Legends): its end is a branch
// to itself, which the CPU overwrites with the next commands while the RSP
// spins on it. The walk pauses there and dlist_resume continues it once the
// words change.
static int   dl_wait;
static dword dl_waitw[2];
static int   dl_cmdcnt;
static int   dl_nosynccnt;

static int  dlist_walk(void);
static void dlist_finish(void);

// A task starts with the microcode's own othermode words: the RSP's data
// memory is loaded from the task's microcode data, which holds EF080CFF
// 00000000 (texture perspective on), and G_SETOTHERMODE_H/L change fields of
// that copy. Kept from zero and from task to task instead, Harvest Moon 64,
// which never sets texture perspective, drew everything with it off: its
// texture coordinates were taken at half (rdp.c tri_texhalf) and every tile,
// sprite and letter showed a quarter of its texture.
static void dlist_startothermode(OSTask_t *task)
{
    int i,size=task->ucode_data_size;
    if(size<=0 || size>0x800) size=0x800;
    for(i=0;i+8<=size;i+=4)
    {
        if(mem_read32p(task->m_ucode_data+i)==0xEF080CFF)
        {
            gst.omodeh=0x080CFF;
            gst.omodel=mem_read32p(task->m_ucode_data+i+4);
            return;
        }
    }
}

int dlist_execute(OSTask_t *task)
{
    int cmdnum,i,n,c,nopcount=0,errorcount=0;
    int vtxcnt=0,tricnt=0;

    zerocnt=0;
    static int firsttime=1;
    int starttime;

    dl_wait=0;
    dl_cmdcnt=0;
    // the limit is per list: BattleTanx puts one bad word (a palette frame
    // past its table, the game's own data) in every frame, and a session
    // count reached 100 after a few minutes of play
    errors=0;
    dl_histn=0;
    dl_histshown=0;
    if(gst.ignore)
    {
        os_event(OS_EVENT_SP);
        os_event(OS_EVENT_DP);
        return(0);
    }

    starttime=timer_us(&st2.timer);

    cmdnum=task->data_size/8;
    dllimit=0;

    if(st.dumpgfx)
    {
        print("displaylist-start\n"); flushdisplay();
    }

    logd("Displaylist:\n");
    logd("list: bootucode %5i bytes\n",task->ucode_boot_size);
    logd("list: ucode     %5i bytes (%08X)\n",task->ucode_size,task->m_ucode);
    logd("list: ucodedata %5i bytes\n",task->ucode_data_size);
    logd("list: dramstack %5i bytes\n",task->dram_stack_size);
    logd("list: data      %5i bytes (%08X)\n",task->data_size,task->m_data_ptr);
    logd("list: yielddata %5i bytes\n",task->yield_data_size);
    logd("list: %i commands:\n",cmdnum);
    logd("mode: zelda=%i diddly=%i wave=%i wrus=%i geye=%i\n",
        cart.dlist_zelda,
        cart.dlist_diddlyvx,
        cart.dlist_wavevx,
        cart.dlist_wrusvx,
        cart.dlist_geyevx);

    if(firsttime && st.dumpgfx)
    {
        firsttime=0;
        // IMEM holds 4K; Gauntlet Legends leaves ucode_size as junk
        // (0x7FFF0DAD) and the dump never ended
        disasm_dumpucode("rsp.log",
            task->m_ucode     ,task->ucode_size>4096?4096:task->ucode_size,
            task->m_ucode_data,task->ucode_data_size,
            cart.iszelda?0x1008:0x1080);
        logd("RSP microcode/data dumped to RSP.LOG\n");
    }

    // no limit from data_size: the microcode follows the list to ENDDL and
    // the size is only the buffer (Destruction Derby gives 513184 bytes; the
    // dropped list then had no FULLSYNC, so no DP interrupt, and it waited).
    // The command walk below has its own limit.

    dlpnt=task->m_data_ptr;

    if(1)
    { // check start of cmdlist for errors
        if(cmdnum>16) n=16;
        else n=cmdnum;
        errorcount=0;
        for(i=0;i<n;i++)
        {
            cmd[0]=mem_read32p(dlpnt+i*8+0);
            cmd[1]=mem_read32p(dlpnt+i*8+4);
            c=cmd[0]>>24;
            // the list may end early: data_size is the buffer, not the
            // list (Wipeout 64's first list is 7 commands in a zeroed 448
            // byte buffer, and its zeros were counted as errors)
            if(c==0xb8 || c==0xdf) { n=i+1; break; } // ENDDL (F3D, F3DEX2)
            if((c>0x10 && c<0xb0) || !c)
            {
                //error("dlist: unexpected command %08X %08X at %08X",cmd[0],cmd[1],dlpnt+i*8);
                errorcount++;
            }
        }
        if(errorcount>n/4)
        {
            // Not a display list, and its microcode has no graphics version
            // string: a task of the game's own sent with the graphics type.
            // Last Legion UX sends one before every display list, a table of
            // pointers for a second microcode; dropped, whatever it computes
            // for the frame was missing. HLE OS: it runs on the RSP
            // interpreter like the task types with no HLE (rsp_runtask), and
            // ends without a frame or a DP interrupt of its own.
            if(!st.lleos)
            {
                char text[64];
                int  size=task->ucode_data_size;
                if(size<=0 || size>0x1000) size=0x1000;
                if(ucode_findtext(task->m_ucode_data,size,text)==UCODE_UNKNOWN && !*text)
                {
                    static dword told;
                    if(told!=task->m_ucode)
                    {
                        told=task->m_ucode;
                        print("dlist: task with microcode %08X is no display list: run on the RSP interpreter\n",task->m_ucode);
                    }
                    rsp_runtask(task);
                    return(0);
                }
            }
            error("dlist: display list doesn't look right (ecnt=%i/%i)",errorcount,n/4);
            dlist_abort();
            return(0);
        }
        errorcount=0;
    }

    dlist_startothermode(task);

    cart.dlist_wrusvx=0; // re-detected from this task's first G_VTX
    gst.pdvx=0;
    gst.vtxcolorbase=0;
    switch(dlist_detectucode(task))
    {
    case UCODE_F3D:
        cart.dlist_diddlyvx=0;
        cart.dlist_geyevx=0;
        cart.dlist_zelda=0;
        cart.dlist_wavevx=1;
        break;
    case UCODE_F3DEX:
        cart.dlist_diddlyvx=0;
        cart.dlist_geyevx=0;
        cart.dlist_zelda=0;
        cart.dlist_wavevx=0;
        break;
    case UCODE_F3DEX2:
        cart.dlist_diddlyvx=0;
        cart.dlist_geyevx=0;
        cart.dlist_zelda=1;
        break;
    default: // no version string: previous title-based choice
        if(!strncmp(cart.title,"Diddy Kong Racing",16)) cart.dlist_diddlyvx=1;
        // Perfect Dark: Rare's Fast3D (GLideN64 F3DPD), GoldenEye's with
        // color-indexed vertices. Fast3D vertex counts, 4-triangle B1.
        if(!strncmp(cart.title,"Perfect Dark",12))
        {
            gst.pdvx=1;
            cart.dlist_diddlyvx=0;
            cart.dlist_wavevx=1;
            cart.dlist_geyevx=1;
        }
        if(cart.ismario) cart.dlist_wavevx=1;
        else if(cart.iszelda) cart.dlist_zelda=1;
        else cart.dlist_zelda=0;
        break;
    }

    x_fastfpu(1);

    rdp_framestart();
    gst.dkr_vertexi=0;
    gst.dkr_billboard=0;
    gst.dkr_mtxbase=gst.dkr_vtxbase=0;
    g_resetmtx();
    memset(gst.objmtx,0,sizeof(gst.objmtx));
    gst.objmtx[0]=gst.objmtx[3]=gst.objmtx[6]=gst.objmtx[7]=1; // identity
    memset(gst.objstatus,0,sizeof(gst.objstatus)); // DMEM, reloaded per task
    gst.sprite2d=0;
    gst.lastframehadbackground=gst.framehadbackground;
    gst.framehadbackground=0;
    gst.lightnum=0;
    gst.lightnumchanged=1;
    gst.lookaton=0;

    stackp=1;
    if(dlist_walk()) return(1);
    dlist_finish();
    starttime=timer_us(&st2.timer)-starttime;
    st.us_gfx+=starttime;
    return(0);
}

// the command walk: 1 = paused at a branch to itself (dl_wait)
static int dlist_walk(void)
{
    int c;
    while(stackp>0)
    {
        if(st.breakout) break;

        if(dllimit>0)
        {
            dllimit--;
            if(!dllimit)
            {
                c_dlend();
                continue;
            }
        }

        cmd[0]=mem_read32p(dlpnt+0);
        cmd[1]=mem_read32p(dlpnt+4);
        c=cmd[0]>>24;
        if(c==(cart.dlist_zelda?0xde:0x06) && ((cmd[0]>>16)&0xff)==1 &&
           address(cmd[1])==(dlpnt&0xffffff))
        { // branch to itself: wait for the CPU to write the rest
            dl_wait=1;
            dl_waitw[0]=cmd[0];
            dl_waitw[1]=cmd[1];
            if(st.dumpgfx) logd("%08X: waiting for the CPU (branch to itself)\n",dlpnt);
            x_fastfpu(0);
            return(1);
        }
        if(st.dumpgfx)
        {
            dumpcmd(dlpnt,cmd);
            logd(NULL); // flush
        }
        dl_hist[dl_histn%DL_HIST][0]=dlpnt;
        dl_hist[dl_histn%DL_HIST][1]=cmd[0];
        dl_hist[dl_histn%DL_HIST][2]=cmd[1];
        dl_histn++;
        dlpnt+=8;

        if(dl_cmdcnt++>200000) // a runaway walk; Destruction Derby races run 20000+
        {
            error("dlist: display list too large (%i commands)\n",dl_cmdcnt);
            dlist_abort();
            return(0);
        }
        if(errors>100)
        {
            error("dlist: display list has too many errors");
            errors=0;
            dlist_abort();
            return(0);
        }

        if(c) zerocnt=0;

        if(c==0xEF)
        {
            gst.omodeh=cmd[0]&0xffffff;
            gst.omodel=cmd[1];
        }

        if(c==0xe4 && gst.s2dex &&
           (mem_read32p(dlpnt)>>24)==(cart.dlist_zelda?0x04:0xb0))
        {
            // S2DEX RDPHALF_0 of the G_SELECT_DL that follows, not a texrect
            gst.selecthalf[0]=cmd[0];
            gst.selecthalf[1]=cmd[1];
            if(st.dumpgfx) logd(" rdphalf_0 (select dl)\n");
        }
        else if(c>=0xe4)
        {
            int extra;
            logd("-> RDP ");
            extra=rdp_cmd(cmd);
            if(st.dumpgfx) logd("\n");
            while(extra>0)
            {
                cmd[0]=mem_read32p(dlpnt+0);
                cmd[1]=mem_read32p(dlpnt+4);
                if(st.dumpgfx) dumpcmd(dlpnt,cmd);
                dlpnt+=8;
                logd("-> RDP (extra data) ");
                extra=rdp_cmd(cmd);
                if(st.dumpgfx) logd("\n");
            }
        }
        else
        {
            if(cart.dlist_zelda)
            {
                rsp_cmd_zelda(c);
            }
            else
            {
                rsp_cmd_basic(c);
            }
            if(st.dumpgfx) logd("\n");
        }
    }
    return(0);
}

// the list has ended: the frame, logs
static void dlist_finish(void)
{
    int cmdcnt=dl_cmdcnt;
    if(st2.gfxdpsyncpending || dl_nosynccnt>10)
    {
        rdp_frameend();
        dl_nosynccnt=0;
    }
    else
    {
        dl_nosynccnt++;
    }

    x_fastfpu(0);

    logd("Additionally: %i vertices, %i triangles, %i matrices\n",
        gst.cnt_vtx,gst.cnt_tri,gst.cnt_mtx);
    logd(NULL); // flush

    if(st.gfxthread)
    {
        logd("Displaylist %i commands (separate gfxthread).\n",cmdcnt);
        logh("Displaylist %i commands (separate gfxthread).\n",cmdcnt);
    }
    else
    {
        logd("Displaylist %i commands.\n",cmdcnt);
        logh("Displaylist %i commands.\n",cmdcnt);
    }

    if(st.dumpgfx)
    {
        print("displaylist-end (%i commands)\n",cmdcnt);
        flushdisplay();
    }
}

// a paused list (dl_wait): 1 = still waiting, 0 = it has ended now or
// nothing was paused
int dlist_resume(void)
{
    if(!dl_wait) return(0);
    if(mem_read32p(dlpnt)==dl_waitw[0] && mem_read32p(dlpnt+4)==dl_waitw[1]) return(1);
    dl_wait=0;
    dl_cmdcnt=0;
    x_fastfpu(1);
    if(dlist_walk()) return(1);
    dlist_finish();
    return(0);
}

void dlist_cammove(float x,float y,float z)
{
    gst.cammove[0]=x;
    gst.cammove[1]=y;
    gst.cammove[2]=z;
    gst.cammoveset=1;
}

void dlist_ignoregraphics(int ignore)
{
    gst.ignore=ignore;
}

