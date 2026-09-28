#include "ultra.h"

#define DUMPSTAGEWAVS 0
#define KOE           0

// Zelda memory usage:
// 940=Left      (1A0)
// AE0=Right     (1A0)
// C80=tempecho? (1A0)
// E20=tempecho? (1A0)
// FC0
//---
// start:
// zeromem 940 & AE0
// loadbuf C80 & E20
// mixer   940 -> C80  *0.5   add echo
// mixer   C80 -> C80  *0.7   silence echo
// savebuf C80 & E20
// interl  940,AE0->3C0
// savebuf 3C0

typedef struct
{
    // memory
    //    0: 4K data
    // 2048: codebook (256 bytes available)
    short    mem[2048+256];
    //
    dword    segment;
    // last SETBUFF vars
    int      out;
    int      in;
    int      cnt;
    int      aux1;
    int      aux2;
    int      aux3;
    //
    dword    m_adpcmloopdata;
    // envmix params
    int      env_lteff;
    int      env_rteff;
    int      env_ltvol;
    int      env_rtvol;
    int      env_ltadd;
    int      env_rtadd;
    int      env_lttar; // target volume
    int      env_rttar;
    // output
    int      lastcodebook;
    int      lastcodebooklen;
    int      dataleft;
    int      lastloadb_from;
    int      lastloadb_outend;
    int      lastloadb_outbeg;
    int      lastadpcm_state;
    int      lastadpcm_outend;
    int      lastadpcm_outbeg;
    int      lastresample_state;
    short    res[0x200];
    int      lastnextbuffer;
    int      lastinterleave;

    int      envmixcnt;

    int      debugadpcmmem;
    int      debugsrcpos;
    int      debugsrcbase;
    int      debugsrcsize;
    int      debugrespos;

    int      adpcmerror_reported;

    int      errorcnt[256];
} SndState;

// states:
// ENVMIX_STATE  =40 short
// RESAMPLE_STATE=16 short
// POLEF_STATE   =4 short
// ADPCM_STATE   =16 short

static SndState sst;

typedef struct
{
    int   cmd;
    char *name;
} Cmdname;

static Cmdname mario_cmdnames[]={
{0x00,"A_SPNOOP"},
{0x01,"A_ADPCM"},
{0x02,"A_CLEARBUFF"},
{0x03,"A_ENVMIXER"},
{0x04,"A_LOADBUFF"},
{0x05,"A_RESAMPLE"},
{0x06,"A_SAVEBUFF"},
{0x07,"A_SEGMENT"},
{0x08,"A_SETBUFF"},
{0x09,"A_SETVOL"},
{0x0a,"A_DMEMMOVE"},
{0x0b,"A_LOADADPCM"},
{0x0c,"A_MIXER"},
{0x0d,"A_INTERLEAV"},
{0x0e,"A_POLEF"},
{0x0f,"A_SETLOOP"},
{0,NULL}};

static Cmdname zelda_cmdnames[]={
{0x00,"Z_NOP"},
{0x01,"Z_ADPCM"},
{0x02,"Z_MEMCLEAR"},
{0x03,"Z_???"},
{0x04,"Z_MIXER2"},
{0x05,"Z_RESAMPLE"},
{0x06,"Z_???"},
{0x07,"Z_FILTER"},
{0x08,"Z_SETBUF"},
{0x09,"Z_MEMLOOP"},
{0x0a,"Z_MEMMOVE"},
{0x0b,"Z_ADPCMCODEB"},
{0x0c,"Z_MIXER"},
{0x0d,"Z_INTERLEAVE"},
{0x0e,"Z_BOOST"},
{0x0f,"Z_ADPCMLOOP"},
{0x10,"Z_MEMMOVE"},
{0x11,"Z_MEMHALVE"},
{0x12,"Z_ENVSET1"},
{0x13,"Z_ENVMIXER"},
{0x14,"Z_LOADBUF"},
{0x15,"Z_SAVEBUF"},
{0x16,"Z_ENVSET2"},
{0x17,"Z_???"},
{0x18,"Z_???"},
{0x19,"Z_???"},
{0x1a,"Z_???"},
{0x1b,"Z_???"},
{0x1c,"Z_???"},
{0x1d,"Z_???"},
{0x1e,"Z_???"},
{0x1f,"Z_???"},
{0,NULL}};

/*************************************************************************/

void printcurve(int x,int scale)
{
    static char space[101]={"                                                                                                    "};
    loga("%7.4f <",(float)x/scale);
    x=x*30/scale+30;
    if(x<0) x=0;
    if(x>59) x=59;
    loga(space+100-x);
    loga("*");
    loga(space+100-(59-x));
    loga(">\n");
}

void printdata(dword *cmd)
{
    int i,x;
    loga("\n+data%08X@%08X: ",cmd[0],cmd[1]);
    for(i=0;i<32;i+=4)
    {
        x=mem_read32p(cmd[1]+i);
        loga("%08X ",x);
    }
}

void printdata2(dword *cmd)
{
    int i,x;
    loga("\n+data%08X@%08X:",cmd[0],cmd[1]);
    for(i=0;i<0x80;i+=4)
    {
        if(!(i&15)) loga("\n%03X: ",i);
        x=mem_read32p(cmd[1]+i);
        loga("%08X ",x);
    }
}

void printcurvedata(dword addr,int len)
{
    int i,x;
    loga("\n");
    for(i=0;i<len;i+=2)
    {
        x=(short)mem_read16(addr+i);
        printcurve(x,10000);
    }
}

void printlocalcurvedata(int src,int len)
{
    int i,x;
    loga("\n");
    for(i=0;i<len;i+=2)
    {
        x=sst.mem[src+i/2];
        printcurve(x,10000);
    }
}


/*************************************************************************/

void loadshort(short *dst,dword addr,int cnt)
{
    int i,a;
    for(i=0;i<cnt;i+=4)
    {
        a=mem_read32p(i+addr);
        *dst++=(a>>16);
        *dst++=a;
    }
}

void loadmem(dword dst,dword addr,int cnt)
{
    int i,a;
    cnt=(cnt+3)&~3; // round to 4 bytes
    if((dst&1) || (addr&3) || (cnt&1))
    {
        error("slist: memort alignment");
        loga("\n!loadmem: memory alignment");
        return;
    }
//loga("\n+loadmem %04X (%03X)",dst,cnt);
    dst>>=1;
    for(i=0;i<cnt;i+=4)
    {
        a=mem_read32p(addr);
        addr+=4;
        sst.mem[dst++]=(a>>16);
        sst.mem[dst++]=a;
    }
}

void savemem(dword dst,dword addr,int cnt)
{
    int i,a;
    cnt=(cnt+3)&~3; // round to 4 bytes
    if((dst&1) || (addr&3) || (cnt&1))
    {
        error("slist: memort alignment");
        loga("\n!savemem: memory alignment");
        return;
    }
//loga("\n+savemem %04X (%03X)",dst,cnt);
    dst>>=1;
    for(i=0;i<cnt;i+=4)
    {
        a=sst.mem[dst++]<<16;
        a|=sst.mem[dst++]&65535;
        mem_write32(i+addr,a);
    }
}

void movemem(int out,int in,int cnt)
{
    if(in>=4096 || out>=4096 || in+cnt>4096 || out+cnt>4096)
    {
        error("slist: memcopy overflow");
        loga("\n!movemem: memory overflow");
        return;
    }
    out>>=1;
    in>>=1;
    memcpy(sst.mem+out,sst.mem+in,cnt);
}

void zeromem(int out,int cnt)
{
    if(out>=4096 || out+cnt>4096)
    {
        error("slist: memcopy overflow");
        loga("\n!zeromem: memory overflow");
        return;
    }
    out>>=1;
    memset(sst.mem+out,0,cnt);
}

/*************************************************************************/
// ADPCM decompress

void loadsamplestate(int dst,dword m_state,int flags)
{
    // load state
    if(flags&1)
    {
        // init (start of sample)
        zeromem(dst,32);
        if(st.dumpsnd) loga("start ");
    }
    else if(flags&2)
    {
        // loop (load loopstate)
        loadmem(dst,sst.m_adpcmloopdata,32);
        if(st.dumpsnd) loga("loop(%08X) ",sst.m_adpcmloopdata);
    }
    else
    {
        // load stored state
        loadmem(dst,m_state,32);
        if(st.dumpsnd) loga("continue ");
    }
}

void savesamplestate(int dst,dword m_state,int flags)
{
    savemem(dst,m_state,32);
}

void adpcm_inittables(void)
{
}

