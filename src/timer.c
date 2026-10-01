#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#include "ultra.h"
#include "rsp_cxd4.h"

#pragma comment(lib,"dbghelp.lib")
#pragma comment(lib,"winmm.lib")

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

/****************************************************************************
** Sampling profiler (command 'sample 1'): a thread reads the emulation
** thread's instruction pointer about 1000 times a second, and every report
** lists the functions the samples fell in (inlined ones by their own name;
** needs the .pdb). The thread sampled is the one that calls prof_report.
*/

#define SAMPLE_MAX   8192
#define SAMPLE_NAMES 512
#define SAMPLE_HASH  4096
#define SAMPLE_TOP   24

typedef struct
{
    char name[96];
    int  count;
} SampleName;

static DWORD64       sample_ip[SAMPLE_MAX];
static unsigned char sample_cls[SAMPLE_MAX]; // prof_opclass at each sample
static volatile LONG sample_n,sample_on,sample_hold;
static HANDLE        sample_target,sample_thread;
static SampleName    sample_name[SAMPLE_NAMES]; // [0] collects what didn't fit
static int           sample_names;
static DWORD64       sample_haship[SAMPLE_HASH];
static short         sample_hashname[SAMPLE_HASH];

static DWORD WINAPI sample_main(LPVOID arg)
{
    CONTEXT c;
    timeBeginPeriod(1);
    while(sample_on)
    {
        Sleep(1);
        if(sample_hold || sample_n>=SAMPLE_MAX) continue;
        if(SuspendThread(sample_target)==(DWORD)-1) continue;
        c.ContextFlags=CONTEXT_CONTROL;
        if(GetThreadContext(sample_target,&c))
        {
#ifdef _WIN64
            sample_ip[sample_n]=c.Rip;
#else
            sample_ip[sample_n]=c.Eip;
#endif
            sample_cls[sample_n++]=(unsigned char)prof_opclass;
        }
        ResumeThread(sample_target);
    }
    timeEndPeriod(1);
    return(0);
}

volatile int       prof_opclass;
int                prof_opon;
unsigned long long prof_opcount[OPC_N];

void prof_sample(int on)
{
    sample_on=on;
    prof_opon=on;
    prof_opclass=OPC_OUTSIDE;
    memset(prof_opcount,0,sizeof(prof_opcount));
    if(!on && sample_thread)
    {
        WaitForSingleObject(sample_thread,1000);
        CloseHandle(sample_thread);
        CloseHandle(sample_target);
        sample_thread=sample_target=NULL;
        sample_n=0;
    }
}

// the name's index in sample_name for an instruction pointer
static int sample_resolve(DWORD64 ip)
{
    HANDLE             proc=GetCurrentProcess();
    char               buf[sizeof(SYMBOL_INFO)+256],text[96];
    SYMBOL_INFO       *si=(SYMBOL_INFO *)buf;
    IMAGEHLP_MODULE64  mod;
    DWORD64            disp=0;
    DWORD              ctx=0,frame=0;
    int                h=(int)((ip*0x9E3779B97F4A7C15ull)>>52)&(SAMPLE_HASH-1);
    int                i,ok;

    if(sample_haship[h]==ip) return(sample_hashname[h]);

    memset(buf,0,sizeof(buf));
    si->SizeOfStruct=sizeof(SYMBOL_INFO);
    si->MaxNameLen=255;
    memset(&mod,0,sizeof(mod));
    mod.SizeOfStruct=sizeof(mod);
    if(SymAddrIncludeInlineTrace(proc,ip) &&
       SymQueryInlineTrace(proc,ip,0,ip,ip,&ctx,&frame))
    {
        ok=SymFromInlineContext(proc,ip,ctx,&disp,si);
    }
    else ok=SymFromAddr(proc,ip,&disp,si);
    if(!SymGetModuleInfo64(proc,ip,&mod)) strcpy(mod.ModuleName,"?");
    _snprintf(text,sizeof(text)-1,"%s!%s",mod.ModuleName,ok?si->Name:"?");
    text[sizeof(text)-1]=0;

    for(i=1;i<sample_names;i++)
    {
        if(!strcmp(sample_name[i].name,text)) break;
    }
    if(i>=sample_names)
    {
        if(sample_names<SAMPLE_NAMES)
        {
            i=sample_names++;
            strcpy(sample_name[i].name,text);
            sample_name[i].count=0;
        }
        else i=0;
    }
    sample_haship[h]=ip;
    sample_hashname[h]=(short)i;
    return(i);
}

