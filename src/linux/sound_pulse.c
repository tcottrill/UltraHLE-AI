// sound.h on Linux: the DirectSound model sound.c uses on Windows - a looping
// 256KB buffer the game's audio is written into at wpos while a play cursor
// moves through it in real time - kept as a ring here. A thread writes the
// ring to a PulseAudio stream (PipeWire on SteamOS) a period at a time; the
// blocking write paces it, so its read position is the play cursor. Like
// DirectSound it plays whatever is in the ring: sync.c keeps wpos ahead.
//
// The stream is opened the way AAE's alsa_backend.cpp InitPulseNative does:
// S16LE stereo, server latency bounded at four 1/60 s periods.

#include "../ultra.h"
#include "../bootlog.h"
#include <pthread.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#define BUFSIZE (256*1024)   // bytes, a power of 2 as on Windows

static unsigned char   ring[BUFSIZE];
static volatile int    playpos;      // play cursor (bytes), moved by the thread
static int             wpos;         // write position (bytes)
static int             bufrate=32000;
static int             period;       // bytes per write
static pa_simple      *pulse;
static pthread_t       thread;
static volatile int    running;
static int             initdone;

static void *feed(void *arg)
{
    int err;
    (void)arg;
    while(running)
    {
        int p=playpos;
        int n=period;
        if(p+n>BUFSIZE) n=BUFSIZE-p;   // period divides BUFSIZE: never split
        if(pa_simple_write(pulse,ring+p,n,&err)<0)
        {
            bootlog("sound: pa_simple_write failed: %s",pa_strerror(err));
            break;
        }
        playpos=(p+n)&(BUFSIZE-1);
    }
    return(NULL);
}

int sound_init(int rate)
{
    (void)rate;
    initdone=1;
    return(0);
}

void sound_start(int rate)
{
    pa_sample_spec ss;
    pa_buffer_attr ba;
    int frames,err=0;

    if(!initdone || pulse) return;
    bufrate=rate;

    // a period of about a 1/60 s, rounded down to a power of 2 so periods
    // tile the ring exactly
    frames=rate/60;
    period=4;
    while(period*2<=frames*4) period*=2;

    memset(&ss,0,sizeof(ss));
    ss.format  =PA_SAMPLE_S16LE;
    ss.rate    =rate;
    ss.channels=2;

    ba.maxlength=(uint32_t)-1;
    ba.tlength  =(uint32_t)(period*4);
    ba.prebuf   =(uint32_t)-1;
    ba.minreq   =(uint32_t)-1;
    ba.fragsize =(uint32_t)-1;

    pulse=pa_simple_new(NULL,"UltraHLE",PA_STREAM_PLAYBACK,NULL,"emulation",&ss,NULL,&ba,&err);
    if(!pulse)
    {
        print("sound: no PulseAudio server (%s), sound off\n",pa_strerror(err));
        return;
    }

    memset(ring,0,sizeof(ring));
    playpos=0;
    wpos=BUFSIZE/2;
    running=1;
    if(pthread_create(&thread,NULL,feed,NULL))
    {
        running=0;
        pa_simple_free(pulse);
        pulse=NULL;
        return;
    }
    bootlog("sound: PulseAudio stream %i Hz, period %i bytes",rate,period);
}

void sound_stop(void)
{
    if(!pulse) return;
    running=0;
    pthread_join(thread,NULL);
    pa_simple_flush(pulse,NULL);
    pa_simple_free(pulse);
    pulse=NULL;
}

// bytes between the play cursor and wpos, as sound.c computes it
int sound_buffered(void)
{
    int a;
    if(!pulse) return(0);
    a=wpos-playpos;
    if(a<-BUFSIZE/2) a+=BUFSIZE;
    if(a> BUFSIZE/2) a=BUFSIZE-a;
    return(a);
}

int sound_position(int *bufsizeptr)
{
    if(!pulse) return(0);
    if(bufsizeptr) *bufsizeptr=BUFSIZE;
    return(playpos);
}

// add samples at wpos (no checking for space, call sound_buffered first)
int sound_add(short *data0,int bytes)
{
    unsigned char *data=(unsigned char *)data0;
    int n;
    if(!pulse) return(-2);
    if(bytes<=0 || bytes>65535) return(-1);
    n=BUFSIZE-wpos;
    if(n>bytes) n=bytes;
    memcpy(ring+wpos,data,n);
    if(bytes>n) memcpy(ring,data+n,bytes-n);
    wpos=(wpos+bytes)&(BUFSIZE-1);
    return(0);
}

// wpos to target bytes past the play cursor, the gap silent
void sound_resync(int target)
{
    int p,n;
    if(!pulse) return;
    p=playpos;
    wpos=(p+target)&(BUFSIZE-1);
    n=BUFSIZE-p;
    if(n>target) n=target;
    memset(ring+p,0,n);
    if(target>n) memset(ring,0,target-n);
}

/****************************************************************************
** WAV dumps (as sound.c)
*/

static unsigned char wavheaderstereo[]={
0x52,0x49,0x46,0x46,0x32,0x00,0x00,0x00,
0x57,0x41,0x56,0x45,0x66,0x6D,0x74,0x20,
0x12,0x00,0x00,0x00,0x01,0x00,0x02,0x00,
0x22,0x56,0x00,0x00,0x88,0x58,0x01,0x00,
0x04,0x00,0x10,0x00,0x00,0x00,0x64,0x61,
0x74,0x61,0x00,0x00,0x00,0x00,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0};

static unsigned char wavheadermono[]={
0x52,0x49,0x46,0x46,0x32,0x00,0x00,0x00,
0x57,0x41,0x56,0x45,0x66,0x6D,0x74,0x20,
0x12,0x00,0x00,0x00,0x01,0x00,0x01,0x00,
0x22,0x56,0x00,0x00,0x2b,0xec,0x00,0x00,
0x04,0x00,0x10,0x00,0x00,0x00,0x64,0x61,
0x74,0x61,0x00,0x00,0x00,0x00,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0};

static void wav_fixheader(FILE *f1,unsigned char *header)
{
    int size,size1,size2,a;

    fseek(f1,0,SEEK_END);
    size=ftell(f1);
    size1=size-0x8;
    size2=size-0x8-0x2a;

    fseek(f1,0,SEEK_SET);
    fwrite(header,0x40,1,f1);

    fseek(f1,0x2a,SEEK_SET);
    fwrite(&size2,1,4,f1);

    fseek(f1,0x04,SEEK_SET);
    fwrite(&size1,1,4,f1);

    fseek(f1,0x18,SEEK_SET);
    fwrite(&bufrate,1,4,f1);

    a=bufrate*2;
    fseek(f1,0x1c,SEEK_SET);
    fwrite(&a,1,4,f1);

    fseek(f1,0,SEEK_END);
}

void sound_addwavfile(char *file,short *data,int cnt,int stereo)
{
    FILE *f1;
    f1=fopen(file,"r+b");
    if(!f1)
    {
        f1=fopen(file,"wb");
        if(!f1) return;
        fwrite(wavheaderstereo,1,0x40,f1);
    }
    fseek(f1,0,SEEK_END);
    fwrite(data,1,cnt,f1);
    if(stereo) wav_fixheader(f1,wavheaderstereo);
    else       wav_fixheader(f1,wavheadermono);
    fclose(f1);
}

void sound_debugsavebuffer(char *file)
{
    FILE *f1=fopen(file,"wb");
    if(!f1) return;
    fwrite(ring,1,BUFSIZE,f1);
    fclose(f1);
    print("sound: debugsavebuffer %s\n",file);
}
