// Render-mode setters (x_mask, x_blend, x_combine, ...) and xgl_state_apply(),
// which pushes the current mode to GL before each draw.

#include <string.h>
#include "xgl_internal.h"

static GLenum blendfactor(int f,int isdst,int alpha)
{
    if(f==X_CONSTALPHA)    return GL_CONSTANT_ALPHA;
    if(f==X_INVCONSTALPHA) return GL_ONE_MINUS_CONSTANT_ALPHA;
    if(!isdst)
    {
        switch(f)
        {
        case X_ZERO:          return GL_ZERO;
        case X_OTHER:         return alpha?GL_DST_ALPHA:GL_DST_COLOR;
        case X_ALPHA:         return GL_SRC_ALPHA;
        case X_OTHERALPHA:    return GL_DST_ALPHA;
        case X_INVOTHER:      return alpha?GL_ONE_MINUS_DST_ALPHA:GL_ONE_MINUS_DST_COLOR;
        case X_INVALPHA:      return GL_ONE_MINUS_SRC_ALPHA;
        case X_INVOTHERALPHA: return GL_ONE_MINUS_DST_ALPHA;
        default:              return GL_ONE;
        }
    }
    switch(f)
    {
    case X_ONE:           return GL_ONE;
    case X_OTHER:         return alpha?GL_SRC_ALPHA:GL_SRC_COLOR;
    case X_ALPHA:         return GL_DST_ALPHA;
    case X_OTHERALPHA:    return GL_SRC_ALPHA;
    case X_INVOTHER:      return alpha?GL_ONE_MINUS_SRC_ALPHA:GL_ONE_MINUS_SRC_COLOR;
    case X_INVALPHA:      return GL_ONE_MINUS_DST_ALPHA;
    case X_INVOTHERALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    default:              return GL_ZERO;
    }
}

static GLenum depthfunc(int t)
{
    switch(t)
    {
    case X_TESTEQ:  return GL_EQUAL;
    case X_TESTNE:  return GL_NOTEQUAL;
    case X_TESTGT:  return GL_GREATER;
    case X_TESTLT:  return GL_LESS;
    case X_DISABLE:
    case 0:         return GL_ALWAYS;
    default:        return GL_LEQUAL;
    }
}

// fx.c: reference (int)(limit*256) clamped to 255, test off for 0 and >=1
static float alpharef(float limit)
{
    int r;
    if(!(limit<1.0f) || limit==0.0f) return -1.0f;
    r=(int)(limit*256.0f);
    if(r<0)   r=0;
    if(r>255) r=255;
    return (float)r;
}

void xgl_state_default(void)
{
    xg.geometry=xg.geometryon=xg.geometryoff=0;
    xg.colortext1=X_COLOR;
    xg.text1text2=0;
    xg.text1=xg.text2=0;
    xg.env[0]=xg.env[1]=xg.env[2]=xg.env[3]=0.0f;
    xg.fogtype=X_DISABLE;
    xg.fogmin=xg.fogmax=0.0f;
    xg.fogcolor[0]=xg.fogcolor[1]=xg.fogcolor[2]=0.0f;
    xg.n64cycles=0;
}

void x_forcegeometry(int forceon,int forceoff)
{
    x_flush();
    xg.geometryon =forceon;
    xg.geometryoff=forceoff;
}

void x_geometry(int flags)
{
    x_flush();
    xg.geometry=(flags|xg.geometryon)&~xg.geometryoff;
}

int x_mask(int colormask,int depthmask,int depthtest)
{
    if(colormask!=X_ENABLE && colormask!=X_DISABLE) return 1;
    if(depthmask!=X_ENABLE && depthmask!=X_DISABLE) return 1;
    if(depthtest==0 || depthtest==1) return 1;
    x_flush();
    xg.colormask=(colormask==X_ENABLE);
    xg.depthmask=(depthmask==X_ENABLE);
    xg.depthtest=depthtest;
    return 0;
}

int x_dither(int type)
{
    return (type==X_ENABLE || type==X_DISABLE)?0:1; // 8-bit target, nothing to dither
}

int x_blend(int src,int dst)
{
    if(src<X_ZERO || src>X_LASTBLEND) return 1;
    if(dst<X_ZERO || dst>X_LASTBLEND) return 1;
    x_flush();
    xg.blendsrc=src;
    xg.blenddst=dst;
    return 0;
}

// the alpha X_CONSTALPHA blends with (the RDP blender's fog alpha)
int x_blendalpha(float a)
{
    if(a==xg.blendalpha) return 0;
    x_flush();
    xg.blendalpha=a;
    return 0;
}

int x_alphatest(float limit)
{
    if(xg.geometry&X_WIRE) limit=1.0f;
    if(limit<0.0f || limit>1.0f) return 1;
    x_flush();
    xg.alphatest=limit;
    return 0;
}

