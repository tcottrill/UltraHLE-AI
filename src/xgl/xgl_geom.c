// Vertex submission and batching (x_begin/x_vx/x_end/x_flush), viewport and
// depth range for the OpenGL x_* backend.

#include "xgl_internal.h"
#include <stddef.h>

static xt_pos *posarray;
static int     posarraysize;
static int     warnedprim,warnedmatrix;

int xgl_geom_create(void)
{
    glGenVertexArrays(1,&xg.vao);
    glGenBuffers(1,&xg.vbo);
    glBindVertexArray(xg.vao);
    glBindBuffer(GL_ARRAY_BUFFER,xg.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(xgl_vertex),(void *)offsetof(xgl_vertex,x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,sizeof(xgl_vertex),(void *)offsetof(xgl_vertex,r));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,sizeof(xgl_vertex),(void *)offsetof(xgl_vertex,s1));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3,2,GL_FLOAT,GL_FALSE,sizeof(xgl_vertex),(void *)offsetof(xgl_vertex,s2));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4,1,GL_FLOAT,GL_FALSE,sizeof(xgl_vertex),(void *)offsetof(xgl_vertex,zs));
    xg.batchn=0;
    return 1;
}

void xgl_geom_destroy(void)
{
    if(xg.vbo) glDeleteBuffers(1,&xg.vbo);
    if(xg.vao) glDeleteVertexArrays(1,&xg.vao);
    xg.vbo=xg.vao=0;
    xg.batchn=0;
}

// XGLIDE x_open: projection 90/0.9/65535; viewport defaults to the full target
void xgl_geom_default(void)
{
    xg.batchn=0;
    xg.zdecal=1.0f;
    memset(&xg.as,0,sizeof(xg.as));
    x_viewport(0.0f,0.0f,(float)(xg.xs-1),(float)(xg.ys-1));
    x_projection(90.0f,0.9f,65535.0f);
}

/****************************************************************************
** viewport and depth
*/

// Flips x0/y0/x1/y1 (x_* coordinates: y runs down) to GL's bottom-left
// origin before truncating, exactly as XGLIDE api.c x_viewport did.
void x_viewport(float x0,float y0,float x1,float y1)
{
    x_flush();
    xg.view_x0=(int)x0;
    xg.view_x1=(int)x1;
    xg.view_y0=(int)((xg.ys-1)-y1);
    xg.view_y1=(int)((xg.ys-1)-y0);
}

// The RDP scissor (SETSCISSOR), in x_* coordinates with exclusive x1/y1:
// drawing outside it is dropped. Majora's Mask letterboxes by scissoring the
// scene to (0,32)-(320,208); without it the scene covered the black bars.
void x_scissor(int on,float x0,float y0,float x1,float y1)
{
    x_flush();
    xg.sc_on=on;
    xg.sc_x0=(int)(x0+0.5f);
    xg.sc_x1=(int)(x1+0.5f);
    xg.sc_y0=xg.ys-(int)(y1+0.5f);
    xg.sc_y1=xg.ys-(int)(y0+0.5f);
}

void xgl_applyviewport(void)
{
    int w=xg.view_x1-xg.view_x0+1;
    int h=xg.view_y1-xg.view_y0+1;
    int y=xg.view_y0;
    int sx0,sy0,sx1,sy1;
    if(w<1) w=1;
    if(h<1) h=1;
    glViewport(xg.view_x0,y,w,h);
    glEnable(GL_SCISSOR_TEST);
    // the whole viewport, x1/y1 exclusive. XGLIDE's grClipWindow took the
    // inclusive corner as its exclusive max and lost the right column and
    // top row: 1:1 render-to-texture pictures kept a column of RDRAM garbage
    // (Pokemon Stadium's Game Pak Check cards, VS screen pictures).
    sx0=xg.view_x0; sx1=xg.view_x0+w;
    sy0=y;          sy1=y+h;
    if(xg.sc_on)
    {
        if(sx0<xg.sc_x0) sx0=xg.sc_x0;
        if(sy0<xg.sc_y0) sy0=xg.sc_y0;
        if(sx1>xg.sc_x1) sx1=xg.sc_x1;
        if(sy1>xg.sc_y1) sy1=xg.sc_y1;
        if(sx1<sx0) sx1=sx0;
        if(sy1<sy0) sy1=sy0;
    }
    glScissor(sx0,sy0,sx1-sx0,sy1-sy0);
}

void xgl_applydepth(void)
{
    float n=xg.znear,f=xg.zfar;
    float a=(f+n)/(f-n);
    float b=-2.0f*f*n/(f-n);
    glUniform1f(xg.u_deptha,a);
    glUniform1f(xg.u_depthb,b*xg.zdecal);
}

// In XFORM_MODE_NONE only the depth range of the projection matters.
void x_projection(float fov,float znear,float zfar)
{
    (void)fov;
    x_flush();
    xg.znear=znear;
    xg.zfar =zfar;
}

void x_frustum(float xmin,float xmax,float ymin,float ymax,float znear,float zfar)
{
    (void)xmin; (void)xmax; (void)ymin; (void)ymax;
    x_projection(90.0f,znear,zfar);
}

void x_ortho(float xmin,float ymin,float xmax,float ymax,float znear,float zfar)
{
    (void)xmin; (void)xmax; (void)ymin; (void)ymax;
    x_projection(0.0f,znear,zfar);
}