// handles a block of 16 samples
void adpcm_block(short *out,short *last,int src,short *book,int bits)
{
    int s,r,i,n;
    unsigned char buf[16];

    if(bits==4) n=9; else n=5;
    i=0;
    s=src>>1;
    if(src&1)
    {
        // src was not short aligned
        buf[i++]=(unsigned char)sst.mem[s++];
    }
    for(;i<n;)
    {
        r=sst.mem[s++];
        buf[i++]=r>>8;
        buf[i++]=r;
    }

    // Mupen64Plus rsp-hle (alist.c adpcm_decode_frames, audio.c
    // adpcm_compute_residuals), as the microcode does it: the header byte
    // is scale (high nibble) and predictor (low nibble, any of 16: Nuclear
    // Strike uses up to 14, and predictors over 7 used to play silence);
    // each half of the frame is its residuals << 11 plus the predictor's two
    // 8-tap vectors over the last two samples and the earlier residuals.
    // The old decoder ran a 2-tap recursion on the vectors' first taps and
    // halved the residuals only ("50% volume").
    {
        short res[16];
        const short *book1,*book2;
        int  scale=buf[0]>>4,rshift,h,k;
        long accu;

        book1=book+((buf[0]&15)<<4);
        book2=book1+8;
        if(bits==4)
        {
            rshift=scale<12?12-scale:0;
            for(i=0;i<8;i++)
            {
                res[2*i+0]=(short)((unsigned short)(buf[1+i]&0xf0)<<8)>>rshift;
                res[2*i+1]=(short)((unsigned short)(buf[1+i]&0x0f)<<12)>>rshift;
            }
        }
        else
        {
            rshift=scale<14?14-scale:0;
            for(i=0;i<4;i++)
            {
                res[4*i+0]=(short)((unsigned short)(buf[1+i]&0xc0)<< 8)>>rshift;
                res[4*i+1]=(short)((unsigned short)(buf[1+i]&0x30)<<10)>>rshift;
                res[4*i+2]=(short)((unsigned short)(buf[1+i]&0x0c)<<12)>>rshift;
                res[4*i+3]=(short)((unsigned short)(buf[1+i]&0x03)<<14)>>rshift;
            }
        }
        for(h=0;h<2;h++)
        {
            // the two samples before this half: the last frame's, then ours
            int l1=h?out[6]:last[14],l2=h?out[7]:last[15];
            for(i=0;i<8;i++)
            {
                accu=(long)res[h*8+i]<<11;
                accu+=(long)book1[i]*l1+(long)book2[i]*l2;
                for(k=0;k<i;k++) accu+=(long)book2[k]*res[h*8+i-1-k];
                accu>>=11;
                if(accu<-32768) accu=-32768;
                if(accu> 32767) accu= 32767;
                out[h*8+i]=(short)accu;
            }
        }
    }
}

void adpcm(dword m_state,int src,int dst,int cnt,int flags)
{
    int src0;
    int dst0;
    short *last,*next;

    if(st.dumpsnd) loga("\n+adpcm ");

    dst>>=1;
    cnt=(cnt+31)>>5; // 32 bytes/block
    src0=src;
    dst0=dst;

    // state was also first 16 samples
    loadsamplestate(dst*2,m_state,flags);
    last=sst.mem+dst;
    dst+=16;

    if(flags&4)
    {
        //printlocalcurvedata(2048,sst.lastcodebooklen);
        // decode
        while(cnt--)
        {
            next=sst.mem+dst;
            adpcm_block(next,last,src,sst.mem+2048,2);
            last=next;
            src+=5;
            dst+=16;
        }
    }
    else
    {
        static int lastsrcbyte,thissrcbyte,a;
        if(KOE && m_state==0x1d7500)
        {
            a=src-1;
            if((a&0xf)==0xf) thissrcbyte=-1;
            else  if(a&1)    thissrcbyte=sst.mem[a>>1];
            else             thissrcbyte=sst.mem[a>>1]>>8;
            if(thissrcbyte==-1)               print("#unk ");
            else if(thissrcbyte!=lastsrcbyte) print("#DIF ");
            else                              print("#ok  ");
            if(thissrcbyte!=lastsrcbyte) loga("\n#adpcm DIF! last=%02X this=%02X",
                lastsrcbyte&255,thissrcbyte&255);
        }
        // decode
        while(cnt--)
        {
            next=sst.mem+dst;
            adpcm_block(next,last,src,sst.mem+2048,4);
            last=next;
            src+=9;
            dst+=16;
        }
        if(KOE && m_state==0x1d7500)
        {
            a=src-1;
            if(a&1) lastsrcbyte=sst.mem[a>>1];
            else    lastsrcbyte=sst.mem[a>>1]>>8;
        }
    }
    sst.lastadpcm_state=m_state;
    sst.lastadpcm_outbeg=dst0*2;
    sst.lastadpcm_outend=dst*2;

    if(KOE && m_state==0x1d7500)
    {
        int carta,cartb,i;
        if(flags&1)
        {
            sst.debugsrcpos=-16;
            sst.debugsrcsize=0;
            remove("koe.log");
        }
        sst.debugadpcmmem=sst.lastloadb_from;
        sst.debugsrcpos+=sst.debugsrcsize;
        sst.debugsrcbase=dst0*2;
        sst.debugsrcsize=(dst-dst0)-16;

        i=os_finddmasource(sst.lastloadb_from);
        if(i<0)
        {
            carta=0;
            cartb=sst.lastloadb_from;
        }
        else
        {
            carta=st2.dmahistory[i].cart+(sst.lastloadb_from-st2.dmahistory[i].addr);
            cartb=carta+(src-src0)*2;
        }

        print("#a f%02X src:%04X..%04X dst:%05X..%05X cart:%08X..%08X ",
            flags,
            src0*2,src*2,
            sst.debugsrcpos&0xfffff,sst.debugsrcpos+sst.debugsrcsize,
            carta,cartb);
    }

    // save state
    savesamplestate((dst-16)*2,m_state,flags);

    // save to file (debug)
    if(DUMPSTAGEWAVS) // && sst.envmixcnt<1 && st.dumpwav && m_state==0x1d7500)
    {
        char name[16];
        short peak=32767;
        sprintf(name,"ad%06X.wav",m_state&0xffffff);
        sound_addwavfile(name,sst.mem+dst0,2*(dst-dst0-16),0);
    }

    /*
    // check for overflows (debug)
    if(src0<sst.lastloadb_outbeg ||
       src >sst.lastloadb_outend )
    {
        loga(" OUT! by %i (used %04X..%04X, last %04X..%04X)",
            src-sst.lastloadb_outend,
            src0,src,
            sst.lastloadb_outbeg,
            sst.lastloadb_outend);
    }
    */
}



void resample(dword m_state,int src,int dst,int cnt,int flags,int speed)
{
    int subpos,a,b,r;
    int dst0,cnt0,src0;
    int statedata,adpcmdata;
    int loadedstate[8];
    int savedstate[8];

    src-=16; // first 4 shorts will come from state

    speed<<=1;
    cnt>>=1;
    dst>>=1;
    src>>=1;
    dst0=dst;
    src0=src;
    cnt0=cnt;

    sst.lastresample_state=m_state;

    // state was also first 16 samples

    adpcmdata=(sst.mem[src+2]<<16) | sst.mem[src+3];
    if(flags&1)
    {
        subpos=0;
        sst.mem[src+0]=0;
        sst.mem[src+1]=0;
        sst.mem[src+2]=0;
        sst.mem[src+3]=0;
    }
    else
    {
        loadedstate[0]=mem_read32p(m_state+0);
        loadedstate[1]=mem_read32p(m_state+4);
        loadedstate[2]=mem_read32p(m_state+8);
        loadedstate[3]=mem_read32p(m_state+12);
        loadedstate[4]=mem_read32p(m_state+16);
        sst.mem[src+0]=loadedstate[0]>>16;
        sst.mem[src+1]=loadedstate[0];
        sst.mem[src+2]=loadedstate[1]>>16;
        sst.mem[src+3]=loadedstate[1];
        sst.mem[src+4]=loadedstate[2]>>16;
        sst.mem[src+5]=loadedstate[2];
        sst.mem[src+6]=loadedstate[3]>>16;
        sst.mem[src+7]=loadedstate[3];
        subpos        =loadedstate[4];
    }
    statedata=(sst.mem[src+2]<<16) | sst.mem[src+3];

    cnt+=16;

    subpos&=0xffff;
    while(cnt--)
    {
        a=(subpos>>16);
        b=(subpos&0xffff);
        if(1)
        { // linear interpolation
            r=sst.mem[src+a+1]-sst.mem[src+a+0];
            r=((r*b)>>16)+sst.mem[src+a+0];
        }
        else
        {
            r=sst.mem[src+a];
        }
        sst.mem[dst++]=r;
        subpos+=speed;
    }

    subpos-=speed*16;

    src+=(subpos>>16);

    if(st.dumpsnd) loga("\n+resample in:%04X out:%04X speed:%.3f ",2*(src-src0),2*(dst-dst0),speed/65535.0);

    /*
    if(src0*2<sst.lastadpcm_outbeg || src *2>sst.lastadpcm_outend )
    {
        loga(" OUT! by %i (used %04X..%04X, last %04X..%04X)",
            src*2-sst.lastadpcm_outend,
            src0*2,src*2,
            sst.lastadpcm_outbeg,
            sst.lastadpcm_outend);
    }
    */

    // save state
    {
        a =sst.mem[src+0]<<16;
        a|=sst.mem[src+1]&0xffff;
        savedstate[0]=a;
        a =sst.mem[src+2]<<16;
        a|=sst.mem[src+3]&0xffff;
        savedstate[1]=a;
        a =sst.mem[src+4]<<16;
        a|=sst.mem[src+5]&0xffff;
        savedstate[2]=a;
        a =sst.mem[src+6]<<16;
        a|=sst.mem[src+7]&0xffff;
        savedstate[3]=a;
        savedstate[4]=subpos;
        mem_write32(m_state+0,savedstate[0]);
        mem_write32(m_state+4,savedstate[1]);
        mem_write32(m_state+8,savedstate[2]);
        mem_write32(m_state+12,savedstate[3]);
        mem_write32(m_state+16,savedstate[4]);
    }

    // save to file (debug)
    if(DUMPSTAGEWAVS) // && sst.envmixcnt<1 && st.dumpwav && m_state==0x1d7520)
    {
        char name[16];
        short peak=32767;
        sprintf(name,"re%06X.wav",m_state&0xffffff);
        //sound_addwavfile(name,&peak,2,0);
        sound_addwavfile(name,sst.mem+src0,2*(src-src0),0);
    }

    if(KOE && m_state==0x1d7520 && speed==0x47d6*2)
    {
        if(flags&1)
        {
            sst.debugrespos=0;
        }
        a=sst.debugsrcpos+(src0*2-sst.debugsrcbase)/2;
        b=sst.debugrespos;

        print("#r f%02X src:%05X..%05X (%08X) delta %3i srccnt %05X.000 rescnt %05X.%03X ",
                flags,
                a&0xfffff,
                a+(src-src0),
                sst.debugadpcmmem,
                a-(b>>16)+4,
                a,b>>16,(b&0xffff)>>4);
        print("loaded: ");
        for(a=0;a<3;a++) print("%08X ",loadedstate[a]);
        print("saved: ");
        for(a=0;a<3;a++) print("%08X ",savedstate[a]);
        print("\n");

        loga("\n#resample src %05X..%05X",
                a,
                a+(src-src0));

        sst.debugrespos+=cnt0*speed;
    }
}

