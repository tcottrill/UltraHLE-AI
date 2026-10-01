// The shader behind every x_combine / x_procombine mode, alpha test and fog.

#include "xgl_internal.h"

static const char *vs_src=
"#version 330 core\n"
"layout(location=0) in vec3 a_pos;\n"            // x, y, w in clip space
"layout(location=1) in vec4 a_col;\n"
"layout(location=2) in vec2 a_tex1;\n"
"layout(location=3) in vec2 a_tex2;\n"
"layout(location=4) in float a_zs;\n"            // screen depth 0..1, <0: from w
"uniform float u_deptha;\n"
"uniform float u_depthb;\n"
"noperspective out vec4 v_col;\n"                // Glide iterates color in screen space
"out vec2  v_tex1;\n"
"out vec2  v_tex2;\n"
"out float v_w;\n"
"void main()\n"
"{\n"
"    float w=a_pos.z;\n"
// raw RDP triangles carry the RDP's own z, linear in screen space as the
// RDP interpolates it (z/w is what GL interpolates linearly), separate
// from the perspective w that corrects the texture coordinates
"    float z=(a_zs>=0.0)?(2.0*a_zs-1.0)*w:u_deptha*w+u_depthb;\n"
"    gl_Position=vec4(a_pos.x,a_pos.y,z,w);\n"
"    v_col=clamp(a_col,0.0,1.0);\n"
"    v_tex1=a_tex1;\n"
"    v_tex2=a_tex2;\n"
"    v_w=w;\n"
"}\n";