int x_combine(int colortext1)
{
    if(colortext1<X_WHITE || colortext1>X_LASTCOMBINE) return 1;
    x_flush();
    xg.colortext1=colortext1;
    xg.text1text2=0;
    xg.n64cycles=0;
    return 0;
}

int x_procombine(int rgb,int alpha)
{
    x_flush();
    xg.colortext1=rgb|(alpha<<16);
    xg.text1text2=0;
    xg.n64cycles=0;
    return 0;
}

void x_n64combine(int cycles,const int *cc,const int *ac,
                  const float *prim,const float *env,float primlod)
{
    x_flush();
    xg.n64cycles=cycles==2?2:1;
    memcpy(xg.n64cc,cc,sizeof(xg.n64cc));
    memcpy(xg.n64ac,ac,sizeof(xg.n64ac));
    memcpy(xg.n64prim,prim,sizeof(xg.n64prim));
    memcpy(xg.env,env,sizeof(xg.env));
    xg.n64primlod=primlod;
}

void x_n64convert(float k4,float k5)
{
    if(k4==xg.n64k[0] && k5==xg.n64k[1]) return;
    x_flush();
    xg.n64k[0]=k4;
    xg.n64k[1]=k5;
}

// Two texture units are always available (the old Voodoo2 check passes).
int x_combine2(int colortext1,int text1text2,int sametex)
{
    (void)sametex;
    if(colortext1<X_WHITE || colortext1>X_LASTCOMBINE) return 1;
    if(text1text2<X_WHITE || text1text2>X_LASTCOMBINE) return 1;
    x_flush();
    xg.colortext1=colortext1;
    xg.text1text2=text1text2;
    xg.n64cycles=0;
    return 0;
}

int x_procombine2(int rgb,int alpha,int text1text2,int sametex)
{
    (void)sametex;
    if(text1text2<X_WHITE || text1text2>X_LASTCOMBINE) return 1;
    x_flush();
    xg.colortext1=rgb|(alpha<<16);
    xg.text1text2=text1text2;
    xg.n64cycles=0;
    return 0;
}

int x_envcolor(float r,float g,float b,float a)
{
    x_flush();
    xg.env[0]=r; xg.env[1]=g; xg.env[2]=b; xg.env[3]=a;
    return 0;
}

int x_texture(int text1handle)
{
    if(text1handle<=0) return 1;
    x_flush();
    xg.text1=text1handle;
    xg.text2=0;
    return 0;
}

// either may be 0 (no texture) for the N64 combiner, which reads only the
// tiles its mode uses
int x_texture2(int text1handle,int text2handle)
{
    if(text1handle<0 || text2handle<0 || (!text1handle && !text2handle)) return 1;
    x_flush();
    xg.text1=text1handle;
    xg.text2=text2handle;
    return 0;
}

// Texture rectangles that sample only inside their tile: clamp those axes
// whatever the texture's wrap. At 1x the RDP samples texel centres and a
// tile-sized wrap never shows; drawn larger, bilinear filtering at the
// rectangle's edge blended in the tile's opposite row (SF Rush's Midway
// logo, 8-row strips with an 8-row mask, had a line at every join).
int x_rectclamp(int s,int t)
{
    int c=(s?1:0)|(t?2:0);
    if(c==xg.rectclamp) return 0;
    x_flush();
    xg.rectclamp=c;
    return 0;
}

// a sampler for clamp mode c (1 s, 2 t, 3 both) and filter, made once
static GLuint rectsampler(int c,int nearest)
{
    static GLuint smp[4][2];
    GLuint *s=&smp[c][nearest];
    if(!*s)
    {
        GLenum f=nearest?GL_NEAREST:GL_LINEAR;
        glGenSamplers(1,s);
        glSamplerParameteri(*s,GL_TEXTURE_WRAP_S,(c&1)?GL_CLAMP_TO_EDGE:GL_REPEAT);
        glSamplerParameteri(*s,GL_TEXTURE_WRAP_T,(c&2)?GL_CLAMP_TO_EDGE:GL_REPEAT);
        glSamplerParameteri(*s,GL_TEXTURE_MIN_FILTER,f);
        glSamplerParameteri(*s,GL_TEXTURE_MAG_FILTER,f);
    }
    return *s;
}

int x_fog(int type,float min,float max,float r,float g,float b)
{
    x_flush();
    xg.fogtype=type;
    xg.fogmin=min;
    xg.fogmax=max;
    xg.fogcolor[0]=r; xg.fogcolor[1]=g; xg.fogcolor[2]=b;
    return 0;
}

