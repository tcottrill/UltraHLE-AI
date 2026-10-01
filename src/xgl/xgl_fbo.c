// Offscreen render target. The emulator draws into an FBO at its configured
// output size, which is then scaled into the window with letterboxing.
// Adapted from AAE aae_video/gl_fbo.cpp (create_texture / create_fbo /
// CHECK_FRAMEBUFFER_STATUS), with a depth-stencil attachment added.

#include "xgl_internal.h"

static const char *fbostatus(GLenum s)
{
    switch(s)
    {
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:         return "incomplete attachment";
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "missing attachment";
    case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:        return "incomplete draw buffer";
    case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:        return "incomplete read buffer";
    case GL_FRAMEBUFFER_UNSUPPORTED:                   return "unsupported format combination";
    case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:        return "incomplete multisample";
    default:                                           return "unknown status";
    }
}

int xgl_fbo_create(int w,int h)
{
    GLenum status;

    // RGBA8: GL 3.3 core doesn't guarantee RGB8 is color-renderable. The
    // Voodoo's 16-bit framebuffer had no alpha buffer next to depth, so
    // alpha is never written (xgl_state_apply) and is cleared to 1.0 here,
    // making destination alpha read as 1.0 throughout, as XGLIDE did.
    glGenTextures(1,&xg.fbocolor);
    glBindTexture(GL_TEXTURE_2D,xg.fbocolor);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,0);

    glGenRenderbuffers(1,&xg.fbodepth);
    glBindRenderbuffer(GL_RENDERBUFFER,xg.fbodepth);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,w,h);

    glGenFramebuffers(1,&xg.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER,xg.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,xg.fbocolor,0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,xg.fbodepth);

    status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status!=GL_FRAMEBUFFER_COMPLETE)
    {
        x_log("xgl_fbo_create: framebuffer %s (0x%04X)\n",fbostatus(status),status);
        return 0;
    }

    // start from black rather than undefined contents; masks may be left
    // over from drawing when the FBO is rebuilt on reopen
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0,0,0,1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    // Front copy: the emulator clears the FBO as soon as the next frame
    // starts, so the finished frame is kept here for X_FB_FRONT readback
    // (screenshots, framebuffer-as-texture effects).
    glGenTextures(1,&xg.frontcolor);
    glBindTexture(GL_TEXTURE_2D,xg.frontcolor);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D,0);
    glGenFramebuffers(1,&xg.frontfbo);
    glBindFramebuffer(GL_FRAMEBUFFER,xg.frontfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,xg.frontcolor,0);
    status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status!=GL_FRAMEBUFFER_COMPLETE)
    {
        x_log("xgl_fbo_create: front framebuffer %s (0x%04X)\n",fbostatus(status),status);
        return 0;
    }
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER,xg.fbo);
    return 1;
}

void xgl_fbo_destroy(void)
{
    if(xg.fbo)        glDeleteFramebuffers(1,&xg.fbo);
    if(xg.fbodepth)   glDeleteRenderbuffers(1,&xg.fbodepth);
    if(xg.fbocolor)   glDeleteTextures(1,&xg.fbocolor);
    if(xg.frontfbo)   glDeleteFramebuffers(1,&xg.frontfbo);
    if(xg.frontcolor) glDeleteTextures(1,&xg.frontcolor);
    xg.fbo=xg.fbodepth=xg.fbocolor=xg.frontfbo=xg.frontcolor=0;
}

// Copies the finished frame to the front copy (GPU to GPU, cheap).
void xgl_fbo_savefront(void)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER,xg.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,xg.frontfbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0,0,XGL_MAINXS,XGL_MAINYS,0,0,XGL_MAINXS,XGL_MAINYS,
                      GL_COLOR_BUFFER_BIT,GL_NEAREST);
}

void xgl_fbo_bind(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER,xg.rtton?xg.rttfbo:xg.fbo);
}

/****************************************************************************
** render to texture: a second FBO the RDP draws a game's offscreen color
** buffer into (Pokemon Stadium builds its Game Pak cards that way). Same
** pattern as the main FBO (and multifbo in SpriteTest): an RGBA8 color
** texture and a depth-stencil renderbuffer, rebuilt when the size changes.
*/

static int rtt_create(int w,int h)
{
    GLenum status;
    if(xg.rttfbo && xg.rttw==w && xg.rtth==h) return 1;
    if(xg.rttfbo)     glDeleteFramebuffers(1,&xg.rttfbo);
    if(xg.rttdepth)   glDeleteRenderbuffers(1,&xg.rttdepth);
    if(xg.rttcolor)   glDeleteTextures(1,&xg.rttcolor);
    xg.rttfbo=xg.rttdepth=xg.rttcolor=0;

    glGenTextures(1,&xg.rttcolor);
    glBindTexture(GL_TEXTURE_2D,xg.rttcolor);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D,0);

    glGenRenderbuffers(1,&xg.rttdepth);
    glBindRenderbuffer(GL_RENDERBUFFER,xg.rttdepth);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,w,h);

    glGenFramebuffers(1,&xg.rttfbo);
    glBindFramebuffer(GL_FRAMEBUFFER,xg.rttfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,xg.rttcolor,0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,xg.rttdepth);
    status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status!=GL_FRAMEBUFFER_COMPLETE)
    {
        x_log("x_rtt: framebuffer %s (0x%04X)\n",fbostatus(status),status);
        return 0;
    }
    xg.rttw=w;
    xg.rtth=h;
    return 1;
}