void filter_block(int dst,short *coef)
{
    int i,j,s;
    // state=previous unfiltered is at 2048
    // copy new unfiltered block    to 2048+8
    for(i=0;i<8;i++)
    {
        sst.mem[2048+i+8]=sst.mem[dst+i];
    }
    // filter 2048..+16 to dst
    for(i=0;i<8;i++)
    {
        s=0;
        for(j=0;j<8;j++)
        {
            s+=coef[j]*sst.mem[2048+i+j];
        }
        sst.mem[dst+i]=s>>16;
    }
    // copy unfiltered to previous unfiltered (2048+8->2048)
    for(i=0;i<8;i++)
    {
        sst.mem[2048+i]=sst.mem[2048+8+i];
    }
}

void filter(int dst,int cnt,int m_coef,int m_state,int flags)
{
    short coef[8];
    static int report=0;
    dst>>=1;
    cnt>>=4; // 16 bytes/block

    loadshort(coef,m_coef,16);
    if(coef[3]==0x7fff && coef[2]==0 && coef[4]==0 && coef[1]==0 && coef[5]==0)
    {
        // unity filter, no need to calc
        loga("\n+filter unity (skipped)");
        return;
    }
    if(1)
    {
        // filtering is slow and doesn't seem to be used for
        // very audible things, ignore for now
        loga("\n+filter ignored!");
        return;
    }
    loga("\n+filter processed");

    // dump filter to console
    {
        int i;
        print("filter: ");
        for(i=0;i<8;i++)
        {
            print("%.2f ",(float)coef[i]/32768.0);
        }
        print("\n");
    }

    loadsamplestate(2048,m_state,flags&1);
    dst+=8;
    while(cnt--)
    {
        filter_block(dst,coef);
        dst+=8;
    }

    savesamplestate(2048,m_state,flags&1);
}

void envmix_zelda(int src,int dst1,int dst2,int eff1,int eff2,int cnt)
{
    int i,j,jn,lx,rx;
    int lteff,rteff;
    int ltmul,ltadd;
    int rtmul,rtadd;
    int lt,rt;
    cnt>>=1;
    src>>=1;
    dst1>>=1;
    dst2>>=1;
    eff1>>=1;
    eff2>>=1;

    lteff=sst.env_lteff;
    rteff=sst.env_rteff;
    ltmul=sst.env_ltvol;
    rtmul=sst.env_rtvol;
    ltadd=sst.env_ltadd;
    rtadd=sst.env_rtadd;

    if(!lteff && !rteff && !ltmul && !ltadd && !rtmul && !rtmul)
    {
        // no need to mix, volumes zero
        return;
    }

    if(st.dumpsnd) loga("\n+envelope_z eff(%04X,%04X) left(%04X,%6i) right(%04X,%6i)",
        lteff,rteff,ltmul,ltadd,rtmul,rtadd);

    if(!lteff && !rteff && !ltadd && !rtadd)
    {
        // no effect mix
        for(j=0;j<cnt;j++)
        {
            lt=rt=sst.mem[src+j];
            lt=(lt*ltmul)>>16;
            rt=(rt*rtmul)>>16;
            lx=lt+sst.mem[dst1+j];
            rx=rt+sst.mem[dst2+j];
            /*
            if(lx<-32767) lx=-32767;
            if(lx> 32767) lx= 32767;
            if(rx<-32767) rx=-32767;
            if(rx> 32767) rx= 32767;
            */
            sst.mem[dst1+j]=lx;
            sst.mem[dst2+j]=rx;
        }
    }
    else if(!ltadd && !rtadd)
    {
        // no volume change
        for(j=0;j<cnt;j++)
        {
            lt=rt=sst.mem[src+j];
            lt=(lt*ltmul)>>16;
            rt=(rt*rtmul)>>16;
            lx=lt+sst.mem[dst1+j];
            rx=rt+sst.mem[dst2+j];
            /*
            if(lx<-32767) lx=-32767;
            if(lx> 32767) lx= 32767;
            if(rx<-32767) rx=-32767;
            if(rx> 32767) rx= 32767;
            */
            sst.mem[dst1+j]=lx;
            sst.mem[dst2+j]=rx;
            lt=(lt*lteff)>>16;
            rt=(rt*rteff)>>16;
            sst.mem[eff1+j]+=lt;
            sst.mem[eff2+j]+=rt;
        }
    }
    else
    {
        for(i=0;i<cnt;i+=8)
        {
            jn=i+8;
            for(j=i;j<jn;j++)
            {
                lt=rt=sst.mem[src+j];
                lt=(lt*ltmul)>>16;
                rt=(rt*rtmul)>>16;
                lx=lt+sst.mem[dst1+j];
                rx=rt+sst.mem[dst2+j];
                /*
                if(lx<-32767) lx=-32767;
                if(lx> 32767) lx= 32767;
                if(rx<-32767) rx=-32767;
                if(rx> 32767) rx= 32767;
                */
                sst.mem[dst1+j]=lx;
                sst.mem[dst2+j]=rx;
                lt=(lt*lteff)>>16;
                rt=(rt*rteff)>>16;
                sst.mem[eff1+j]+=lt;
                sst.mem[eff2+j]+=rt;
            }
            ltmul+=ltadd;
            rtmul+=rtadd;
            if(ltmul>65535)
            {
                ltmul=65535;
                ltadd=0;
            }
            else if(ltmul<0)
            {
                ltmul=0;
                ltadd=0;
            }
            if(rtmul>65535)
            {
                rtmul=65535;
                rtadd=0;
            }
            else if(rtmul<0)
            {
                rtmul=0;
                rtadd=0;
            }
        }
    }
}

void moveloop(int out,int in,int count) // 128 byte loop entries
{
    int i,j;
    out>>=1;
    in>>=1;
    count>>=1;
    for(i=0;i<count;i+=64)
    {
        for(j=0;j<64;j++)
        {
            sst.mem[out+j+i]=sst.mem[in+j];
        }
    }
}

void movehalve(int out,int in,int count)
{
    int i;
    out>>=1;
    in>>=1;
    count>>=1;
    for(i=0;i<count;i++)
    {
        sst.mem[out+i]=sst.mem[in+i*2];
    }
}

void envmix_mario(int src,int dst1,int dst2,int eff1,int eff2,int cnt)
{
    int i,j,jn;
    int lteff,rteff;
    int ltmul,ltadd,lttar;
    int rtmul,rtadd,rttar;
    int lt,rt;
    cnt>>=1;
    src>>=1;
    dst1>>=1;
    dst2>>=1;
    eff1>>=1;
    eff2>>=1;

    lteff=sst.env_lteff<<1;
    rteff=sst.env_rteff<<1;
    ltmul=sst.env_ltvol<<1;
    rtmul=sst.env_rtvol<<1;
    ltadd=sst.env_ltadd<<1;
    rtadd=sst.env_rtadd<<1;
    lttar=sst.env_lttar<<1;
    rttar=sst.env_rttar<<1;

    if(ltadd==65535 && ltadd>0)
    {
        ltmul=lttar;
        ltadd=0;
    }
    if(rtadd==65535 && rtadd>0)
    {
        rtmul=rttar;
        rtadd=0;
    }

    if(!lteff && !rteff && !ltmul && !ltadd && !rtmul && !rtmul==0)
    {
        // no need to mix, volumes zero
        return;
    }

    if(st.dumpsnd) loga("\n+envelope_m eff(%04X,%04X) left(%04X,%6i,%04X) right(%04X,%6i,%04X)",
        lteff,rteff,ltmul,ltadd,lttar,rtmul,rtadd,rttar);

    if(!lteff && !rteff && !ltadd && !rtadd)
    {
        // no effect mix
        for(j=0;j<cnt;j++)
        {
            lt=rt=sst.mem[src+j];
            lt=(lt*ltmul)>>16;
            rt=(rt*rtmul)>>16;
            sst.mem[dst1+j]+=lt;
            sst.mem[dst2+j]+=rt;
        }
    }
    else if(!ltadd && !rtadd)
    {
        // no volume change
        for(j=0;j<cnt;j++)
        {
            lt=rt=sst.mem[src+j];
            lt=(lt*ltmul)>>16;
            rt=(rt*rtmul)>>16;
            sst.mem[dst1+j]+=lt;
            sst.mem[dst2+j]+=rt;
            lt=(lt*lteff)>>16;
            rt=(rt*rteff)>>16;
            sst.mem[eff1+j]+=lt;
            sst.mem[eff2+j]+=rt;
        }
    }
    else
    {
        // full mix (no target)
        for(i=0;i<cnt;i+=8)
        {
            jn=i+8;
            for(j=i;j<jn;j++)
            {
                lt=rt=sst.mem[src+j];
                lt=(lt*ltmul)>>16;
                rt=(rt*rtmul)>>16;
                sst.mem[dst1+j]+=lt;
                sst.mem[dst2+j]+=rt;
                lt=(lt*lteff)>>16;
                rt=(rt*rteff)>>16;
                sst.mem[eff1+j]+=lt;
                sst.mem[eff2+j]+=rt;
            }
            ltmul+=ltadd;
            if(ltadd>0)
            {
                if(ltmul>lttar)
                {
                    ltmul=lttar;
                    ltadd=0;
                }
            }
            else
            {
                if(ltmul<lttar)
                {
                    ltmul=lttar;
                    ltadd=0;
                }
            }
            rtmul+=rtadd;
            if(rtadd>0)
            {
                if(rtmul>rttar)
                {
                    rtmul=rttar;
                    rtadd=0;
                }
            }
            else
            {
                if(rtmul<rttar)
                {
                    rtmul=rttar;
                    rtadd=0;
                }
            }
        }
    }

    sst.env_lteff=lteff>>1;
    sst.env_rteff=rteff>>1;
    sst.env_ltadd=ltadd>>1;
    sst.env_rtadd=rtadd>>1;
    sst.env_ltvol=ltmul>>1;
    sst.env_rtvol=rtmul>>1;
    sst.env_lttar=lttar>>1;
    sst.env_rttar=rttar>>1;
}

