#pragma once

// A SETZIMG has no dimensions. Use the actual depth-clear image, not VI size.
static int depth_target_height(int width,int clearwidth,int clearheight,
                               unsigned int base,unsigned int ramsize)
{
    if(width<=0 || width>1024 || (width&1) || width!=clearwidth ||
       clearheight<=0 || clearheight>1024 || !base || (base&3) ||
       base>=ramsize || (unsigned int)(width*clearheight*2)>ramsize-base)
        return 0;
    return clearheight;
}

// RDP 18-bit Z -> exponent/mantissa depth word, dz bits zero.
// Same packing as rdp_soft.c's zcompress; GL supplies normalized window Z.
static unsigned short depth_pack(float value)
{
    static const int shift[8]={6,5,4,3,2,1,0,0};
    unsigned int z,e=0;
    if(!(value<1.0f)) return 0xfffc; // far plane and invalid GL samples
    if(value<=0.0f) return 0;
    z=(unsigned int)(value*262144.0f+0.5f);
    if(z>0x3ffff) z=0x3ffff;
    while(e<7 && (z&(1u<<(17-e)))) e++;
    return (unsigned short)(((e<<11)|((z>>shift[e])&0x7ff))<<2);
}

// Source is GL bottom-up. Destination is the emulator's word-swapped RDRAM.
// Only w pixels in each of h rows are written; stride is in N64 pixels.
static void depth_copy(unsigned short *dst,int stride,int w,int h,
                       const float *src,int sw,int sh)
{
    int x,y;
    for(y=0;y<h;y++)
    {
        int sy=sh-1-(int)(((long long)y*2+1)*sh/(2*h));
        for(x=0;x<w;x++)
        {
            int sx=(int)(((long long)x*2+1)*sw/(2*w));
            dst[(y*stride+x)^1]=depth_pack(src[sy*sw+sx]);
        }
    }
}
