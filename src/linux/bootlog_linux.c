// bootlog.h on Linux: startup.log in the program's home folder (ui_linux.c,
// $ULTRAHLE_HOME or the exe's folder), each line appended and the file
// closed so the trace survives a crash. Also echoed to stderr.

#include "../ultra.h"
#include "../bootlog.h"
#include "../ui.h"
#include <pthread.h>
#include <time.h>

static char            logdir[MAX_PATH];
static pthread_mutex_t loglock=PTHREAD_MUTEX_INITIALIZER;
static struct timespec start;

char *bootlog_path(char *out,const char *name)
{
    sprintf(out,"%s%s",logdir,name);
    return(out);
}

void bootlog(const char *fmt,...)
{
    char            line[1024];
    struct timespec now;
    va_list         ap;
    FILE           *f;
    double          t;

    clock_gettime(CLOCK_MONOTONIC,&now);
    t=(now.tv_sec-start.tv_sec)+(now.tv_nsec-start.tv_nsec)/1e9;
    va_start(ap,fmt);
    vsnprintf(line,sizeof(line),fmt,ap);
    va_end(ap);

    pthread_mutex_lock(&loglock);
    f=fopen(bootlog_path((char[MAX_PATH+32]){0},"startup.log"),"at");
    if(f)
    {
        fprintf(f,"%9.3f %s\n",t,line);
        fclose(f);
    }
    fprintf(stderr,"[%.3f] %s\n",t,line);
    pthread_mutex_unlock(&loglock);
}

void bootlog_modules(const char *when) { (void)when; }

void bootlog_begin(void)
{
    char  path[MAX_PATH];
    char *p;
    FILE *f;

    clock_gettime(CLOCK_MONOTONIC,&start);
    ui_exepath(path,sizeof(path));
    p=strrchr(path,'/');
    if(p) p[1]=0;
    else strcpy(path,"./");
    strcpy(logdir,path);
    f=fopen(bootlog_path(path,"startup.log"),"wt"); // a new log each start
    if(f) fclose(f);
    else
    {
        // home folder not writable: /tmp
        strcpy(logdir,"/tmp/");
    }
    bootlog("UltraHLE starting (Linux)");
}

void ui_linux_emuready(void); // ui_linux.cpp

void bootlog_ready(void)
{
    bootlog("startup done");
    ui_linux_emuready(); // main_thread is about to wait for commands
}

void bootlog_crash(struct _EXCEPTION_POINTERS *ep) { (void)ep; }
