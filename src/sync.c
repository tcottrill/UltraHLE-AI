// This file is a mess, look away :)
// real pain this audio syncronization! :)

#include <windows.h>
#include "ultra.h"

/* Sync routines:
**
** sync_checkretrace - called from cpu.c periodically, checks if retrace should be done
** sync_retrace      - a retrace occurred (called by the above)
** sync_gfxframedone - called by RDP.C when a new gfx frame is complete
** sync_audio        - checks and adjusts sound related syncing info (from sync_retrace)
**
*/

static Timer retracetimer;
static Timer longtimer;

#define SOUNDTARGET      20000   // try to keep 20Kbytes of sound in buffer
#define SYNC_HELDMAX     30      // HLE: retraces held back for a game that doesn't take the message

void sync_init(void)
{
    timer_reset(&retracetimer);
    timer_reset(&longtimer);
}

// These sync_* routines called from the main thread

void sync_audio(void)
{
    int a,b,bufsize;
    static int lastsndpos;
    static int playrate;

    if(st2.audiorequest)
    {
        // slist.c has found audio and wants to enable
        // directsound playback
        if(!st2.audioon)
        {
            if(!st.audiorate) st.audiorate=32000;
            print("Sound initialized: %ihz\n",st.audiorate);
            sound_init(st.audiorate);
            sound_start(st.audiorate);
            playrate=st.audiorate;
            st2.audioon=1;
            remove("audio.wav");
        }
        st2.audiorequest=0;
    }

    // the game changed the DAC rate after playback started (Bassmasters
    // 2000 queues a 4000 Hz buffer at boot, then plays at 16385 Hz)
    if(st2.audioon && st.audiorate && st.audiorate!=playrate)
    {
        print("Sound rate changed: %ihz\n",st.audiorate);
        sound_stop();
        sound_start(st.audiorate);
        playrate=st.audiorate;
    }

    if(st2.audioon)
    {
        int buffered;
        int target;

        target=SOUNDTARGET;
        /*
        if(st.audiorate<23000) target=SOUNDTARGET/2;
        else target=SOUNDTARGET;
        */

        a=sound_position(&bufsize);
        b=(a-lastsndpos); if(b<0) b+=bufsize;
        if(b<0 || b>bufsize) b=0;
        lastsndpos=a;
        st2.sync_soundused+=b;

        // we have audio, syncronize to it
        buffered=sound_buffered(); // how many bytes in buffer
        st2.audiobuffered=buffered-target;

        st2.audiobufferedsum+=st2.audiobuffered;
        st2.audiobufferedcnt++;

        {
            if(buffered>target*4)
            { // buffer overflowing, resync
                sound_resync(target);
                st2.audiostatus=9;
                st2.audioresync++;
                st2.audiobufferedcnt+=0x10000;
            }
            else if(buffered<4000)
            { // ran out of buffer! (4K safety)
                sound_resync(target*3);
                st2.audiostatus=-9;
                st2.audioresync++;
                st2.audiobufferedcnt+=0x10000;
            }
            else if(buffered>target*6/4)
            { // too much data, slow down
                st2.audiostatus=2;
                st2.audioresync++;
            }
            else if(buffered<target*2/4)
            { // too little data, more speed
                st2.audiostatus=-2;
            }
            else if(buffered>target*5/4)
            { // too much data, slow down
                st2.audiostatus=1;
            }
            else if(buffered<target*3/4)
            { // too little data, more speed
                st2.audiostatus=-1;
            }
            else
            { // about the correct amount, reset status when we cross target
                if(buffered<target && st2.audiostatus>0) st2.audiostatus=0;
                if(buffered>target && st2.audiostatus<0) st2.audiostatus=0;
            }
        }

        if(st.timing) a=51;
        else a=61;
             if(st2.audiostatus>1)  a-=8;
        else if(st2.audiostatus<-1) a+=16;
        else if(st2.audiostatus>0)  a-=3;
        else if(st2.audiostatus<0)  a+=8;
        st2.frameus=1000000/a;
    }

    if(st.retraces%60==0)
    {
        if(st2.audioon && (st.retraces<=600 || st.retraces%300==0))
        { // every second for the first 10 s, then every 5 s: what the audio path is doing
            print("sound: retrace %i added %i used %i buffered %+i status %i resyncs %i peak %i\n",
                st.retraces,st2.sync_soundadd,st2.sync_soundused,st2.audiobuffered,
                st2.audiostatus,st2.audioresync,st2.audiopeak);
            st2.audiopeak=0;
        }
        st2.sync_soundused=0;
        st2.sync_soundadd=0;
    }
}

