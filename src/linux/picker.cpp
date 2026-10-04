// The ROM picker drawn in the game window (Linux): a list of the ROMs in
// one folder, driven by keyboard or controller, for SteamOS game mode where
// one Steam shortcut should open UltraHLE and choose a game.
//
// Runs on the main thread with a GL context of its own (sys_gl_new_context),
// only while emulation is stopped. Text is AAE's vector font glyph table
// (aae/vf_fontdata.h), each stroke drawn as a thin quad.
//
// Titles come from the ROM header, or ultra.ini's alttitle= for the game,
// as in the Windows ROM list (listview.c).

extern "C" {
#include "../ultra.h"
#include "../bootlog.h"
#include "../zip/romzip.h"
#include "../xgl/xgl_internal.h"   // xgl_ctx_size
}
#include "aae/sys_gl.h"
#include "aae/vf_fontdata.h"
#include "ui_linux.h"

#include <dirent.h>
#include <algorithm>
#include <string>
#include <vector>

/****************************************************************************
** ROM list
*/

struct RomEntry
{
    std::string path;     // full path, as main_command("rom ...") takes it
    std::string title;    // what the list shows
    std::string region;   // USA, JAP, ...
    int         mbit;
};

static std::vector<RomEntry> s_roms;
static std::string           s_dir;
static std::string           s_message;   // shown above the hints (load errors)
static int                   s_sel, s_top;

static bool romext(const char *name)
{
    static const char *ext[]={".z64",".v64",".n64",".rom",".bin",".u64",".zip"};
    const char *dot=strrchr(name,'.');
    if(!dot) return false;
    for(const char *e:ext) if(!strcasecmp(dot,e)) return true;
    return false;
}

static const char *regionname(int c)
{
    switch(c)
    {
    case 0x44: return "GER";
    case 0x45: return "USA";
    case 0x4A: return "JAP";
    case 0x50: return "EUR";
    case 0x55: return "AUS";
    }
    return "";
}

// header (64 bytes) in .z64 byte order; false if the file is no ROM
static bool readheader(const std::string &path,unsigned char *h,int *size)
{
    if(romzip_iszip(path.c_str()))
    {
        if(romzip_peek(path.c_str(),h,64,size)) return false;
    }
    else
    {
        FILE *f=fopen(path.c_str(),"rb");
        if(!f) return false;
        fseek(f,0,SEEK_END);
        *size=(int)ftell(f);
        fseek(f,0,SEEK_SET);
        bool ok=fread(h,1,64,f)==64;
        fclose(f);
        if(!ok) return false;
    }
    if(h[0]==0x37)          // .v64: 16-bit words swapped
    {
        for(int i=0;i<64;i+=2) std::swap(h[i],h[i+1]);
    }
    else if(h[0]==0x40)     // .n64: little endian words
    {
        for(int i=0;i<64;i+=4) { std::swap(h[i],h[i+3]); std::swap(h[i+1],h[i+2]); }
    }
    return h[0]==0x80;
}

void picker_scan(const char *dir)
{
    s_roms.clear();
    s_dir=dir;
    s_sel=s_top=0;
    DIR *d=opendir(dir);
    if(!d) return;
    while(dirent *e=readdir(d))
    {
        if(e->d_name[0]=='.' || !romext(e->d_name)) continue;
        RomEntry r;
        unsigned char h[64];
        int size=0;
        r.path=s_dir+"/"+e->d_name;
        if(!readheader(r.path,h,&size)) continue;

        char name[21];
        memcpy(name,h+0x20,20);
        name[20]=0;
        for(int i=0;i<20;i++) if((unsigned char)name[i]<32 || (unsigned char)name[i]>126) name[i]=' ';
        for(int i=19;i>=0 && name[i]==' ';i--) name[i]=0;

        // ultra.ini's alttitle= for this game, as the Windows ROM list
        strcpy(romList.name,name);
        romList.alttitle[0]=romList.comment[0]=0;
        inifile_readtemp(romList.name);

        r.title =*romList.alttitle ? romList.alttitle : (*name ? name : e->d_name);
        r.title.erase(0,r.title.find_first_not_of(' '));
        r.region=regionname(h[0x3E]);
        r.mbit  =size/131072;
        s_roms.push_back(r);
    }
    closedir(d);
    std::sort(s_roms.begin(),s_roms.end(),[](const RomEntry &a,const RomEntry &b)
        { return strcasecmp(a.title.c_str(),b.title.c_str())<0; });
    bootlog("picker: %d ROMs in %s",(int)s_roms.size(),dir);
}

