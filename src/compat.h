// Portability: the MSVC names the core uses, mapped for GCC on Linux.
// Included by ultra.h; on Windows it only adds MAX_PATH and direct.h.

#pragma once

#include <stdint.h>

#ifndef MAX_PATH
#define MAX_PATH 260 // the same as windows.h's, so either may come first
#endif

#ifdef _WIN32

#include <direct.h>   // _mkdir

#define PATHSEP    '\\'   // folder separator
#define PATHSEPSTR "\\"

#else

#include <stddef.h>
#include <ctype.h>
#include <strings.h>  // strcasecmp
#include <sys/stat.h> // mkdir
#include <unistd.h>   // usleep
#include <sched.h>    // sched_yield
#include <limits.h>
#include <errno.h>

#define PATHSEP    '/'
#define PATHSEPSTR "/"

#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#define _mkdir(path) mkdir((path),0755)
#define _snprintf snprintf
#define Sleep(ms) compat_sleep(ms)
#define __int64 long long
#define GetLastError() errno

#define localtime_s(tm,t) localtime_r((t),(tm))

// Sleep(0) gives up the rest of the time slice, as on Windows
static inline void compat_sleep(unsigned ms)
{
    if(ms) usleep(ms*1000);
    else sched_yield();
}

static inline char *_strlwr(char *s)
{
    char *p;
    for(p=s;*p;p++) *p=(char)tolower((unsigned char)*p);
    return(s);
}

static inline int _memicmp(const void *a,const void *b,size_t n)
{
    const unsigned char *x=(const unsigned char *)a,*y=(const unsigned char *)b;
    for(;n;n--,x++,y++)
    {
        int d=tolower(*x)-tolower(*y);
        if(d) return(d);
    }
    return(0);
}

#endif