void interleave(int dst,int left,int right,int cnt)
{
    int i,a;
    cnt>>=1;
    left>>=1;
    right>>=1;
    dst>>=1;
    for(i=0;i<cnt;i++)
    {
        // upscale volume 2x and check overflows
        // volume is left at 50%, but better overflow protection
        a=(int)sst.mem[left+i];
//        a<<=1;
        if(a<-32767) a=-32767;
        if(a> 32767) a= 32767;
        sst.mem[dst+i*2+0]=a;

        a=(int)sst.mem[right+i];
//        a<<=1;
        if(a<-32767) a=-32767;
        if(a> 32767) a= 32767;
        sst.mem[dst+i*2+1]=a;
    }

    if(st.dumpwav)
    {
        short peak=32767;
//        sound_addwavfile("audio.wav",&peak,2,0);
        sound_addwavfile("audio.wav",sst.mem+dst,cnt*2*2-4,1);
    }
    for(i=0;i<cnt*2;i++)
    { // for the periodic "sound:" report in sync.c
        a=(int)sst.mem[dst+i]; if(a<0) a=-a;
        if(a>st2.audiopeak) st2.audiopeak=a;
    }
    // HLE: not played from here. SAVEBUFF puts the frame in the game's
    // RDRAM buffer, and what the AI takes from osAiSetNextBuffer is what
    // plays (slist_aiplay), as on hardware and in ares. Playing every mixed
    // frame made too much sound when the game mixes ahead (Chameleon
    // Twist, 8% over its 44.1 kHz) and the sync slowed the game down.
    // LLE: played here, as before 385a8dd. lle.c's AI FIFO drains once per
    // retrace, so it looks full when the hardware's has room; taking the
    // game's buffer anyway dropped the waiting one and Paper Mario's music
    // crackled, and refusing it froze Nuclear Strike.
    if(st.lleos && st.soundenable)
    {
        st2.audiorequest=1;
        sound_add(sst.mem+dst,cnt*2*2);
        st2.sync_soundadd+=cnt*2*2;
        st.samples+=cnt;
    }
    sst.lastinterleave=cnt*4;
    loga("\n+interleave; added %i samples",cnt);
    //logi("sound: added %04X bytes\n",cnt*2*2);
}

void mixer(int dst,int src,int cnt,int gain)
{
    int i,r;
    loga("\n+mixer gain %04X ",gain&0xffff);
    dst>>=1;
    src>>=1;
    cnt>>=1;
    for(i=0;i<cnt;i++)
    {
        r=(sst.mem[src+i]*gain)>>15;
        r+=sst.mem[dst+i];
        if(r<-32767) r=-32767;
        if(r> 32767) r= 32767;
        sst.mem[dst+i]=r;
    }
}

void boost(int dst,int cnt,int gain)
{
    int i,r;
    loga("\n+boost gain %04X ",gain&0xffff);
    dst>>=1;
    cnt>>=1;
    for(i=0;i<cnt;i++)
    {
        r=(sst.mem[dst+i]*gain)>>8;
        if(r<-32767) r=-32767;
        if(r> 32767) r= 32767;
        sst.mem[dst+i]=r;
    }
}

void envmix_loadstate(dword addr)
{
    sst.env_ltvol=mem_read32p(addr+ 0);
    sst.env_rtvol=mem_read32p(addr+ 4);
    sst.env_lteff=mem_read32p(addr+ 8);
    sst.env_rteff=mem_read32p(addr+12);
    sst.env_ltadd=mem_read32p(addr+16);
    sst.env_rtadd=mem_read32p(addr+20);
    sst.env_lttar=mem_read32p(addr+24);
    sst.env_rttar=mem_read32p(addr+28);
}

void envmix_savestate(dword addr)
{
    mem_write32(addr+ 0,sst.env_ltvol);
    mem_write32(addr+ 4,sst.env_rtvol);
    mem_write32(addr+ 8,sst.env_lteff);
    mem_write32(addr+12,sst.env_rteff);
    mem_write32(addr+16,sst.env_ltadd);
    mem_write32(addr+20,sst.env_rtadd);
    mem_write32(addr+24,sst.env_lttar);
    mem_write32(addr+28,sst.env_rttar);
}

void envmix_clearstate(void)
{
    sst.env_lteff=0;
    sst.env_rteff=0;
    sst.env_ltadd=0;
    sst.env_rtadd=0;
    sst.env_lttar=0;
    sst.env_rttar=0;
    sst.env_ltvol=0;
    sst.env_rtvol=0;
}

void envmix_simplestate(void)
{
    sst.env_lteff=0;
    sst.env_rteff=0;
    sst.env_ltadd=0;
    sst.env_rtadd=0;
    sst.env_lttar=0;
    sst.env_rttar=0;
    sst.env_ltvol=16384;
    sst.env_rtvol=16384;
}

/*************************************************************************/

static int adpcmcnt=0;
static int cmdcnt=0;

void saveaudiomem(void)
{
    static int done=0;
    if(!adpcmcnt) return;
    if(!done)
    {
        FILE *f1;
        f1=fopen("audiomem.dat","wb");
        fwrite(sst.mem,1,4096+1024,f1);
        fclose(f1);
        done=1;
    }
}

static __inline dword address(dword address)
{ // segment convert to physical address
    return( (address&0xffffff) + sst.segment );
}

static int audio_detected; // 1 when the audio ucode was identified (see slist_detectucode)

// naudio volume state (Mupen64Plus hle->alist_naudio): set by SETVOL
static struct
{
    short vol[2],target[2],dry,wet;
    int   rate[2];
} nau;

typedef struct { __int64 value,step,target; } Ramp;

// Mupen64Plus ramp_step: move toward the target, stop on reaching it
static short ramp_step(Ramp *r)
{
    int reached;
    r->value+=r->step;
    reached=(r->step<=0)?(r->value<=r->target):(r->value>=r->target);
    if(reached) { r->value=r->target; r->step=0; }
    return((short)(r->value>>16));
}

static short clamp16(long v) { return((short)(v<-32768?-32768:v>32767?32767:v)); }

// Mupen64Plus alist_envmix_lin (naudio ENVMIXER): the left and right volume
// ramp linearly; each sample is added to dry left/right and wet left/right
// at volume*dry or volume*wet. The state (80 bytes at address) is ours: the
// ramps and levels between lists. naudio's volumes were mixed by ABI1's
// envmixer with its own rate units before, which clipped once ADPCM decoded
// at full level (Nuclear Strike peaked at 32767).
static void envmix_lin(int init,int dl,int dr,int wl,int wr,int in,int count,dword address)
{
    Ramp  ramp[2];
    short dry=nau.dry,wet=nau.wet;
    int   k,j;

    if(init)
    {
        for(j=0;j<2;j++)
        {
            ramp[j].step  =nau.rate[j]/8;
            ramp[j].value =(__int64)nau.vol[j]<<16;
            ramp[j].target=(__int64)nau.target[j]<<16;
        }
    }
    else
    {
        wet=(short)mem_read16(address+0);
        dry=(short)mem_read16(address+2);
        for(j=0;j<2;j++)
        {
            ramp[j].target=(__int64)(short)mem_read16(address+4+j*2)<<16;
            ramp[j].step  =(int)mem_read32p(address+8+j*4);
            ramp[j].value =(int)mem_read32p(address+32+j*4);
        }
    }
    dl>>=1; dr>>=1; wl>>=1; wr>>=1; in>>=1;
    count>>=1;
    for(k=0;k<count;k++)
    {
        short lv=ramp_step(&ramp[0]),rv=ramp_step(&ramp[1]);
        short s=sst.mem[in+k];
        short g[4];
        g[0]=clamp16(((long)lv*dry+0x4000)>>15);
        g[1]=clamp16(((long)rv*dry+0x4000)>>15);
        g[2]=clamp16(((long)lv*wet+0x4000)>>15);
        g[3]=clamp16(((long)rv*wet+0x4000)>>15);
        sst.mem[dl+k]=clamp16(sst.mem[dl+k]+(((long)s*g[0])>>15));
        sst.mem[dr+k]=clamp16(sst.mem[dr+k]+(((long)s*g[1])>>15));
        sst.mem[wl+k]=clamp16(sst.mem[wl+k]+(((long)s*g[2])>>15));
        sst.mem[wr+k]=clamp16(sst.mem[wr+k]+(((long)s*g[3])>>15));
    }
    mem_write16(address+0,(word)wet);
    mem_write16(address+2,(word)dry);
    for(j=0;j<2;j++)
    {
        mem_write16(address+4+j*2,(word)(short)(ramp[j].target>>16));
        mem_write32(address+8+j*4,(dword)(int)ramp[j].step);
        mem_write32(address+32+j*4,(dword)(int)ramp[j].value);
    }
}