void picker_message(const char *msg)
{
    s_message=msg?msg:"";
}

/****************************************************************************
** drawing: a 1024x768 canvas (y up), letterboxed into the window
*/

static const char *vs_src=
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec4 col;\n"
    "out vec4 vcol;\n"
    "void main(){ vcol=col; gl_Position=vec4(pos.x/512.0-1.0,pos.y/384.0-1.0,0.0,1.0); }\n";
static const char *fs_src=
    "#version 330 core\n"
    "in vec4 vcol;\n"
    "out vec4 color;\n"
    "void main(){ color=vcol; }\n";

static void *s_ctx;
static GLuint s_prog,s_vao,s_vbo;
static std::vector<float> s_verts;   // x,y,r,g,b,a
static int    s_fstart[256];
static float  s_fwidth[256],s_foffset[256];

struct Rgba { float r,g,b,a; };
static const Rgba COL_WHITE ={1.00f,1.00f,1.00f,1};
static const Rgba COL_GREY  ={0.62f,0.66f,0.72f,1};
static const Rgba COL_DIM   ={0.40f,0.43f,0.48f,1};
static const Rgba COL_YELLOW={1.00f,0.84f,0.25f,1};
static const Rgba COL_SELBAR={0.13f,0.27f,0.55f,1};
static const Rgba COL_RED   ={1.00f,0.45f,0.40f,1};

static GLuint compile(GLenum type,const char *src)
{
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,NULL);
    glCompileShader(s);
    return s;
}

// glyph start, width and left bearing, as AAE's VectorFont::InitFontData
static void fontinit(void)
{
    for(int i=0;i<256;i++) { s_fstart[i]=-1; s_fwidth[i]=0; s_foffset[i]=0; }
    for(int a=0;vf_fontdata[a]>-1.0f;)
    {
        int ch=(int)vf_fontdata[a++];
        if(ch>=0 && ch<256) s_fstart[ch]=a;
        float minx=1e9f,maxx=-1e9f;
        while((int)vf_fontdata[a]!=VF_EOC)
        {
            minx=std::min(minx,std::min(vf_fontdata[a],vf_fontdata[a+2]));
            maxx=std::max(maxx,std::max(vf_fontdata[a],vf_fontdata[a+2]));
            a+=4;
        }
        a++; // EOC
        if(ch>=0 && ch<256 && maxx>=minx) { s_foffset[ch]=minx; s_fwidth[ch]=maxx-minx; }
    }
    s_fwidth[32]=7.0f;   // SPACE_WIDTH
    s_foffset[32]=0.0f;
}

static bool glinit(void)
{
    if(s_ctx) return sys_gl_bind(s_ctx);
    s_ctx=sys_gl_new_context();
    if(!s_ctx) return false;
    s_prog=glCreateProgram();
    glAttachShader(s_prog,compile(GL_VERTEX_SHADER,vs_src));
    glAttachShader(s_prog,compile(GL_FRAGMENT_SHADER,fs_src));
    glLinkProgram(s_prog);
    glGenVertexArrays(1,&s_vao);
    glGenBuffers(1,&s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER,s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(2*sizeof(float)));
    fontinit();
    return true;
}

