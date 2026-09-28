#include <windows.h>
#include "ultra.h"

static LARGE_INTEGER  perf_freq;
static int            initdone;

void timer_reset(Timer *t)
{
    if(!initdone)
    {
        if(!QueryPerformanceFrequency(&perf_freq))
        {
            exception("This computer does not support Pentium Performance Counter\n");
        }
        initdone=1;
    }
    QueryPerformanceCounter((LARGE_INTEGER *)&t->perf_zero);
}

int  timer_us(Timer *t)
{
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    return((int)(1000000*(pc.QuadPart-((LARGE_INTEGER *)&t->perf_zero)->QuadPart)/perf_freq.QuadPart));
}

int  timer_ms(Timer *t)
{
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    return((int)(1000*(pc.QuadPart-((LARGE_INTEGER *)&t->perf_zero)->QuadPart)/perf_freq.QuadPart));
}

int  timer_usreset(Timer *t)
{
    LARGE_INTEGER pc;
    int r;
    QueryPerformanceCounter(&pc);
    r=(int)(1000000*(pc.QuadPart-((LARGE_INTEGER *)&t->perf_zero)->QuadPart)/perf_freq.QuadPart);
    *(LARGE_INTEGER *)&t->perf_zero=pc;
    return(r);
}

int  timer_msreset(Timer *t)
{
    LARGE_INTEGER pc;
    int r;
    QueryPerformanceCounter(&pc);
    r=(int)(1000*(pc.QuadPart-((LARGE_INTEGER *)&t->perf_zero)->QuadPart)/perf_freq.QuadPart);
    *(LARGE_INTEGER *)&t->perf_zero=pc;
    return(r);
}

/****************************************************************************
** Performance buckets (timer.h). A bucket entered again before it ends
** (nested) counts only the outer span.
*/

static LONGLONG prof_start[PROF_N],prof_sum[PROF_N],prof_wall;
static int      prof_depth[PROF_N];

void prof_begin(int i)
{
    LARGE_INTEGER pc;
    if(prof_depth[i]++) return;
    QueryPerformanceCounter(&pc);
    prof_start[i]=pc.QuadPart;
}

void prof_end(int i)
{
    LARGE_INTEGER pc;
    if(prof_depth[i]<=0 || --prof_depth[i]) return;
    QueryPerformanceCounter(&pc);
    prof_sum[i]+=pc.QuadPart-prof_start[i];
}

// ms per second of wall time for each bucket since the last report, and the
// frame rate the emulated retraces came at
void prof_report(int retraces)
{
    static const char *name[PROF_N]={"burst","rsp","rdp","present","audio","wait"};
    LARGE_INTEGER pc;
    double wall;
    int    i;

    if(!initdone) { Timer t; timer_reset(&t); }
    QueryPerformanceCounter(&pc);
    if(!prof_wall) { prof_wall=pc.QuadPart; return; }
    wall=(double)(pc.QuadPart-prof_wall)/perf_freq.QuadPart;
    prof_wall=pc.QuadPart;
    if(wall<=0) return;
    print("perf: %.1f s, %.1f VI/s;",wall,retraces/wall);
    for(i=0;i<PROF_N;i++)
    {
        print(" %s %.0f",name[i],1000.0*prof_sum[i]/perf_freq.QuadPart/wall);
        prof_sum[i]=0;
    }
    print(" ms/s (rsp includes rdp; burst includes rsp, rdp, audio)\n");
}