void slist_banjo(OSTask_t *task)
{
    int cmdnum,c,lastc;
    dword cmd[8],dlpnt;

    cmdcnt=0;
    sst.segment=0;

    cmdnum=task->data_size/8;
    dlpnt=address(task->m_data_ptr);
    while(cmdnum--)
    {
        cmd[0]=mem_read32p(dlpnt+0);
        cmd[1]=mem_read32p(dlpnt+4);
        c=cmd[0]>>24;
        dlpnt+=8;

        if(st.dumpsnd)
        {
            int j;
            loga("%08X: %08X %08X ",dlpnt-8,cmd[0],cmd[1]);
            for(j=0;mario_cmdnames[j].name;j++)
            {
                if(mario_cmdnames[j].cmd==c) break;
            }
            if(!mario_cmdnames[j].name)
            {
                loga("??? ");
            }
            else loga("%-12s ",mario_cmdnames[j].name);
        }

        cmdcnt++;

        if(c<0x20 && c>0x10 && !audio_detected) // guess only if ucode unknown
        {
            cart.slist_type=1;
            return;
        }

    sst.in=0;
    sst.out=0;
    sst.cnt=0;
// different tabbing than zelda to distinquish in quick searches
switch(c)
{
case 0x7: // SEGMENT
    {
    } break;
case 0x8: // SETBUFF
    {
        int in,out,count,flags;
        flags=FIELD(cmd[0],16,8);
        in   =FIELD(cmd[0],0,16);
        out  =FIELD(cmd[1],16,16);
        count=FIELD(cmd[1],0,16);
        if(flags&8)
        {
            sst.aux1=in;
            sst.aux2=out;
            sst.aux3=count;
        }
        else
        {
            sst.in=in;
            sst.out=out;
            sst.cnt=count;
        }
        loga("%04X -> %04X (%03X) ",in,out,count);
    } break;
case 0xa: // DMEMMOVE
    {
        int in,out,count,flags;
        flags=FIELD(cmd[0],16,8);
        in   =FIELD(cmd[0],0,16);
        out  =FIELD(cmd[1],16,16);
        count=FIELD(cmd[1],0,16);
        if(in>=4096 || out>=4096 || in+count>4096 || out+count>4096)
        {
            error("slist: MEMCOPY overflow");
        }
        loga("%04X -> %04X (count %04X, flag %02X) ",in,out,count,flags);
        movemem(out,in,count);
    } break;
case 0x2: // CLEARBUFF
    {
        int out,count;
        out  =FIELD(cmd[0],0,16);
        count=FIELD(cmd[1],0,16);
        if(st.dumpsnd) loga("%04X (%04X) ",out,count);
        zeromem(out,count);
    } break;
case 0xF: // SETLOOP
    {
        sst.m_adpcmloopdata=cmd[1];
    } break;
case 0xB: // LOADADPCM
    {
        dword addr=cmd[1]+sst.segment;
        int   len =FIELD(cmd[0],0,12);
        if(len>512)
        { // 16 predictors fill the 512 bytes after DMEM in sst.mem
            static int seen;
            if(!seen++) error("slist: codebook of %i bytes, 512 kept",len);
            len=512;
        }
        loadmem(4096,addr,len);
        sst.lastcodebook=addr;
    } break;
case 0x4: // LOADBUFF
    {
        int dst=FIELD(cmd[0],0,12);
        int len=FIELD(cmd[0],12,12);
        dword addr=cmd[1]+sst.segment;
        if(st.dumpsnd) loga("%04X (%03X) <- %08X",dst,len,addr);
        loadmem(dst,addr,len);
        sst.lastloadb_from=cmd[1];
        sst.lastloadb_outbeg=dst;
        sst.lastloadb_outend=dst+len;
    } break;
case 0x6: // SAVEBUFF
    {
        int dst=FIELD(cmd[0],0,12);
        int len=FIELD(cmd[0],12,12);
        dword addr=cmd[1]+sst.segment;
        if(st.dumpsnd) loga("%04X (%03X) -> %08X",dst,len,addr);
        savemem(dst,addr,len);
        if(KOE && lastc==0xD) print("--interleave %08X\n",cmd[1]);
    } break;
case 0x1: // ADPCM
    {
        int flags=FIELD(cmd[0],16,8);
        int state=cmd[1];
        int cnt  =sst.cnt;

        state=cmd[0]&0xffffff;
        sst.in=FIELD(cmd[1],12,4);
        sst.out=FIELD(cmd[1],0,12);
        sst.cnt=FIELD(cmd[1],16,12);

        // init and loop only: naudio has no 2-bit ADPCM (Mupen64Plus
        // alist_naudio passes two_bit false); bit 2 made adpcm() decode
        // 4-bit samples as 2-bit, garbling Nuclear Strike's sound
        flags=FIELD(cmd[1],28,4)&3;
        if(st.dumpsnd) loga("%04X -> %04X (%03X) ",
            sst.in,sst.out,sst.cnt);
        adpcm(state,sst.in,sst.out,sst.cnt,flags);
        adpcmcnt++;
    } break;
case 0x5: // RESAMPLE
    {
        int flags=FIELD(cmd[1],30,2);
        int speed=FIELD(cmd[1],14,16);
        int state=cmd[0]&0xffffff;

        sst.in=FIELD(cmd[1],2,12);
        sst.out=(cmd[1]&3)?0x170:0x0;
        sst.cnt=0x170;

        if(st.dumpsnd) loga("%04X -> %04X (%03X) speed %04X",
            sst.in,sst.out,sst.cnt,speed);
        resample(state,sst.in,sst.out,sst.cnt,flags,speed);
    } break;
case 0xC: // MIXER
    {
        // naudio has no SETBUFF: always one frame (Mupen64Plus NAUDIO_COUNT)
        int cnt=0x170;
        int gain=cmd[0]&0xffff;
        int src=cmd[1]>>16;
        int dst=cmd[1]&0xffff;
        gain=(short)gain;
        if(st.dumpsnd) loga("%04X -> %04X (%03X) gain %04X",
            src,dst,cnt,gain&0xffff);
        mixer(dst,src,cnt,gain);
    } break;
case 0xD: // INTERLEAVE
    {
        int dst=sst.out;
        int left=cmd[1]>>16;
        int right=cmd[1]&0xffff;
        int cnt=sst.cnt;

        left =0x4e0; //+09D0 = 4E0
        right=0x650; //+0B40 = 650
        dst  =0x0;
        cnt  =0x170; //      = 170

        if(st.dumpsnd) loga("%04X,%04X -> %04X (%03X)",
            left,right,dst,cnt);

        interleave(dst,left,right,cnt);
        st2.snd_interl++;
        sst.envmixcnt=0;
    } break;
case 0x0E: // POLEFILTER
    {
        // ignored
    } break;
case 0x9: // SETVOL (Mupen64Plus alist_naudio SETVOL)
    {
        int flag=FIELD(cmd[0],16,8);
        if(flag&4) // A_VOL
        {
            if(flag&2) // A_LEFT: left volume, dry and wet levels
            {
                nau.vol[0]=(short)cmd[0];
                nau.dry   =(short)(cmd[1]>>16);
                nau.wet   =(short)cmd[1];
            }
            else // A_RIGHT: right target and rate
            {
                nau.target[1]=(short)cmd[0];
                nau.rate[1]  =(int)cmd[1];
            }
        }
        else // A_RATE: left target and rate
        {
            nau.target[0]=(short)cmd[0];
            nau.rate[0]  =(int)cmd[1];
        }
    } break;
case 0x03: // ENVMIXER (Mupen64Plus alist_naudio ENVMIXER, alist_envmix_lin)
    {
        // naudio DMEM, relative to NAUDIO_MAIN (0x4F0) like the rest here:
        // in 000, dry left 4E0, dry right 650, wet left 7C0, wet right 930
        nau.vol[1]=(short)cmd[0];
        envmix_lin(FIELD(cmd[0],16,8)&1,0x4e0,0x650,0x7c0,0x930,0x000,0x170,cmd[1]&0xffffff);
        st2.snd_envmix++;
    } break;
default:
    loga("???unimplemented");
    sst.errorcnt[c]++;
    if(sst.errorcnt[c]<10) warning("unimplemented slist op %02X",c);
    break;
}
        if(c!=8) lastc=c;

        loga("\n");
    }
}