// start drawing into a w x h offscreen target; rgba (w*h*4 bytes, top row
// first, may be NULL) is its starting picture: the RDRAM it stands for
int x_rtt_begin(int w,int h,const unsigned char *rgba)
{
    if(!xg.open || w<=0 || h<=0) return 0;
    x_flush();
    if(!rtt_create(w,h)) { xgl_fbo_bind(); return 0; }
    if(!xg.rtton)
    {
        xg.mainxs=xg.xs;
        xg.mainys=xg.ys;
    }
    xg.rtton=1;
    xg.xs=w;
    xg.ys=h;
    glBindFramebuffer(GL_FRAMEBUFFER,xg.rttfbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glClearDepth(1.0);
    glClearStencil(0);
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    xg.rttcover=0;
    if(rgba)
    {   // GL rows start at the bottom
        int y;
        glBindTexture(GL_TEXTURE_2D,xg.rttcolor);
        for(y=0;y<h;y++)
            glTexSubImage2D(GL_TEXTURE_2D,0,0,h-1-y,w,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba+y*w*4);
        glBindTexture(GL_TEXTURE_2D,0);
    }
    return 1;
}

// read rows 0..h-1 of the target (top row first) as RGBA8, and if cover
// isn't NULL each pixel's cover value (w*h bytes)
void x_rtt_read(int h,unsigned char *rgba,unsigned char *cover)
{
    int y,w=xg.rttw;
    unsigned char *tmp;
    if(!xg.rtton || h<=0) return;
    if(h>xg.rtth) h=xg.rtth;
    x_flush();
    tmp=malloc(w*h*4);
    if(!tmp) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,xg.rttfbo);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,xg.rtth-h,w,h,GL_RGBA,GL_UNSIGNED_BYTE,tmp);
    for(y=0;y<h;y++) memcpy(rgba+y*w*4,tmp+(h-1-y)*w*4,w*4);
    if(cover)
    {
        glReadPixels(0,xg.rtth-h,w,h,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,tmp);
        for(y=0;y<h;y++) memcpy(cover+y*w,tmp+(h-1-y)*w,w);
    }
    free(tmp);
    xgl_fbo_bind();
}

// go on drawing into the target as x_rtt_end left it, if it is w x h still:
// no clear, no new picture. 0 = it isn't, use x_rtt_begin
int x_rtt_resume(int w,int h)
{
    if(!xg.open || !xg.rttfbo || xg.rttw!=w || xg.rtth!=h) return 0;
    x_flush();
    if(!xg.rtton)
    {
        xg.mainxs=xg.xs;
        xg.mainys=xg.ys;
    }
    xg.rtton=1;
    xg.xs=w;
    xg.ys=h;
    glBindFramebuffer(GL_FRAMEBUFFER,xg.rttfbo);
    return 1;
}

// rows y0..y0+h-1 of the target (top row first), as x_rtt_read
void x_rtt_readrows(int y0,int h,unsigned char *rgba,unsigned char *cover)
{
    int y,w=xg.rttw;
    unsigned char *tmp;
    if(!xg.rtton || h<=0 || y0<0 || y0+h>xg.rtth) return;
    x_flush();
    tmp=malloc(w*h*4);
    if(!tmp) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,xg.rttfbo);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,xg.rtth-y0-h,w,h,GL_RGBA,GL_UNSIGNED_BYTE,tmp);
    for(y=0;y<h;y++) memcpy(rgba+y*w*4,tmp+(h-1-y)*w*4,w*4);
    if(cover)
    {
        glReadPixels(0,xg.rtth-y0-h,w,h,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,tmp);
        for(y=0;y<h;y++) memcpy(cover+y*w,tmp+(h-1-y)*w,w);
    }
    free(tmp);
    xgl_fbo_bind();
}

// new pictures for rows y0..y0+h-1 of the target (rgba top row first): the
// RDRAM they stand for has changed
void x_rtt_writerows(int y0,int h,const unsigned char *rgba)
{
    int y,w=xg.rttw;
    if(!xg.rtton || h<=0 || y0<0 || y0+h>xg.rtth) return;
    x_flush();
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glBindTexture(GL_TEXTURE_2D,xg.rttcolor);
    for(y=0;y<h;y++)
        glTexSubImage2D(GL_TEXTURE_2D,0,0,xg.rtth-1-(y0+y),w,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba+y*w*4);
    glBindTexture(GL_TEXTURE_2D,0);
}

void x_rtt_cover(int cover)
{
    if(cover==xg.rttcover) return;
    x_flush();
    xg.rttcover=cover;
}

// back to the main render target
void x_rtt_end(void)
{
    if(!xg.rtton) return;
    x_flush();
    xg.rtton=0;
    xg.xs=xg.mainxs;
    xg.ys=xg.mainys;
    xgl_fbo_bind();
}

// Scales the FBO into the window client area, keeping the aspect ratio,
// with black bars (fit logic as in C64Emu ComputePresentViewport).
void xgl_fbo_present(void)
{
    RECT  rc;
    int   ww,wh,w,h,x,y,xs=XGL_MAINXS,ys=XGL_MAINYS;
    float sx,sy,s;

    GetClientRect(xg.hwnd,&rc);
    ww=rc.right;
    wh=rc.bottom;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,xg.fbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0,0,ww,wh);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(0,0,0,1);
    glClear(GL_COLOR_BUFFER_BIT);

    if(ww<=0 || wh<=0) return;
    sx=(float)ww/xs;
    sy=(float)wh/ys;
    s =sx<sy?sx:sy;
    w =(int)(xs*s+0.5f);
    h =(int)(ys*s+0.5f);
    x =(ww-w)/2;
    y =(wh-h)/2;
    glBlitFramebuffer(0,0,xs,ys,x,y,x+w,y+h,GL_COLOR_BUFFER_BIT,GL_LINEAR);
}