/****************************************************************************
** The clock of osGetTime and osGetCount (HLE)
**
** It follows the VI: a frame of clocks for every retrace that has come due,
** delivered or not (a game that doesn't take its retrace messages has them
** held back and dropped, sync_checkretrace), and within the frame the part
** of it the CPU has run. It never goes back and it never stands still while
** the CPU runs.
**
** Star Wars Episode I Racer takes a lower osGetCount for a wrap and adds
** 91 s, and it takes its frame time from two readings and divides by it:
** with a clock that stopped at the frame's end until the next delivered
** retrace, half a second of held retraces gave it dt = 0, 1/0 in its
** physics and NaN for every position (HUD over a black world).
**
** st.clockahead (saved in states) counts the retraces that came due and
** aren't in st.retraces. Within the frame the CPU's clocks are scaled by
** what it ran between the last retraces, so the frame's end is reached
** about when the next one is due; past nine tenths the rest is approached
** without being reached.
*/

static qword clock_tickcpu;   // cputime when the last retrace came due
static qword clock_period[2]; // CPU clocks between the last ones
static int   clock_set;

// a retrace has come due
static void clock_tick(void)
{
    if(st.lleos) return;
    if(clock_set && st.cputime>clock_tickcpu)
    {
        clock_period[1]=clock_period[0];
        clock_period[0]=st.cputime-clock_tickcpu;
    }
    clock_tickcpu=st.cputime;
    clock_set=1;
    st.clockahead++;
}

// ... and was delivered: st.retraces has it now
static void clock_delivered(void)
{
    if(st.clockahead>0) st.clockahead--;
}

// a state was loaded (st.cputime, st.retraces and st.clockahead are its own)
void sync_clockload(void)
{
    clock_set=0;
}

qword sync_clock(void)
{
    qword  frame=os_clockrate()/60;
    qword  period;
    double x;

    if(!clock_set || st.cputime<clock_tickcpu || st.cputime-clock_tickcpu>frame*120)
    { // first reading, or a state was loaded: a frame on from there, past
      // any time the game has read
        clock_tickcpu=st.cputime;
        clock_period[0]=clock_period[1]=0;
        clock_set=1;
        st.clockahead++;
    }

    // the longer of the last two periods: retraces that catch up come in
    // pairs a few thousand clocks apart
    period=clock_period[0]>clock_period[1]?clock_period[0]:clock_period[1];
    if(period<frame/2) period=frame/2;

    x=(double)(st.cputime-clock_tickcpu)/(double)period;
    if(x>0.9) x=0.9+0.1*(1.0-1.0/(1.0+(x-0.9)*10.0));

    return((qword)(st.retraces+st.clockahead)*frame+(qword)(x*(double)frame));
}

int sync_swappending; // osViSwapBuffer called, not shown by a retrace yet

