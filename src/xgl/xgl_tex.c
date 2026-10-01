// Texture handles for x_createtexture / x_loadtexturelevel. Handles are
// 1..MAXTEXTURES-1 as in XGLIDE api.c; 0 or less means "none".

#include "xgl_internal.h"

typedef struct
{
    int    used;
    GLuint name;
    int    format,width,height;
    int    maxlevel;   // highest mip level loaded
} xgl_texture;

static xgl_texture tex[MAXTEXTURES];

static xgl_texture *get(int h)
{
    if(h<=0 || h>=MAXTEXTURES || !tex[h].used) return NULL;
    return &tex[h];
}

GLuint xgl_tex_glname(int h)
{
    xgl_texture *t=get(h);
    return t?t->name:0;
}

// point sampled (X_NOBILIN): x_rectclamp's sampler keeps the same filter
int xgl_tex_nearest(int h)
{
    xgl_texture *t=get(h);
    return t?(t->format&X_NOBILIN)!=0:0;
}

// the texture's own clamp, bit 0 s and bit 1 t (as x_createtexture sets it):
// x_rectclamp's sampler replaces the wrap of both axes and keeps these
int xgl_tex_clamp(int h)
{
    xgl_texture *t=get(h);
    if(!t || !(t->format&X_CLAMP)) return 0;
    return ((t->format&X_CLAMPNOX)?0:1)|((t->format&X_CLAMPNOY)?0:2);
}

int x_createtexture(int format,int width,int height)
{
    int h,clamps,clampt,nearest;

    if(!xg.glready) return -1;
    for(h=1;h<MAXTEXTURES;h++) if(!tex[h].used) break;
    if(h==MAXTEXTURES)
    {
        x_log("too many textures\n");
        return -1;
    }
    memset(&tex[h],0,sizeof(tex[h]));
    tex[h].used  =1;
    tex[h].format=format;
    tex[h].width =width;
    tex[h].height=height;

    // same rules as fx.c mode_texturemode()
    clamps =(format&X_CLAMP) && !(format&X_CLAMPNOX);
    clampt =(format&X_CLAMP) && !(format&X_CLAMPNOY);
    nearest=(format&X_NOBILIN)!=0;

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1,&tex[h].name);
    glBindTexture(GL_TEXTURE_2D,tex[h].name);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,clamps?GL_CLAMP_TO_EDGE:GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,clampt?GL_CLAMP_TO_EDGE:GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,nearest?GL_NEAREST:GL_LINEAR);
    if(format&X_MIPMAP)
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,nearest?GL_NEAREST_MIPMAP_NEAREST:GL_LINEAR_MIPMAP_LINEAR);
    else
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,nearest?GL_NEAREST:GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    xgl_stats.text_total+=width*height*4;
    return h;
}

// data is 32-bit RGBA, bytes R,G,B,A (see XGLIDE fxtext.c text_loadlevel)
int x_loadtexturelevel(int handle,int level,char *data)
{
    xgl_texture *t=get(handle);
    int w,h;

    if(!xg.glready) return 0;
    if(!t || level<0 || level>31) return 0;
    x_flush();   // pending triangles may use this texture's old contents

    w=t->width>>level;  if(w<1) w=1;
    h=t->height>>level; if(h<1) h=1;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,t->name);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,level,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,data);
    if(level>t->maxlevel) t->maxlevel=level;
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,(t->format&X_MIPMAP)?t->maxlevel:0);
    xgl_stats.text_uploaded+=w*h*4;
    return (t->format&X_MIPMAP)?2:1;
}

void x_freetexture(int handle)
{
    xgl_texture *t=get(handle);
    if(!xg.glready) return;
    if(!t) return;
    x_flush();
    glDeleteTextures(1,&t->name);
    xgl_stats.text_total-=t->width*t->height*4;
    memset(t,0,sizeof(*t));
}

// XGLIDE only evicted textures from Voodoo memory here; handles stayed valid.
void x_cleartexmem(void)
{
}

int x_gettextureinfo(int handle,int *format,int *memformat,int *width,int *height)
{
    xgl_texture *t=get(handle);
    if(!t) return 1;
    if(format)    *format   =t->format;
    if(memformat) *memformat=t->format;
    if(width)     *width    =t->width;
    if(height)    *height   =t->height;
    return 0;
}

uchar *x_opentexturedata(int handle)   { (void)handle; return NULL; } // X_DYNAMIC unused
void   x_closetexturedata(int handle)  { (void)handle; }