static void vert(float x,float y,const Rgba &c)
{
    s_verts.insert(s_verts.end(),{x,y,c.r,c.g,c.b,c.a});
}

static void rect(float x0,float y0,float x1,float y1,const Rgba &c)
{
    vert(x0,y0,c); vert(x1,y0,c); vert(x1,y1,c);
    vert(x0,y0,c); vert(x1,y1,c); vert(x0,y1,c);
}

// a stroke as a quad of width w, ends extended by w/2 so corners close
static void line(float x0,float y0,float x1,float y1,float w,const Rgba &c)
{
    float dx=x1-x0,dy=y1-y0,l=sqrtf(dx*dx+dy*dy);
    if(l<0.001f) { dx=1; dy=0; l=1; }
    dx*=w*0.5f/l; dy*=w*0.5f/l;
    x0-=dx; y0-=dy; x1+=dx; y1+=dy;
    vert(x0-dy,y0+dx,c); vert(x0+dy,y0-dx,c); vert(x1+dy,y1-dx,c);
    vert(x0-dy,y0+dx,c); vert(x1+dy,y1-dx,c); vert(x1-dy,y1+dx,c);
}

// AAE's lowercase glyphs are stylised for arcade screens; the list reads
// best in capitals
// The table's '/' (47) and '\' (92) strokes lean the wrong way in its y-up
// space, so the two are swapped here.
static std::string upper(const char *s)
{
    std::string t=s;
    for(char &c:t)
    {
        if(c=='/') c='\\';
        else if(c=='\\') c='/';
        else c=(char)toupper((unsigned char)c);
    }
    return t;
}

static float textwidth(const char *s0,float scale)
{
    std::string u=upper(s0);
    const char *s=u.c_str();
    float w=0;
    for(;*s;s++)
    {
        unsigned char ch=(unsigned char)*s;
        w+=(s_fwidth[ch]+(ch==32?0:2.0f))*scale; // CHAR_GAP 2
    }
    return w;
}

// text at x, baseline y; cut with "..." past maxw (0 = no limit)
static void text(float x,float y,float scale,const Rgba &c,const char *s,float maxw=0)
{
    std::string t=upper(s);
    if(maxw>0 && textwidth(t.c_str(),scale)>maxw)
    {
        while(!t.empty() && textwidth((t+"...").c_str(),scale)>maxw) t.pop_back();
        t+="...";
    }
    float w=0.5f*scale+0.8f;   // stroke width
    for(unsigned char ch:t)
    {
        if(ch==32 || s_fstart[ch]<0) { x+=s_fwidth[32]*scale; continue; }
        for(int a=s_fstart[ch];(int)vf_fontdata[a]!=VF_EOC;a+=4)
        {
            float off=s_foffset[ch];
            line((vf_fontdata[a]  -off)*scale+x,vf_fontdata[a+1]*scale+y,
                 (vf_fontdata[a+2]-off)*scale+x,vf_fontdata[a+3]*scale+y,w,c);
        }
        x+=(s_fwidth[ch]+2.0f)*scale;
    }
}

static void textcentered(float y,float scale,const Rgba &c,const char *s)
{
    text(512-textwidth(s,scale)/2,y,scale,c,s);
}

// SIGUSR2 (ui_linux.cpp): the next frame goes to picker.ppm in the home
// folder (startup.log's), for checking the picker without a screen capture
volatile int picker_capture;

static void capture(int w,int h)
{
    std::vector<unsigned char> px((size_t)w*h*3);
    char name[MAX_PATH+32];
    picker_capture=0;
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadBuffer(GL_BACK);
    glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,px.data());
    FILE *f=fopen(bootlog_path(name,"picker.ppm"),"wb");
    if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=h-1;y>=0;y--) fwrite(&px[(size_t)y*w*3],1,(size_t)w*3,f);
    fclose(f);
    bootlog("picker: frame saved to %s",name);
}