void sync_retrace(void)
{
    int a;

    st2.lastretracecputime=st.cputime;

    // report framebuffer swap to os routines
    st.fb_current=st.fb_next;
    sync_swappending=0;

    st2.retracetime=st.cputime;
    st.retraces++;
    clock_delivered();
#ifdef DK64_DIAGNOSTICS
    if(getenv("DK64_TRACE") && mem.ramsize==8*1024*1024)
    {
        static dword lastmap=~0u,lastactive=~0u,lastidx=~0u;
        dword map=mem_read32(0x8076A0A8),active=mem_read8(0x807444EC),idx=mem_read32(0x807F5D14);
        if(map!=lastmap || active!=lastactive || idx!=lastidx || st.retraces%300==0)
        {
            qword start=((qword)mem_read32(0x807F5CE0)<<32)|mem_read32(0x807F5CE4);
            print("dktrace: vi %i map %X next %X active %u idx %u frame %u elapsed %.3f dp %X swap %X fb %X/%X sched %u lastswap %u\n",
                  st.retraces,map,mem_read32(0x807444E4),active,idx,mem_read32(0x8076A068),
                  start?(double)(sync_clock()-start)/46875000.0:0.0,RDP[3],mem_read32(0x807F04E0),
                  st.fb_current,st.fb_next,mem_read32(0x80767CC4),mem_read32(0x80746820));
            lastmap=map; lastactive=active; lastidx=idx;
        }
        if(getenv("DK64_MOVE"))
        { // movement: the player's position and control state every retrace
            dword pl=mem_read32(0x807FBB4C);
            if(pl>=0x80000000 && pl<0x80800000)
            {
                dword xi=mem_read32(pl+0x7C),yi=mem_read32(pl+0x80),zi=mem_read32(pl+0x84);
                float x,y,z; memcpy(&x,&xi,4); memcpy(&y,&yi,4); memcpy(&z,&zi,4);
                dword bi=mem_read32(pl+0xB8); float b; memcpy(&b,&bi,4);
                // Actor: unkB8 speed, unkEE move angle, unkF2..unkF8 s16, unkFA collision-limited speed, unkFC collision on
                print("dkmove: vi %i frame %u pos %.2f %.2f %.2f state %u/%u pad %08X lag %u gate %02X speed %.3f EE %04X F2 %04X F4 %04X F6 %04X F8 %04X FA %i FC %u yrot %04X\n",
                      st.retraces,mem_read32(0x8076A068),x,y,z,mem_read8(pl+0x154),mem_read8(pl+0x155),
                      mem_read32(0x807ECD10),mem_read32(0x80744478),mem_read8(0x8076A0B1),b,
                      mem_read16(pl+0xEE),mem_read16(pl+0xF2),mem_read16(pl+0xF4),mem_read16(pl+0xF6),mem_read16(pl+0xF8),
                      (short)mem_read16(pl+0xFA),mem_read8(pl+0xFC),mem_read16(pl+0xE6));
            }
        }
        if(getenv("DK64_STOP_VI") && st.retraces>=atoi(getenv("DK64_STOP_VI"))) st.breakout=1;
    }
#endif
    if(!st.lleos) slist_aiupdate(); // HLE AI FIFO: interrupt when a buffer ends

    rdp_snapshotpoll(); // F10/F12 on a screen the game doesn't redraw
    rdp_retrace();

//    logh("--retrace--\n");
    // HLE: osViSetEvent's retraceCount (LLE games count in their own VI manager)
    if(st.lleos || st.viretracecount<=1 || ++st.viretracewait>=st.viretracecount)
    {
        st.viretracewait=0;
        // a full queue loses the message (sync_checkretrace, SYNC_HELDMAX)
        if(st.lleos || os_eventqueuefree(OS_EVENT_RETRACE)) os_event(OS_EVENT_RETRACE);
    }
    st2.pendingretraces--;

    if(st.dumpinfo)
    {
//        print("--retrace-- audiobuf %8i (%3i, %.0fhz)\n",st2.audiobuffered,st2.audiostatus,1000000.0/st2.frameus);
        if(0)
        {
            float fr;
            fr=(60.0f/1000.0f)*timer_ms(&longtimer);
            logi("--retrace--(wait %2ims, time %7.2f, %2i pending)--",a,fr,st2.pendingretraces);
        }
        if(0)
        {
            static Timer tim;
            static int cnt;
            int    ms;
            if(!cnt) timer_reset(&tim);

            if(cnt>50)
            {
                ms=timer_ms(&tim);
                if(!ms) ms=1;

                print("ret/sec: %.2f (frameus=%.2f)\n",
                    1000.0*cnt/ms,
                    1000000.0/st2.frameus);

                timer_reset(&tim);
                cnt=0;
            }

            cnt++;
        }
    }

    a_cleardeadgroups();

    inifile_patches(-1);

    pad_frame();

    sync_audio();
}