void slist_mario(OSTask_t *task)
{
    int cmdnum,c,lastc;
    dword cmd[8],dlpnt;

    cmdcnt=0;
    sst.segment=0;

    cmdnum=task->data_size/8;
    dlpnt=address(task->m_data_ptr);
    while(cmdnum--)
    {
        cmd[0]=mem_read32p(dlpnt+0);
        cmd[1]=mem_read32p(dlpnt+4);
        c=cmd[0]>>24;
        dlpnt+=8;

        if(st.dumpsnd)
        {
            int j;
            loga("%08X: %08X %08X ",dlpnt-8,cmd[0],cmd[1]);
            for(j=0;mario_cmdnames[j].name;j++)
            {
                if(mario_cmdnames[j].cmd==c) break;
            }
            if(!mario_cmdnames[j].name)
            {
                loga("??? ");
            }
            else loga("%-12s ",mario_cmdnames[j].name);
        }

        cmdcnt++;

        if(c<0x20 && c>0x10 && !audio_detected) // guess only if ucode unknown
        {
            cart.slist_type=1;
            return;
        }

        switch(c)
        {
        case 0x7: // SEGMENT
            {
            } break;
        case 0x8: // SETBUFF
            {
                int in,out,count,flags;
                flags=FIELD(cmd[0],16,8);
                in   =FIELD(cmd[0],0,16);
                out  =FIELD(cmd[1],16,16);
                count=FIELD(cmd[1],0,16);
                if(flags&8)
                {
                    sst.aux1=in;
                    sst.aux2=out;
                    sst.aux3=count;
                }
                else
                {
                    sst.in=in;
                    sst.out=out;
                    sst.cnt=count;
                }
                loga("%04X -> %04X (%03X) ",in,out,count);
            } break;
        case 0xa: // DMEMMOVE
            {
                int in,out,count,flags;
                flags=FIELD(cmd[0],16,8);
                in   =FIELD(cmd[0],0,16);
                out  =FIELD(cmd[1],16,16);
                count=FIELD(cmd[1],0,16);
                if(in>=4096 || out>=4096 || in+count>4096 || out+count>4096)
                {
                    error("slist: MEMCOPY overflow");
                }
                loga("%04X -> %04X (count %04X, flag %02X) ",in,out,count,flags);
                movemem(out,in,count);
            } break;
        case 0x2: // CLEARBUFF
            {
                int out,count;
                out  =FIELD(cmd[0],0,16);
                count=FIELD(cmd[1],0,16);
                if(st.dumpsnd) loga("%04X (%04X) ",out,count);
                zeromem(out,count);
            } break;
        case 0x4: // LOADBUFF
            {
                int flags=FIELD(cmd[0],16,8);
                int dst=sst.in;
                int len=sst.cnt;
                dword addr=cmd[1]+sst.segment;
                if(st.dumpsnd) loga("%04X (%03X) <- %08X",dst,len,addr);
                loadmem(dst,addr,len);
                sst.lastloadb_from=cmd[1];
                sst.lastloadb_outbeg=dst;
                sst.lastloadb_outend=dst+len;
            } break;
        case 0xF: // SETLOOP
            {
                sst.m_adpcmloopdata=cmd[1];
            } break;
        case 0xB: // LOADADPCM
            {
                dword addr=cmd[1]+sst.segment;
                int   len =FIELD(cmd[0],0,12);
                loadmem(4096,addr,len);
                sst.lastcodebook=addr;
            } break;
        case 0x6: // SAVEBUFF
            {
                int flags=FIELD(cmd[0],16,8);
                int dst=sst.out;
                int len=sst.cnt;
                dword addr=cmd[1]+sst.segment;
                if(st.dumpsnd) loga("%04X (%03X) -> %08X",dst,len,addr);
                savemem(dst,addr,len);
                if(KOE && lastc==0xD) print("--interleave %08X\n",cmd[1]);
            } break;
        case 0x1: // ADPCM
            {
                // init and loop only: ABI1 has no 2-bit ADPCM (Mupen64Plus
                // alist_audio passes two_bit false)
                int flags=FIELD(cmd[0],16,8)&3;
                int state=cmd[1];
                int cnt  =sst.cnt;
                state=cmd[1];
                if(st.dumpsnd) loga("%04X -> %04X (%03X) ",
                    sst.in,sst.out,sst.cnt);
                adpcm(state,sst.in,sst.out,sst.cnt,flags);
                adpcmcnt++;
            } break;
        case 0x5: // RESAMPLE
            {
                int flags=FIELD(cmd[0],16,8);
                int state=cmd[1];
                int speed=cmd[0]&0xffff;
                state=cmd[1];
                if(st.dumpsnd) loga("%04X -> %04X (%03X) speed %04X",
                    sst.in,sst.out,sst.cnt,speed);
                resample(state,sst.in,sst.out,sst.cnt,flags,speed);
            } break;
        case 0xC: // MIXER
            {
                int cnt=sst.cnt;
                int gain=cmd[0]&0xffff;
                int src=cmd[1]>>16;
                int dst=cmd[1]&0xffff;
                if(cnt==0 && !audio_detected) // guess only if ucode unknown
                {
                    cart.slist_type=2;
                    return;
                }
                gain=(short)gain;
                if(st.dumpsnd) loga("%04X -> %04X (%03X) gain %04X",
                    src,dst,cnt,gain);
                mixer(dst,src,cnt,gain);
            } break;
        case 0xD: // INTERLEAVE
            {
                int dst=sst.out;
                int left=cmd[1]>>16;
                int right=cmd[1]&0xffff;
                int cnt=sst.cnt;
                if(st.dumpsnd) loga("%04X,%04X -> %04X (%03X)",
                    left,right,dst,cnt);
                interleave(dst,left,right,cnt);
                st2.snd_interl++;
                sst.envmixcnt=0;
            } break;
        case 0x0E: // POLEFILTER
            {
                // ignored
            } break;
        case 0x9: // SETVOL
            {
                int flag=FIELD(cmd[0],16, 8);
                int vol =FIELD(cmd[0], 0,16);
                int tgt =FIELD(cmd[1],16,16);
                int rate=FIELD(cmd[1], 0,16);
                switch(flag)
                {
                case  8: sst.env_lteff=rate;
                         sst.env_rteff=rate;
                         // rate should also affect something, pan?
                         break;
                case  6: sst.env_ltvol=vol;
                         break;
                case  4: sst.env_rtvol=vol;
                         break;
                case  2: sst.env_lttar=vol;
                         if(tgt) sst.env_ltadd=8*(rate&0xffff);
                         else    sst.env_ltadd=8*((short)rate);
                         if(sst.env_ltadd>65535) sst.env_ltadd=65535;
                         if(sst.env_ltadd<-65535) sst.env_ltadd=-65535;
                         break;
                case  0: sst.env_rttar=vol;
                         if(tgt) sst.env_rtadd=8*(rate&0xffff);
                         else    sst.env_rtadd=8*((short)rate);
                         if(sst.env_rtadd>65535) sst.env_rtadd=65535;
                         if(sst.env_rtadd<-65535) sst.env_rtadd=-65535;
                         break;
                default: loga("ignored! "); break;
                }
            } break;
        case 0x03: // ENVMIXER
            {
                int flag=FIELD(cmd[0],16,8);
                int src =sst.in;
                int cnt =sst.cnt;
                int dst1=sst.out;
                int dst2=sst.aux1;
                int tmp1=sst.aux2;
                int tmp2=sst.aux3;
                if(!(flag&1)) envmix_loadstate(cmd[1]);
                if(st.dumpsnd) loga("%04X -> %04X,%04X,%04X,%04X (%03X)",src,dst1,dst2,tmp1,tmp2,cnt);
                envmix_mario(src,dst1,dst2,tmp1,tmp2,cnt);
                envmix_savestate(cmd[1]);
                envmix_clearstate();
                st2.snd_envmix++;
            } break;
        default:
            loga("???unimplemented");
            sst.errorcnt[c]++;
            if(sst.errorcnt[c]<10) warning("unimplemented slist op %02X",c);
            break;
        }
        if(c!=8) lastc=c;

        loga("\n");
    }
}

