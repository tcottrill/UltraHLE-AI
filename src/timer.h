#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int perf_zero[2];
} Timer;

void timer_reset(Timer *t);
int  timer_us(Timer *t);
int  timer_ms(Timer *t);
int  timer_usreset(Timer *t);
int  timer_msreset(Timer *t);

// time buckets for the LLE performance report (lle.c, every 600 VI). They
// nest: RSP runs inside CPU bursts, RDP drawing inside the RSP, presents
// inside either.
enum { PROF_BURST, PROF_RSP, PROF_RDP, PROF_PRESENT, PROF_AUDIO, PROF_WAIT, PROF_N };
void prof_begin(int i);
void prof_end(int i);
void prof_report(int retraces);
void prof_sample(int on); // sampling profiler: functions listed with each report

// LLE instruction classes (with `sample 1`): cpuc.c tags the instruction
// running, the sampler thread reads the tag with each sample, and the report
// gives each class's share of instructions and of host time.
enum { OPC_OUTSIDE, OPC_FETCH, OPC_PIPE, OPC_ALU32, OPC_ALU64, OPC_MUL32, OPC_MUL64,
       OPC_BRANCH, OPC_LOAD, OPC_LOAD64, OPC_STORE, OPC_STORE64, OPC_FPULS,
       OPC_FPUS, OPC_FPUD, OPC_FPUCVT, OPC_FPUCMP, OPC_FPUMOVE, OPC_COP0, OPC_OTHER, OPC_N };
extern volatile int       prof_opclass;       // the tag
extern int                prof_opon;          // tagging on (sample 1, LLE)
extern unsigned long long prof_opcount[OPC_N];

#ifdef __cplusplus
};
#endif