static const char *fs_src=
"#version 330 core\n"
"#define X_WHITE        0x1301\n"
"#define X_COLOR        0x1302\n"
"#define X_TEXTURE      0x1303\n"
"#define X_ADD          0x1304\n"
"#define X_MUL          0x1305\n"
"#define X_DECAL        0x1306\n"
"#define X_MULADD       0x1307\n"
"#define X_TEXTURE_IA   0x1308\n"
"#define X_MUL_TA       0x1309\n"
"#define X_MUL_IA       0x130a\n"
"#define X_TEXTUREBLEND 0x130b\n"
"#define X_TEXTUREENVA  0x130c\n"
"#define X_TEXTUREENVC  0x130d\n"
"#define X_SUB          0x130e\n"
"#define X_TEXTUREENVCR 0x130f\n"
"#define X_LINEAR       0x1f01\n"
"#define X_EXPONENTIAL  0x1f02\n"
"#define X_LINEARADD    0x1f03\n"
"#define X_FOGSHADE     0x1f04\n"
"#define X_FOGSHADEINV  0x1f05\n"
"noperspective in vec4 v_col;\n"
"in vec2  v_tex1;\n"
"in vec2  v_tex2;\n"
"in float v_w;\n"
"uniform sampler2D u_tex1;\n"
"uniform sampler2D u_tex2;\n"
"uniform int   u_rgbmode;\n"
"uniform int   u_alphamode;\n"
"uniform int   u_texmode;\n"
"uniform vec4  u_env;\n"
"uniform float u_alpharef;\n"                   // pass if alpha*255 > ref; <0 = off
"uniform int   u_fogmode;\n"
"uniform float u_fogmin;\n"
"uniform float u_fogmax;\n"
"uniform vec3  u_fogcolor;\n"
"uniform int   u_n64cyc;\n"                     // N64 combiner cycles, 0 = u_rgbmode
"uniform ivec4 u_cc0,u_cc1,u_ac0,u_ac1;\n"      // A,B,C,D input codes (x.h)
"uniform vec4  u_prim;\n"
"uniform float u_primlod;\n"
"uniform vec2  u_n64k;\n"                       // K4, K5 (SETCONVERT)
"out vec4 o_col;\n"
"\n"
// the N64 combiner (x_n64combine): (A-B)*C+D per cycle
"vec4 g_t0,g_t1;\n"
"float noise()\n"
"{\n"
"    return fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453);\n"
"}\n"
"vec3 cin(int s,vec4 cb)\n"
"{\n"
"    if(s==0) return cb.rgb;\n"
"    if(s==1) return g_t0.rgb;\n"
"    if(s==2) return g_t1.rgb;\n"
"    if(s==3) return u_prim.rgb;\n"
"    if(s==4) return v_col.rgb;\n"
"    if(s==5) return u_env.rgb;\n"
"    if(s==6) return vec3(1.0);\n"
"    if(s==8) return vec3(cb.a);\n"
"    if(s==9) return vec3(g_t0.a);\n"
"    if(s==10) return vec3(g_t1.a);\n"
"    if(s==11) return vec3(u_prim.a);\n"
"    if(s==12) return vec3(v_col.a);\n"
"    if(s==13) return vec3(u_env.a);\n"
"    if(s==15) return vec3(u_primlod);\n"
"    if(s==16) return vec3(noise());\n"
"    if(s==17) return vec3(u_n64k.x);\n"
"    if(s==18) return vec3(u_n64k.y);\n"
"    if(s==19) return vec3(0.5);\n"
"    return vec3(0.0);\n"                        // 7 zero, 14 lod fraction
"}\n"
"float ain(int s,vec4 cb)\n"
"{\n"
"    if(s==0 || s==8)  return cb.a;\n"
"    if(s==1 || s==9)  return g_t0.a;\n"
"    if(s==2 || s==10) return g_t1.a;\n"
"    if(s==3 || s==11) return u_prim.a;\n"
"    if(s==4 || s==12) return v_col.a;\n"
"    if(s==5 || s==13) return u_env.a;\n"
"    if(s==6)  return 1.0;\n"
"    if(s==15) return u_primlod;\n"
"    if(s==16) return noise();\n"
"    if(s==19) return 0.5;\n"
"    return 0.0;\n"
"}\n"
"vec4 cycle(ivec4 cc,ivec4 ac,vec4 cb)\n"
"{\n"
"    vec3  rgb=(cin(cc.x,cb)-cin(cc.y,cb))*cin(cc.z,cb)+cin(cc.w,cb);\n"
"    float a  =(ain(ac.x,cb)-ain(ac.y,cb))*ain(ac.z,cb)+ain(ac.w,cb);\n"
"    return clamp(vec4(rgb,a),0.0,1.0);\n"
"}\n"
"\n"
"vec4 texel()\n"                                // grTexCombine on TMU0, fx.c ~577-616
"{\n"
"    vec4 t1=texture(u_tex1,v_tex1);\n"
"    vec4 t2=texture(u_tex2,v_tex2);\n"
"    vec4 t;\n"
"    if(u_texmode==X_ADD)         t=t2+t1;\n"               // SCALE_OTHER_ADD_LOCAL, ONE
"    else if(u_texmode==X_MUL)    t=t2*t1;\n"               // SCALE_OTHER, LOCAL
"    else if(u_texmode==X_DECAL)  t=mix(t1,t2,1.0-t1.a);\n" // BLEND, ONE_MINUS_LOCAL_ALPHA
"    else if(u_texmode==X_MULADD) t=t2*t1+t1;\n"            // SCALE_OTHER_ADD_LOCAL, LOCAL
"    else if(u_texmode==X_SUB)    t=t2-t1;\n"               // SCALE_OTHER_MINUS_LOCAL, ONE
"    else                         t=t1;\n"
"    return clamp(t,0.0,1.0);\n"
"}\n"
"\n"
"float fogfactor()\n"
"{\n"
"    if(u_fogmode==X_LINEAR || u_fogmode==X_LINEARADD)\n"
"    {\n"
"        if(u_fogmax<=u_fogmin) return v_w>=u_fogmax?1.0:0.0;\n"
"        return clamp((v_w-u_fogmin)/(u_fogmax-u_fogmin),0.0,1.0);\n"
"    }\n"
"    if(u_fogmode==X_EXPONENTIAL && u_fogmax>0.0)\n"
"        return clamp(1.0-exp(-2.3*v_w/u_fogmax),0.0,1.0);\n"
"    if(u_fogmode==X_FOGSHADE)    return v_col.a;\n"
"    if(u_fogmode==X_FOGSHADEINV) return 1.0-v_col.a;\n"
"    return 0.0;\n"
"}\n"
"\n"
"void main()\n"
"{\n"
"    vec4  c=v_col;\n"
"    vec4  t=texel();\n"
"    vec3  rgb;\n"
"    float a;\n"
"    int   m=u_rgbmode;\n"                                                     // fx.c ~480-551
"    if(m==X_WHITE)                      { rgb=vec3(1.0);                 a=1.0; }\n"
"    else if(m==X_TEXTURE || m==X_DECAL) { rgb=t.rgb;                     a=t.a; }\n"      // DECAL_TEXTURE
"    else if(m==X_ADD)                   { rgb=t.rgb+c.rgb;               a=t.a; }\n"      // TEXTURE_ADD_ITRGB
"    else if(m==X_MUL)                   { rgb=t.rgb*c.rgb;               a=t.a*c.a; }\n"  // TEXTURE_TIMES_ITRGB
"    else if(m==X_TEXTURE_IA)            { rgb=t.rgb;                     a=c.a; }\n"
"    else if(m==X_MUL_TA)                { rgb=t.rgb*c.rgb;               a=t.a; }\n"
"    else if(m==X_MUL_IA)                { rgb=t.rgb*c.rgb;               a=c.a; }\n"
"    else if(m==X_TEXTUREBLEND)          { rgb=mix(c.rgb,t.rgb,t.a);      a=c.a; }\n"      // grColorCombine(7,4,0,1,0)
"    else if(m==X_TEXTUREENVA)           { rgb=mix(u_env.rgb,c.rgb,t.a);  a=c.a; }\n"      // grColorCombine(7,4,1,0,0)
"    else if(m==X_TEXTUREENVC)           { rgb=mix(u_env.rgb,c.rgb,t.rgb);a=c.a; }\n"      // grColorCombine(7,5,1,0,0)
"    else if(m==X_SUB)                   { rgb=t.rgb-c.rgb;               a=t.a; }\n"      // grColorCombine(6,8,0,1,0)
"    else if(m==X_TEXTUREENVCR)          { rgb=mix(c.rgb,u_env.rgb,t.rgb);a=c.a; }\n"      // grColorCombine(7,5,0,2,0)
"    else if(m==X_MULADD)                { rgb=t.rgb*c.rgb+c.rgb;         a=t.a; }\n"      // x.h meaning
"    else                                { rgb=c.rgb;                     a=c.a; }\n"      // X_COLOR: ITRGB
"\n"
"    if(u_alphamode==X_COLOR)                                        a=c.a;\n"            // x_procombine alpha
"    else if(u_alphamode==X_TEXTURE || u_alphamode==X_DECAL ||\n"
"            u_alphamode==X_ADD)                                     a=t.a;\n"
"    else if(u_alphamode==X_MUL)                                     a=t.a*c.a;\n"
"\n"
"    if(u_n64cyc>0)\n"
"    {\n"
"        vec4 r;\n"
"        g_t0=texture(u_tex1,v_tex1);\n"
"        g_t1=texture(u_tex2,v_tex2);\n"
"        r=cycle(u_cc0,u_ac0,vec4(0.5));\n"
"        if(u_n64cyc>1) r=cycle(u_cc1,u_ac1,r);\n"
"        rgb=r.rgb;\n"
"        a  =r.a;\n"
"    }\n"
"\n"
"    rgb=clamp(rgb,0.0,1.0);\n"
"    a  =clamp(a,0.0,1.0);\n"
"    if(u_alpharef>=0.0 && !(floor(a*255.0+0.5)>u_alpharef)) discard;\n"                  // GR_CMP_GREATER
"\n"
"    float f=fogfactor();\n"
"    if(u_fogmode==X_LINEARADD) rgb*=1.0-f;\n"                                            // GR_FOG_ADD2
"    else                       rgb=mix(rgb,u_fogcolor,f);\n"                             // GR_FOG_WITH_TABLE
"    o_col=vec4(rgb,a);\n"
"}\n";