// each instruction class: its share of the host's time (all samples, waits
// included), of the instructions run, and the host time for one instruction
static void opclass_report(int n,double wall)
{
    static const char *name[OPC_N]={"outside cpu","fetch","pipeline","alu32","alu64",
        "mul/div32","mul/div64","branch","load","load64","store","store64","fpu ld/st",
        "fpu.s","fpu.d","fpu cvt","fpu cmp","fpu move","cop0/cache","other"};
    unsigned long long total=0;
    int cnt[OPC_N],i;
    memset(cnt,0,sizeof(cnt));
    for(i=0;i<n;i++) if(sample_cls[i]<OPC_N) cnt[sample_cls[i]]++;
    for(i=OPC_ALU32;i<OPC_N;i++) total+=prof_opcount[i];
    for(i=0;i<OPC_N;i++)
    {
        double t=n>0?(double)cnt[i]/n:0.0;
        if(!cnt[i] && !prof_opcount[i]) continue;
        if(i<OPC_ALU32)
            print("opclass: %-11s %5.1f%% of time\n",name[i],100.0*t);
        else
            print("opclass: %-11s %5.1f%% of time %5.1f%% of instructions %6.1f ns each\n",
                  name[i],100.0*t,total?100.0*prof_opcount[i]/total:0.0,
                  prof_opcount[i]?t*wall*1e9/prof_opcount[i]:0.0);
        prof_opcount[i]=0;
    }
}

static void sample_report(double wall)
{
    static int syminit;
    int i,j,n,best;

    if(!sample_on) return;
    if(!sample_thread)
    { // first report: this is the thread to sample
        if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),
                            &sample_target,0,FALSE,DUPLICATE_SAME_ACCESS)) return;
        sample_n=0;
        sample_thread=CreateThread(NULL,0,sample_main,NULL,0,NULL);
        return;
    }
    sample_hold=1;
    if(!syminit)
    {
        SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(),NULL,TRUE);
        strcpy(sample_name[0].name,"(other)");
        sample_names=1;
        syminit=1;
    }
    n=sample_n;
    if(n>SAMPLE_MAX) n=SAMPLE_MAX;
    for(i=0;i<sample_names;i++) sample_name[i].count=0;
    for(i=0;i<n;i++) sample_name[sample_resolve(sample_ip[i])].count++;
    print("sample: %i samples\n",n);
    opclass_report(n,wall);
    for(j=0;j<SAMPLE_TOP && n>0;j++)
    {
        best=0;
        for(i=1;i<sample_names;i++)
        {
            if(sample_name[i].count>sample_name[best].count) best=i;
        }
        if(sample_name[best].count<=0) break;
        print("sample: %5.1f%% %s\n",100.0*sample_name[best].count/n,sample_name[best].name);
        sample_name[best].count=0;
    }
    sample_n=0;
    sample_hold=0;
}

// ms per second of wall time for each bucket since the last report, and the
// frame rate the emulated retraces came at
void prof_report(int retraces)
{
    static const char *name[PROF_N]={"burst","rsp","rdp","present","audio","wait"};
    LARGE_INTEGER pc;
    double wall,ms[PROF_N];
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
        ms[i]=1000.0*prof_sum[i]/perf_freq.QuadPart/wall;
        print(" %s %.0f",name[i],ms[i]);
        prof_sum[i]=0;
    }
    print(" ms/s (rsp includes rdp; burst includes rsp, rdp, audio)\n");
    { // instructions a second, and the host's time for one
        static qword lastcpu,lastrsp;
        qword  cpu=st.cputime,rsp=rsp_cxd4_ran();
        double c=(double)(cpu-lastcpu)/wall,r=(double)(rsp-lastrsp)/wall;
        double cms=ms[PROF_BURST]-ms[PROF_RSP]-ms[PROF_AUDIO]-ms[PROF_PRESENT];
        double rms=ms[PROF_RSP]-ms[PROF_RDP];
        print("perf: cpu %.1f M instructions/s at %.1f ns, rsp %.1f M at %.1f ns\n",
            c/1e6,c>0?cms*1e6/c:0.0,r/1e6,r>0?rms*1e6/r:0.0);
        lastcpu=cpu;
        lastrsp=rsp;
    }
    sample_report(wall);
}