int x_zrange(float znear,float zfar)
{
    x_projection(90.0f,znear,zfar);
    return 0;
}

int x_zdecal(float factor)
{
    x_flush();
    xg.zdecal=factor;
    return 0;
}

// Matrices: UltraHLE passes NULL only. Anything else is logged and ignored.
static void matrixwarning(xt_matrix *m)
{
    if(m && !warnedmatrix)
    {
        x_log("xgl: non-NULL matrices are not supported; ignored\n");
        warnedmatrix=1;
    }
}

void x_projmatrix(xt_matrix *m)    { x_flush(); matrixwarning(m); }
void x_cameramatrix(xt_matrix *m)  { matrixwarning(m); }
void x_matrix(xt_matrix *m)
{
    x_flush();
    matrixwarning(m);
    if(m) memcpy(xg.matrix,m,sizeof(xg.matrix));
    else  memset(xg.matrix,0,sizeof(xg.matrix));
}
void x_getmatrix(xt_matrix *m)     { memcpy(m,xg.matrix,sizeof(xg.matrix)); }

/****************************************************************************
** primitive assembly: triangle lists in the vertex order XGLIDE used
** (fxgeom.c vertexdata())
*/

static int assemble(xgl_assembler *as,const xgl_vertex *v,xgl_vertex out[6])
{
    int i=as->count++;

    if(i==0) as->first=*v;
    as->prev[0]=as->prev[1];
    as->prev[1]=as->prev[2];
    as->prev[2]=as->prev[3];
    as->prev[3]=*v;

    switch(as->type)
    {
    case X_TRIANGLES:
        if(i%3!=2) return 0;
        out[0]=as->prev[1]; out[1]=as->prev[2]; out[2]=as->prev[3];
        return 3;
    case X_TRISTRIP:
    case X_QUADSTRIP:
        if(i<2) return 0;
        if(as->flip) { out[0]=as->prev[2]; out[1]=as->prev[1]; }
        else         { out[0]=as->prev[1]; out[1]=as->prev[2]; }
        out[2]=as->prev[3];
        as->flip^=1;
        return 3;
    case X_TRIFAN:
    case X_POLYGON:
        if(i<2) return 0;
        out[0]=as->first; out[1]=as->prev[2]; out[2]=as->prev[3];
        return 3;
    case X_QUADS:
        if(i%4!=3) return 0;
        out[0]=as->prev[0]; out[1]=as->prev[1]; out[2]=as->prev[2];
        out[3]=as->prev[0]; out[4]=as->prev[2]; out[5]=as->prev[3];
        return 6;
    default:
        return 0;   // points and lines: not used by UltraHLE
    }
}

void x_begin(int type)
{
    memset(&xg.as,0,sizeof(xg.as));
    xg.as.type=type;
    if((type==X_POINTS || type==X_LINES || type==X_POLYLINE) && !warnedprim)
    {
        x_log("xgl: points/lines are not supported; ignored\n");
        warnedprim=1;
    }
}

void x_end(void)
{
    xg.as.type=0;
}

void x_vx(xt_pos *p,xt_data *d)
{
    xgl_vertex v,out[6];
    int n,i;

    if(!xg.as.type) x_fatal("vertex without begin");
    v.x =p->x;   v.y =p->y;   v.w =p->z;
    v.r =d->r;   v.g =d->g;   v.b =d->b;   v.a=d->a;
    v.s1=d->t1s; v.t1=d->t1t;
    v.s2=d->t2s; v.t2=d->t2t;
    v.zs=d->zs;
    d->zs=-1.0f; // per vertex: the next one takes its depth from w again
    xgl_stats.in_vx++;

    n=assemble(&xg.as,&v,out);
    if(!n) return;
    if(xg.batchn+n>XGL_BATCHVX) x_flush();
    for(i=0;i<n;i++) xg.batch[xg.batchn++]=out[i];
    xgl_stats.in_tri+=n/3;
}

// Positions are used as given (XFORM_MODE_NONE), so the array is a copy.
void x_vxarray(xt_pos *src,int count,char *mask)
{
    (void)mask;
    posarraysize=0;
    if(!src || count<=0) return;
    posarray=realloc(posarray,count*sizeof(xt_pos));
    if(!posarray) x_fatal("out of memory");
    memcpy(posarray,src,count*sizeof(xt_pos));
    posarraysize=count;
}

void x_vxa(int arrayindex,xt_data *d)
{
    if(arrayindex<0 || arrayindex>=posarraysize) x_fatal("invalid vertex for x_vxa");
    x_vx(&posarray[arrayindex],d);
}

void x_flush(void)
{
    if(!xg.open || !xg.batchn)
    {
        xg.batchn=0;
        return;
    }
    xgl_state_apply();
    glBindVertexArray(xg.vao);
    glBindBuffer(GL_ARRAY_BUFFER,xg.vbo);
    glBufferData(GL_ARRAY_BUFFER,xg.batchn*sizeof(xgl_vertex),xg.batch,GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES,0,xg.batchn);
    xgl_stats.out_tri+=xg.batchn/3;
    xg.batchn=0;
#ifdef _DEBUG
    {
        GLenum e;
        while((e=glGetError())!=GL_NO_ERROR) x_log("xgl: GL error 0x%04X after draw\n",e);
    }
#endif
}
