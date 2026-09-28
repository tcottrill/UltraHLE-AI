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

#ifdef __cplusplus
};
#endif