void slist_zelda(OSTask_t *task)
{
    int cmdnum,c;
    dword cmd[8],dlpnt;

    cmdcnt=0;

    sst.segment=0;

    cmdnum=task->data_size/8;
    dlpnt=address(task->m_data_ptr);
    while(cmdnum--)
    {
        cmd[0]=mem_read32p(dlpnt+0);
        cmd[1]=mem_read32p(dlpnt+4);
        c=cmd[0]>>24;
        dlpnt+=8;

        if(st.dumpsnd)
        {
            int j;
            loga("%08X: %08X %08X ",dlpnt-8,cmd[0],cmd[1]);
            for(j=0;zelda_cmdnames[j].name;j++)
            {
                if(zelda_cmdnames[j].cmd==c) break;
            }
            if(!zelda_cmdnames[j].name)
            {
                loga("??? ");
            }
            else loga("%-12s ",zelda_cmdnames[j].name);
            /*
            loga("%03X %03X  %04X %04X ",
                FIELD(cmd[0],12,12),
                FIELD(cmd[0], 0,12),
                FIELD(cmd[1],16,16),
                FIELD(cmd[1], 0,16));
            */
        }

        cmdcnt++;

        switch(c)
        {
        case 0x8: // SETBUFF
            {
                int in,out,count,flags;
                flags=FIELD(cmd[0],16,8);
                in   =FIELD(cmd[0],0,16);
                out  =FIELD(cmd[1],16,16);
                count=FIELD(cmd[1],0,16);
                if(flags&8)
                {
                    sst.aux1=in;
                    sst.aux2=out;
                    sst.aux3=count;
                }
                else
                {
                    sst.out=out;
                    sst.in=in;
                    sst.cnt=count;
                }
                if(st.dumpsnd) loga("%04X -> %04X (%03X) flags %02X",in,out,count,flags);
            } break;
        case 0x9:  // MEMLOOP
            {
                int in,out,count;
                count=FIELD(cmd[0],16,8)*128;
                in   =FIELD(cmd[0],0,16);
                out  =FIELD(cmd[1],16,16);
                if(st.dumpsnd) loga("%04X -> %04X (%03X) ",in,out,count);
                moveloop(out,in,count);
            } break;
        case 0xa:  // MEMMOVE
            {
                int in,out,count,flags;
                flags=FIELD(cmd[0],16,8);
                in   =FIELD(cmd[0],0,16);
                out  =FIELD(cmd[1],16,16);
                count=FIELD(cmd[1],0,16);
                if(st.dumpsnd) loga("%04X -> %04X (%03X) ",in,out,count);
                movemem(out,in,count);
            } break;
        case 0x11: // MEMHALVE
            {
                int in,out,count,flags;
                flags=FIELD(cmd[0],16,8);
                count=FIELD(cmd[0],0,16)*2;
                in   =FIELD(cmd[1],16,16);
                out  =FIELD(cmd[1],0,16);
                if(st.dumpsnd) loga("%04X -> %04X (%03X) ",in,out,count);
                movehalve(out,in,count);
            } break;
        case 0x14: // LOADBUFF
            {
                dword addr=cmd[1]+sst.segment;
                int   len =FIELD(cmd[0],12,12);
                int   dst =FIELD(cmd[0],0,12);
                if(st.dumpsnd) loga("%04X (%03X) <- %08X",dst,len,addr);
                loadmem(dst,addr,len);
                sst.lastloadb_from=cmd[1];
                sst.lastloadb_outbeg=dst;
                sst.lastloadb_outend=dst+len;
            } break;
        case 0x15: // SAVEBUFF
            {
                dword addr=cmd[1]+sst.segment;
                int   len =FIELD(cmd[0],12,12);
                int   dst =FIELD(cmd[0],0,12);
                if(st.dumpsnd) loga("%04X (%03X) -> %08X",dst,len,addr);
                savemem(dst,addr,len);
            } break;
        case 0x2: // CLEARBUFF
            {
                int out,count;
                out  =FIELD(cmd[0],0,16);
                count=FIELD(cmd[1],0,16);
                if(st.dumpsnd) loga("%04X (%04X) ",out,count);
                zeromem(out,count);
            } break;
        case 0xf: // ADPCMLOOP
            {
                sst.m_adpcmloopdata=cmd[1];
            } break;
        case 0xB: // ADPCMCODEB
            {
                dword addr=cmd[1]+sst.segment;
                int   len =FIELD(cmd[0],0,12);
                if(st.dumpsnd) loga("code (%03X) <- %08X",len,addr);
                loadmem(4096,addr,len);
                sst.lastcodebook=addr;
                sst.lastcodebooklen=len;
            } break;
        case 0x1: // ADPCM
            {
                int flags=FIELD(cmd[0],16,8);
                int state=cmd[1];
                int cnt  =cmd[0]&0xffff;
                state=cmd[1];
                if(st.dumpsnd) loga("%04X -> %04X (%03X) ",
                    sst.in,sst.out,sst.cnt);
                adpcm(state,sst.in,sst.out,sst.cnt,flags);
                adpcmcnt++;
            } break;
        case 0x5: // RESAMPLE
            {
                int flags=FIELD(cmd[0],16,8);
                int state=cmd[1];
                int speed=cmd[0]&0xffff;
                state=cmd[1];
                if(st.dumpsnd) loga("%04X -> %04X (%03X) speed %04X",
                    sst.in,sst.out,sst.cnt,speed);
                resample(state,sst.in,sst.out,sst.cnt,flags,speed);
            } break;
        case 0x12: // ENVSET1
            {
                sst.env_lteff=FIELD(cmd[0], 8,16)&0xff00;
                sst.env_rteff=(short)(FIELD(cmd[0], 0,16)&0xffff)+sst.env_lteff;
                sst.env_ltadd=(short)(FIELD(cmd[1],16,16)&0xffff);
                sst.env_rtadd=(short)(FIELD(cmd[1], 0,16)&0xffff);
            } break;
        case 0x16: // ENVSET2
            {
                sst.env_ltvol=FIELD(cmd[1], 0,16)&0xffff;
                sst.env_rtvol=FIELD(cmd[1],16,16)&0xffff;
            } break;
        case 0x13: // ENVMIXER
            {
                int src =FIELD(cmd[0],16,8)*16;
                int cnt =FIELD(cmd[0], 8,8)*2 ;
                int flag=FIELD(cmd[0], 0,8)   ;
                int dst1=FIELD(cmd[1],24,8)*16;
                int dst2=FIELD(cmd[1],16,8)*16;
                int tmp1=FIELD(cmd[1], 8,8)*16;
                int tmp2=FIELD(cmd[1], 0,8)*16;
                if(st.dumpsnd) loga("%04X -> %04X,%04X,%04X,%04X (%03X)",
                    src,dst1,dst2,tmp1,tmp2,cnt);
                envmix_zelda(src,dst1,dst2,tmp1,tmp2,cnt);
                st2.snd_envmix++;
            } break;
        case 0x4: // MIXER2
        case 0xC: // MIXER
            {
                int cnt=((cmd[0]>>16)&255)*16;
                int gain=(cmd[0]&0xffff);
                int src=cmd[1]>>16;
                int dst=cmd[1]&0xffff;
                gain=(short)gain;
                if(st.dumpsnd) loga("%04X -> %04X (%03X) gain %04X",
                    src,dst,cnt,gain);
                mixer(dst,src,cnt,gain);
            } break;
        case 0xE: // boost
            {
                int cnt =FIELD(cmd[0],0,16);
                int gain=FIELD(cmd[0],16,8)*16;
                int dst =cmd[1]>>16;
                if(st.dumpsnd) loga("%04X (%03X) gain %04X",
                    dst,cnt,gain);
                boost(dst,cnt,gain);
            } break;
        case 0xD: // INTERLEAVE
            {
                int dst=cmd[0]&0xffff;
                int left=cmd[1]>>16;
                int right=cmd[1]&0xffff;
                int cnt=((cmd[0]>>16)&255)*16;
                if(!cnt) cnt=sst.cnt;
                if(st.dumpsnd) loga("%04X,%04X -> %04X (%03X)",
                    left,right,dst,cnt);
                interleave(dst,left,right,cnt);
                st2.snd_interl++;
                sst.envmixcnt=0;
            } break;
        case 0x7: // FILTER
            {
                int flag=FIELD(cmd[0],16,8);
                static int cnt,coef,dst,state;
                if(flag&2)
                {
                    cnt  =FIELD(cmd[0],0,16);
                    coef =cmd[1];
                }
                else
                {
                    dst  =FIELD(cmd[0],0,16);
                    state=cmd[1];
                    if(st.dumpsnd)
                    {
                        if(flag&2) printcurvedata(cmd[1],16);
                    }
                    filter(dst,cnt,coef,state,flag);
                }
            }
            break;
        default:
            loga("???unimplemented");
            sst.errorcnt[c]++;
            if(sst.errorcnt[c]<10) warning("unimplemented slist op %02X",c);
            break;
        }

        loga("\n");
    }
}

/*************************************************************************
** Audio microcode detection
**
** Identifies the audio microcode from signature words in its data segment
** (the values documented by mupen64plus-rsp-hle's hle.c), instead of guessing
** from the game title and from the commands in the list:
**   data[0]==1 && data[0x30]==0xf0000f00 : ABI1, identified by data[0x28]
**   data[0]==1 otherwise                  : NEAD (ABI2), by data[0x10]
**   data[0]!=1                            : naudio / MusyX, by data[0x10]
*/

#define AUDIO_UNKNOWN -1 // keep the old title/command guessing
#define AUDIO_MARIO    0 // slist_mario  (ABI1)
#define AUDIO_ZELDA    1 // slist_zelda  (NEAD)
#define AUDIO_BANJO    2 // slist_banjo  (naudio)
#define AUDIO_NONE     3 // no handler (MusyX): skip the list, silence
#define AUDIO_MUSYX1   4 // musyx_v1_task (ultra.ini musyxhle=1)
#define AUDIO_MUSYX2   5 // musyx_v2_task (ultra.ini musyxhle=1)

void musyx_v1_task(dword data_ptr,dword data_size);
void musyx_v2_task(dword data_ptr,dword data_size);