static GLuint compile(GLenum type,const char *src,const char *label)
{
    GLuint sh=glCreateShader(type);
    GLint  ok=0;
    glShaderSource(sh,1,&src,NULL);
    glCompileShader(sh);
    glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
    if(!ok)
    {
        char info[2048];
        glGetShaderInfoLog(sh,sizeof(info),NULL,info);
        x_log("xgl: %s compile failed:\n%s\n",label,info);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

int xgl_shader_create(void)
{
    GLuint vs=compile(GL_VERTEX_SHADER,vs_src,"vertex shader");
    GLuint fs=compile(GL_FRAGMENT_SHADER,fs_src,"fragment shader");
    GLint  ok=0;

    if(!vs || !fs)
    {
        if(vs) glDeleteShader(vs);
        if(fs) glDeleteShader(fs);
        return 0;
    }
    xg.prog=glCreateProgram();
    glAttachShader(xg.prog,vs);
    glAttachShader(xg.prog,fs);
    glLinkProgram(xg.prog);
    glDetachShader(xg.prog,vs);
    glDetachShader(xg.prog,fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(xg.prog,GL_LINK_STATUS,&ok);
    if(!ok)
    {
        char info[2048];
        glGetProgramInfoLog(xg.prog,sizeof(info),NULL,info);
        x_log("xgl: shader link failed:\n%s\n",info);
        glDeleteProgram(xg.prog);
        xg.prog=0;
        return 0;
    }

    xg.u_tex1     =glGetUniformLocation(xg.prog,"u_tex1");
    xg.u_tex2     =glGetUniformLocation(xg.prog,"u_tex2");
    xg.u_rgbmode  =glGetUniformLocation(xg.prog,"u_rgbmode");
    xg.u_alphamode=glGetUniformLocation(xg.prog,"u_alphamode");
    xg.u_texmode  =glGetUniformLocation(xg.prog,"u_texmode");
    xg.u_env      =glGetUniformLocation(xg.prog,"u_env");
    xg.u_alpharef =glGetUniformLocation(xg.prog,"u_alpharef");
    xg.u_fogmode  =glGetUniformLocation(xg.prog,"u_fogmode");
    xg.u_fogmin   =glGetUniformLocation(xg.prog,"u_fogmin");
    xg.u_fogmax   =glGetUniformLocation(xg.prog,"u_fogmax");
    xg.u_fogcolor =glGetUniformLocation(xg.prog,"u_fogcolor");
    xg.u_deptha   =glGetUniformLocation(xg.prog,"u_deptha");
    xg.u_depthb   =glGetUniformLocation(xg.prog,"u_depthb");
    xg.u_n64cyc   =glGetUniformLocation(xg.prog,"u_n64cyc");
    xg.u_cc0      =glGetUniformLocation(xg.prog,"u_cc0");
    xg.u_cc1      =glGetUniformLocation(xg.prog,"u_cc1");
    xg.u_ac0      =glGetUniformLocation(xg.prog,"u_ac0");
    xg.u_ac1      =glGetUniformLocation(xg.prog,"u_ac1");
    xg.u_prim     =glGetUniformLocation(xg.prog,"u_prim");
    xg.u_primlod  =glGetUniformLocation(xg.prog,"u_primlod");
    xg.u_n64k     =glGetUniformLocation(xg.prog,"u_n64k");

    glUseProgram(xg.prog);
    glUniform1i(xg.u_tex1,0);
    glUniform1i(xg.u_tex2,1);
    return 1;
}

void xgl_shader_destroy(void)
{
    if(xg.prog) glDeleteProgram(xg.prog);
    xg.prog=0;
}
