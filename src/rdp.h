// rdp emulation.

#ifdef __cplusplus
extern "C" {
#endif

#define MAXRDPVX 64

typedef struct
{
    // (24) set by dlist
    float   pos[3]; // clip coordinates
    dword   icol;   // original color/normal
    float   tex[2];
    // (16) set by dlist at first usage
    float   col[4]; // shade color, range 0..255, [r,g,b,a]
    // (24) set by rdp at drawprims
    float   ct[2];  // combined texture coords
    float   cc[4];  // combined color
    // screen depth 0..1 (raw RDP lists: interpolated or primitive z);
    // <0: the depth comes from pos[2] like every HLE vertex
    float   zs;
} Vertex;

extern Vertex *rdpvx[MAXRDPVX];
extern char    rdpvxflag[MAXRDPVX];

#define VX_CLIPX1    0x01
#define VX_CLIPX2    0x02
#define VX_CLIPY1    0x04
#define VX_CLIPY2    0x08
#define VX_CLIPZ     0x10
#define VX_CLIPALL   0x1f
#define VX_INITDONE  0x40

// all primitives are now triangles
typedef struct
{
    Vertex *c[3];
    int     wirecolor; // 0=don't draw, 1-7=color (set in flush)
} Primitive; // 16 bytes
// viewport changes saved as:
// c[0]=NULL
// c[1]=NULL
// c[2]=vertex where pos[0,1]=x0/y0 and tex[0,1]=x1/y1

typedef struct
{
    // this struct used for both texrect and fillrect
    // s0,t0,tile,flip only used on texrect
    float   x0,y0,s0,t0; // NOTE: 1.0=pixel, not 4.0 as in hw
    float   x1,y1,s1,t1;
    int     flip,tile;
} TexRect;

// segments not really handled in rdp, but this rdp supports them too
void rdp_segment(int seg,dword base);

// main execute command. Returns 0 if command processed, -1 if unknown,
// >=0 if that many more following opcodes needed
// (regardless of their command code, used for texrect)
int  rdp_cmd(dword *cmd);

// one command of a raw RDP list (DPC registers), drawn with OpenGL
void rdp_rawcmd(const dword *w,int words);

// drawing (draw commands C0..CF,E4,E5 *not* interpreted with rdp_cmd)
void rdp_fillrect(TexRect *tr);
void rdp_texrect(TexRect *tr);
void rdp_texquad(const float *x,const float *y,const float *s,const float *t); // S2DEX sprite
void rdp_newvtx(int first,int num);
void rdp_dupvtx(int i);
void rdp_tri(int *vind);
void rdp_fogrange(float min,float max); // set fog range (fogcolor used)
void rdp_viewport(float xm,float ym,float xa,float ya);
void rdp_gameviewport(float xm,float ym,float xa,float ya); // G_MV_VIEWPORT
void rdp_flat(int flat);

// frame level control
void rdp_framestart(void);
void rdp_frameend(void);
void rdp_present(void);
void rdp_viorigin(dword origin);
void rdp_retrace(void);
void rdp_showvi(dword origin,int width,int height,int bpp); // RDRAM framebuffer to screen
void rdp_opendisplay(void);
void rdp_closedisplay(void);
void rdp_screenshot(char *file); // PNG; NULL: snap\<title>_<time>.png
void rdp_snapshotpoll(void);     // serve a pending screenshot (sync_retrace)
void rdp_addtestdot(int y);
void rdp_grabscreen(void);
void rdp_swap(void);
void rdp_copybackground(dword base,int wid,int hig);
void rdp_bgimage(dword base,int fmt,int siz,int pal,int imgw,int imgh,
                 float imgx,float imgy,float frx,float fry,float frw,float frh,
                 float scalew,float scaleh); // S2DEX G_BG_1CYC
void rdp_bgimagecopy(dword base,int fmt,int siz,int pal,int imgw,int imgh,
                     float imgx,float imgy,float frx,float fry,float frw,float frh); // G_BG_COPY
void rdp_spriteimage(dword base,int fmt,int siz,int imgw,int imgh,
                     float imgx,float frx,float fry,float frw,float frh,
                     float scalew,float scaleh,int flip); // Fast3D Sprite2D
void txt_loadtlut(int pal,int num,dword addr);
void rdp_settlut(int pal,int num,dword addr); // palette load outside LOADTLUT (S2DEX)

extern int showwire;
extern int showinfo;
extern int showtest;
extern int showtest2;
extern int rdp_n64combiner; // 1: the N64 combiner in the shader, 0: the old pattern compiler
void rdp_forcemodechange(void); // the next draw sets up its whole mode again

void rdp_togglefullscreen(void);

void rdp_texture(int on,int tile,int level);

int  rdp_gfxactive(void);

#ifdef __cplusplus
};
#endif