static int slist_detectucode(OSTask_t *task)
{
    static dword lastaddr=0xffffffff;
    static int   lasttype,lastmusyx;
    dword d=task->m_ucode_data;
    dword v;
    int   type=AUDIO_UNKNOWN;
    const char *name="unknown";

    if(d==lastaddr && cart.musyxhle==lastmusyx) return(lasttype);

    if(mem_read32p(d)==1 && mem_read32p(d+0x30)==0xf0000f00)
    {
        v=mem_read32p(d+0x28);
        switch(v)
        {
        case 0x1e24138c: type=AUDIO_MARIO; name="ABI1"; break;
        case 0x1dc8138c: type=AUDIO_MARIO; name="ABI1 (GoldenEye)"; break;
        case 0x1e3c1390: type=AUDIO_MARIO; name="ABI1 (Blast Corps/DKR)"; break;
        }
    }
    else
    {
        v=mem_read32p(d+0x10);
        if(mem_read32p(d)==1) switch(v)
        {
        case 0x11181350: // Mario Kart, Wave Race (E)
        case 0x111812e0: // Star Fox (J)
        case 0x110412ac: // Wave Race (J rev B)
        case 0x110412cc: // Star Fox / Lylat Wars
        case 0x1cd01250: // F-Zero X
        case 0x1f08122c: // Yoshi's Story
        case 0x1f38122c: // 1080 Snowboarding
        case 0x1f681230: // Zelda OoT, MM (J)
        case 0x1f801250: // Zelda MM, Pokemon Stadium 2
        case 0x109411f8: // Zelda MM (E beta)
        case 0x1eac11b8: // Animal Crossing
        case 0x1f701238: // Mario Artist Talent Studio
        case 0x1f4c1230: // F-Zero X Expansion
            type=AUDIO_ZELDA; name="NEAD"; break;
        case 0x00010010: // Indiana Jones, Battle for Naboo
            if(cart.musyxhle) { type=AUDIO_MUSYX2; name="MusyX v2"; }
            else { type=AUDIO_NONE; name="MusyX v2 (RSP)"; }
            break;
        }
        else switch(v)
        {
        case 0x0000127c: // many games
        case 0x00001280: // Banjo-Kazooie
        case 0x1c58126c: // Donkey Kong 64
        case 0x1ae8143c: // Banjo-Tooie, Perfect Dark (mp3)
        case 0x1ab0140c: // Conker's Bad Fur Day
            type=AUDIO_BANJO; name="naudio"; break;
        case 0x00000001: // Rogue Squadron, Resident Evil 2
            if(cart.musyxhle) { type=AUDIO_MUSYX1; name="MusyX v1"; }
            else { type=AUDIO_NONE; name="MusyX v1 (RSP)"; }
            break;
        }
    }

    print("slist: audio microcode %s (data %08X, sig %08X)\n",name,d,v);
    lastaddr=d;
    lasttype=type;
    lastmusyx=cart.musyxhle;
    return(type);
}

// can slist_execute run this audio task? MusyX without musyxhle can't: its
// tasks go to the RSP interpreter, and the game's AI buffers are played
// (slist_nextbuffer plays every buffer the AI takes). The World Is Not
// Enough.
int slist_hle(OSTask_t *task)
{
    return(slist_detectucode(task)!=AUDIO_NONE);
}

void slist_execute(OSTask_t *task)
{
    int audiotype;
    static int initdone;
    static int firsttime=1;
    int starttime;

    // MusyX mixes into RDRAM and the game queues that on the AI, which
    // plays it: no soundlists flag (in LLE it stops AI buffers playing)
    audiotype=slist_detectucode(task);
    if(audiotype==AUDIO_MUSYX1 || audiotype==AUDIO_MUSYX2)
    {
        starttime=timer_us(&st2.timer);
        if(audiotype==AUDIO_MUSYX1) musyx_v1_task(task->m_data_ptr,task->data_size);
        else                        musyx_v2_task(task->m_data_ptr,task->data_size);
        st.us_audio+=timer_us(&st2.timer)-starttime;
        return;
    }

    st2.audiorequest=1;
    st2.soundlists=1;

    starttime=timer_us(&st2.timer);

    if(!initdone)
    {
        initdone=1;
        memset(sst.mem,0,4096);
    }

    loga("Soundlist:\n");
    loga("list: bootucode %5i bytes\n",task->ucode_boot_size);
    loga("list: ucode     %5i bytes (%08X)\n",task->ucode_size,task->m_ucode);
    loga("list: ucodedata %5i bytes\n",task->ucode_data_size);
    loga("list: dramstack %5i bytes\n",task->dram_stack_size);
    loga("list: data      %5i bytes (%08X)\n",task->data_size,task->m_data_ptr);
    loga("list: yielddata %5i bytes\n",task->yield_data_size);
    loga("list: %i commands:\n",task->data_size/8);

    sst.adpcmerror_reported=0;

    if(firsttime && st.dumpsnd)
    {
        firsttime=0;
        disasm_dumpucode("rspboot.log",
            task->m_ucode_boot,4096,
            0,16,0x1000);
        disasm_dumpucode("rspaudio.log",
            task->m_ucode     ,4096,
            task->m_ucode_data,task->ucode_data_size,
            cart.iszelda?0x1000:0x1080);
        loga("Audio RSP microcode/data dumped to RSPAUDIO.LOG\n");
    }

    adpcm_inittables();

    if(task->data_size/8>10000)
    {
        error("slist: Sound list too large (%i commands)\n",task->data_size/8);
        return;
    }

    sst.segment=0;

    audiotype=slist_detectucode(task);
    audio_detected=(audiotype!=AUDIO_UNKNOWN);
    if(audiotype==AUDIO_NONE)
    {
        // no handler for this microcode: running another ABI's handler on
        // it would write to wrong RAM addresses, so skip the list
        loga("Unsupported audio microcode, list skipped\n");
        return;
    }
    if(audio_detected) cart.slist_type=audiotype;
    else if(cart.iszelda) cart.slist_type=1;
    else if(cart.ismario) cart.slist_type=0;

    if(cart.slist_type==2)
    {
        loga("Using Banjo rspcode\n");
        slist_banjo(task);
    }
    else if(cart.slist_type==1)
    {
        loga("Using Zelda rspcode\n");
        slist_zelda(task);
    }
    else
    {
        loga("Using Mario rspcode\n");
        slist_mario(task);
    }

    loga("Soundlist %i commands.\n",cmdcnt);
    logh("Soundlist %i commands.\n",cmdcnt);

    starttime=timer_us(&st2.timer)-starttime;
    st.us_audio+=starttime;
}

// HLE AI DMA FIFO, as ares n64/ai: two buffers, the first one playing,
// 4 bytes per DAC period. osAiSetNextBuffer fails while two are queued and
// osAiGetLength reads what is left of the playing one. The AI interrupt
// comes when a buffer is queued on an empty FIFO and when the next one
// starts. Time is os_aiclock. Twisted Edge mixes as much as osAiGetLength
// says is missing; with a length of 0 it made 23.5 kHz of 22 kHz audio and
// the sync slowed the game to fit.
static struct
{
    int   cnt;     // buffers queued, 0..2
    dword len[2];  // bytes; len[0] is what is left of the playing one
    dword addr[2]; // their addresses: read when they start playing
    qword t;       // os_aiclock up to which len[0] has played
} hai;

void slist_aiupdate(void)
{
    qword  now=os_aiclock();
    double rate=st.audiorate*4.0/os_clockrate(); // bytes per clock
    dword  b;

    if(!hai.cnt || !st.audiorate || now<=hai.t) { if(!hai.cnt) hai.t=now; return; }
    b=(dword)((double)(now-hai.t)*rate)&~3u;
    if(!b) return;
    hai.t+=(qword)(b/rate);
    while(hai.cnt && b)
    {
        if(b<hai.len[0]) { hai.len[0]-=b; break; }
        b-=hai.len[0];
        hai.len[0]=hai.len[1];
        hai.addr[0]=hai.addr[1];
        if(--hai.cnt)
        {
            slist_aiplay(hai.addr[0],hai.len[0]);
            os_event(OS_EVENT_AI);
        }
    }
    if(!hai.cnt) hai.t=now;
}

int slist_nextbuffer(dword m_addr,int bytes)
{
    int ret;
    dword len;

    if(bytes>32768) bytes=32768;
    sst.lastnextbuffer=bytes;

    len=bytes&0x3fff8;
    // HLE only (LLE queues on lle.c's AI at AI_LEN): this FIFO is the AI,
    // and a buffer plays when it reaches the head (slist_aiupdate)
    slist_aiupdate();
    if(hai.cnt>=2) return(-1); // AI_STATUS full: the buffer isn't taken
    if(len)
    {
        hai.addr[hai.cnt]=m_addr;
        hai.len[hai.cnt++]=len;
        if(hai.cnt==1)
        {
            hai.t=os_aiclock();
            slist_aiplay(m_addr,len);
            os_event(OS_EVENT_AI);
        }
    }

    ret=0;
    if(KOE) print("--nextbuf %08X/%04X (cputime=%08X)\n",m_addr,bytes,(int)st.cputime);
    loga("#osAiNextBuffer %08X size %i bytes, ret=%i\n",m_addr,bytes,ret);
    return(ret);
}

// the AI starts playing len bytes at m_addr: to the host, whoever mixed it
// (HLE sound lists, the RSP interpreter, the CPU). LLE calls this when the
// buffer reaches the head of its FIFO, as the AI DMA reads it then.
void slist_aiplay(dword m_addr,int len)
{
    static short buf[16384];
    int i;

    if(len>32768) len=32768;
    len&=~7;
    if(len<=0) return;
    for(i=0;i<len/2;i++) buf[i]=mem_read16(m_addr+i*2);
    if(st.dumpwav) sound_addwavfile("audio2.wav",buf,len,1);
    if(!st.soundenable) return;
    // LLE with HLE sound lists: interleave() played the mix already
    if(st.lleos && st2.soundlists) return;
    st2.audiorequest=1;
    sound_add(buf,len);
    st2.sync_soundadd+=len;
    st.samples+=len/4;
}

int  slist_getlength(void)
{
    int ret;

    slist_aiupdate();
    ret=hai.cnt?(int)hai.len[0]:0; // AI_LEN

    loga("#osAiGetLength, ret=%i\n",ret);
    return(ret);
}