// called by RDP when a frame sync is done
void sync_gfxframedone(void)
{
    st2.ops+=(int)(st.cputime-st.synctime);
    st.synctime=st.cputime;

    pad_drawframe();

    if(1)
    {
        logi("[sound: added %6i, used %6i, sync %+6i, status %2i]--\n",
            st2.sync_soundadd,
            st2.sync_soundused,
            st2.audiobuffered,
            st2.audiostatus);
    }

    st.frames++;
}

// called from cpu.c
void sync_checkretrace(void)
{
    static Timer retracetimer;
    int          us,due=0;

    us=timer_usreset(&retracetimer);
    if(us>1000000 || us<0) us=0;
    st2.usleft-=us;

    // countperop (LLE): a frame's work is done, so wait here for the
    // retrace (at most one frame) instead of running ahead of the VI
    if(st2.pendingretraces<=0 && st2.usleft>0 && lle_framedone())
    {
        // measured from one start: timer_usreset drops the part below 1us
        // on every call, and short Sleep(0) passes never added up
        Timer wait;
        int   left;
        timer_reset(&wait);
        prof_begin(PROF_WAIT);
        while((left=st2.usleft-timer_us(&wait))>0) Sleep(left>2000?1:0);
        prof_end(PROF_WAIT);
        us=timer_usreset(&retracetimer);
        if(us>1000000 || us<0) us=0;
        st2.usleft-=us;
    }

    if(st2.usleft<0)
    {
        st2.pendingretraces++;
        clock_tick();
        due=1;

        if(!st2.frameus) st2.frameus=1000000/60;

        st2.usleft+=st2.frameus;
    }

    if(st2.pendingretraces>0)
    {
        static int cnt;
        static int held; // HLE: retraces come due with the last message not taken
        // HLE waits until the game has taken the last retrace message;
        // in LLE mode the VI interrupt is delivered regardless, except that
        // countperop games get their whole frame of CPU time first. Behind
        // the host clock, the VI came early with part of the frame unrun,
        // and their AI buffers (timed in CPU time) made that much less
        // audio: Rogue Squadron's sound cut out while the host was idle a
        // third of the time.
        if(st.lleos ? (cart.countperop<=0 || lle_framedone())
                    : os_eventqueuefree(OS_EVENT_RETRACE))
        {
            cnt++;
            held=0;
        }
        else
        {
            cnt=0;
            // the VI still reaches vblank: a swapped framebuffer is shown
            // even when the retrace message is held back. F-Zero X swaps
            // and polls osViGetCurrentFramebuffer from the thread that
            // would empty the retrace queue.
            // At a retrace, not at once: Star Wars Episode I Racer draws its
            // first two frames into one buffer and polls until that buffer
            // is off screen, which never ended with the swap already shown.
            if(due && sync_swappending)
            {
                st.fb_current=st.fb_next;
                sync_swappending=0;
            }
            // A game that leaves the message in the queue for good (Racer
            // only polls the framebuffer) has its retraces all the same:
            // libultra's VI manager sends without blocking and the message
            // is lost. Held back forever, sound never started.
            if(due && !st.lleos && ++held>SYNC_HELDMAX)
            {
                held=SYNC_HELDMAX;
                st2.pendingretraces=1;
                cnt=3;
            }
        }
        if(cnt>2)
        {
            cnt=0;

            if(st.nicebreak)
            {
                hw_check();
                st.breakout=1;
                st.nicebreak=0;
                return;
            }

            sync_retrace();

            if(st2.pendingretraces>9)
            {
                warning("retraces pending counter reset");
                st2.pendingretraces=0;
            }
        }
    }
}