/****************************************************************************
** one frame
*/

#define ROWS  13
#define ROWH  38.0f

int picker_frame(int input,int canresume,char *path,int pathsize)
{
    int n=(int)s_roms.size();

    if(input&PK_QUIT)   return PICK_QUIT;
    if((input&PK_BACK) && canresume) return PICK_RESUME;
    if(input&PK_RESCAN) { std::string d=s_dir; picker_scan(d.c_str()); s_message.clear(); n=(int)s_roms.size(); }
    if(input&PK_UP)     s_sel--;
    if(input&PK_DOWN)   s_sel++;
    if(input&PK_PGUP)   s_sel-=ROWS;
    if(input&PK_PGDN)   s_sel+=ROWS;
    if(input&PK_HOME)   s_sel=0;
    if(input&PK_END)    s_sel=n-1;
    if(s_sel>=n) s_sel=n-1;
    if(s_sel<0)  s_sel=0;
    if(s_sel<s_top) s_top=s_sel;
    if(s_sel>=s_top+ROWS) s_top=s_sel-ROWS+1;
    if((input&PK_OK) && n>0)
    {
        snprintf(path,pathsize,"%s",s_roms[s_sel].path.c_str());
        return PICK_LAUNCH;
    }

    if(!glinit()) return PICK_NONE;

    // canvas: 4:3, letterboxed
    int ww=0,wh=0,vx,vy,vw,vh;
    xgl_ctx_size(&ww,&wh);
    if(ww<=0 || wh<=0) return PICK_NONE;
    vw=ww; vh=ww*3/4;
    if(vh>wh) { vh=wh; vw=wh*4/3; }
    vx=(ww-vw)/2; vy=(wh-vh)/2;

    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glViewport(0,0,ww,wh);
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(vx,vy,vw,vh);

    s_verts.clear();
    rect(0,0,1024,768,{0.04f,0.05f,0.08f,1});
    textcentered(700,6.0f,COL_YELLOW,"ULTRAHLE");
    textcentered(668,1.6f,COL_DIM,s_dir.c_str());

    if(n==0)
    {
        textcentered(420,2.4f,COL_WHITE,"NO ROMS FOUND");
        textcentered(370,1.8f,COL_GREY,"Put .z64, .v64, .n64 or .zip files in the folder above,");
        textcentered(340,1.8f,COL_GREY,"then press Y (F5) to look again.");
    }
    for(int i=0;i<ROWS && s_top+i<n;i++)
    {
        const RomEntry &r=s_roms[s_top+i];
        float y=612-i*ROWH;
        bool sel=(s_top+i==s_sel);
        if(sel) rect(40,y-10,984,y+ROWH-10,COL_SELBAR);
        text(60,y,2.6f,sel?COL_WHITE:COL_GREY,r.title.c_str(),780);
        text(870,y,2.0f,sel?COL_WHITE:COL_DIM,r.region.c_str());
    }
    if(n>ROWS)
    {
        // scroll bar
        float h=ROWS*ROWH,top=612+ROWH-10;
        float y1=top-h*s_top/n,y0=top-h*(s_top+ROWS)/n;
        rect(990,top-h,996,top,{0.15f,0.16f,0.2f,1});
        rect(990,y0,996,y1,COL_GREY);
    }

    if(!s_message.empty()) textcentered(80,1.8f,COL_RED,s_message.c_str());
    textcentered(36,1.6f,COL_DIM,canresume
        ? "A START   B BACK TO GAME   Y RESCAN   BACK+START QUIT"
        : "A START   Y RESCAN   BACK+START QUIT");

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER,s_vbo);
    glBufferData(GL_ARRAY_BUFFER,s_verts.size()*sizeof(float),s_verts.data(),GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES,0,(GLsizei)(s_verts.size()/6));
    if(picker_capture) capture(ww,wh);
    glchain_swap_buffers();
    return PICK_NONE;
}