// Defaults, exactly as XGLIDE api.c x_reset()
void x_reset(void)
{
    x_geometry(X_CULLBACK);
    x_mask(X_ENABLE,X_ENABLE,X_ENABLE);
    x_dither(X_ENABLE);
    x_blend(X_ONE,X_ZERO);
    x_alphatest(1.0f);
    x_combine(X_COLOR);
    x_zdecal(1.0f);
}

void xgl_state_apply(void)
{
    int wire=xg.geometry&X_WIRE;

    xgl_fbo_bind();
    xgl_applyviewport();
    glUseProgram(xg.prog);

    // The test stays enabled so GL_ALWAYS still honours the depth mask,
    // as Glide's GR_CMP_ALWAYS did.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(depthfunc(xg.depthtest));
    // decal/translucent z modes (x_zdecal > 1): the w path moves depth
    // nearer by the factor; screen depth (a_zs, the RDP's z/w) gets the
    // same nudge from a polygon offset
    if(xg.zdecal>1.0f)
    {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f,-2.0f);
    }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(xg.depthmask?GL_TRUE:GL_FALSE);
    // alpha is never written: the FBO has no alpha buffer to draw (xgl_fbo.c)
    glColorMask(xg.colormask?GL_TRUE:GL_FALSE,xg.colormask?GL_TRUE:GL_FALSE,
                xg.colormask?GL_TRUE:GL_FALSE,GL_FALSE);
    // offscreen target: drawn pixels are marked in the stencil (x_rtt_cover)
    if(xg.rtton && xg.rttcover && xg.colormask)
    {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS,xg.rttcover,0xff);
        glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
        glStencilMask(0xff);
    }
    else glDisable(GL_STENCIL_TEST);

    if(xg.blendsrc==X_ONE && xg.blenddst==X_ZERO)
        glDisable(GL_BLEND);
    else
    {
        glEnable(GL_BLEND);
        glBlendColor(0.0f,0.0f,0.0f,xg.blendalpha);
        glBlendFuncSeparate(blendfactor(xg.blendsrc,0,0),blendfactor(xg.blenddst,1,0),
                            blendfactor(xg.blendsrc,0,1),blendfactor(xg.blenddst,1,1));
    }

    glFrontFace(GL_CW);
    if(xg.geometry&X_CULLFRONT)     { glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); }
    else if(xg.geometry&X_CULLBACK) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
    else                            glDisable(GL_CULL_FACE);

    glPolygonMode(GL_FRONT_AND_BACK,wire?GL_LINE:GL_FILL);

    glUniform1i(xg.u_rgbmode,  wire?X_COLOR:(xg.colortext1&0xffff));
    glUniform1i(xg.u_alphamode,wire?0:((xg.colortext1>>16)&0xffff));
    glUniform1i(xg.u_texmode,  xg.text1text2);
    glUniform4f(xg.u_env,xg.env[0],xg.env[1],xg.env[2],xg.env[3]);
    glUniform1f(xg.u_alpharef,alpharef(xg.alphatest));
    glUniform1i(xg.u_fogmode,xg.fogtype);
    glUniform1f(xg.u_fogmin,xg.fogmin);
    glUniform1f(xg.u_fogmax,xg.fogmax);
    glUniform3f(xg.u_fogcolor,xg.fogcolor[0],xg.fogcolor[1],xg.fogcolor[2]);
    glUniform1i(xg.u_n64cyc,wire?0:xg.n64cycles);
    if(xg.n64cycles && !wire)
    {
        glUniform4iv(xg.u_cc0,1,xg.n64cc);
        glUniform4iv(xg.u_cc1,1,xg.n64cc+4);
        glUniform4iv(xg.u_ac0,1,xg.n64ac);
        glUniform4iv(xg.u_ac1,1,xg.n64ac+4);
        glUniform4fv(xg.u_prim,1,xg.n64prim);
        glUniform1f(xg.u_primlod,xg.n64primlod);
        glUniform2f(xg.u_n64k,xg.n64k[0],xg.n64k[1]);
    }
    xgl_applydepth();

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,xgl_tex_glname(xg.text2));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,xgl_tex_glname(xg.text1));
    // the axis it doesn't clamp keeps the texture's wrap: a rectangle inside
    // its tile across but taller than it got GL_REPEAT down a clamped tile
    // (Indiana Jones' title logo, 16x29 strips in 80-row rectangles, showed
    // two or three times)
    glBindSampler(0,xg.rectclamp?rectsampler(xg.rectclamp|xgl_tex_clamp(xg.text1),xgl_tex_nearest(xg.text1)):0);
    glBindSampler(1,xg.rectclamp?rectsampler(xg.rectclamp|xgl_tex_clamp(xg.text2),xgl_tex_nearest(xg.text2)):0);
    xgl_stats.chg_mode++;
}
