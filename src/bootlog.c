// Startup trace, see bootlog.h.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include "bootlog.h"
#pragma comment(lib,"version.lib")
#pragma comment(lib,"dbghelp.lib")
#pragma comment(lib,"advapi32.lib")

static char             logdir[MAX_PATH];
static CRITICAL_SECTION loglock;
static ULONGLONG        starttick;
static volatile LONG    firstchance=20;   // first-chance exceptions still to log

char *bootlog_path(char *out,const char *name)
{
    strcpy(out,logdir);
    strcat(out,name);
    return(out);
}

void bootlog(const char *fmt,...)
{
    char    path[MAX_PATH];
    FILE   *f;
    va_list ap;

    if(!*logdir) return;
    EnterCriticalSection(&loglock);
    f=fopen(bootlog_path(path,"startup.log"),"a");
    if(f)
    {
        fprintf(f,"%7.3f [%5lu] ",(GetTickCount64()-starttick)/1000.0,GetCurrentThreadId());
        va_start(ap,fmt);
        vfprintf(f,fmt,ap);
        va_end(ap);
        fputc('\n',f);
        fclose(f);
    }
    LeaveCriticalSection(&loglock);
}

// "a.b.c.d" file version of a module, or "" when it has none
static void fileversion(const wchar_t *file,char *out)
{
    DWORD  dummy,size=GetFileVersionInfoSizeW(file,&dummy);
    void  *data;
    VS_FIXEDFILEINFO *fi;
    UINT   len;

    *out=0;
    if(!size || !(data=malloc(size))) return;
    if(GetFileVersionInfoW(file,0,size,data) &&
       VerQueryValueW(data,L"\\",(void**)&fi,&len) && fi)
        sprintf(out,"%u.%u.%u.%u",HIWORD(fi->dwFileVersionMS),LOWORD(fi->dwFileVersionMS),
                HIWORD(fi->dwFileVersionLS),LOWORD(fi->dwFileVersionLS));
    free(data);
}

// module containing address, for exception lines
static void modulename(void *addr,char *out,int size,uintptr_t *offset)
{
    HMODULE m=NULL;
    *out=0;
    *offset=(uintptr_t)addr;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)addr,&m) && m)
    {
        char *p;
        GetModuleFileNameA(m,out,size);
        p=strrchr(out,'\\');
        if(p) memmove(out,p+1,strlen(p));
        *offset=(uintptr_t)addr-(uintptr_t)m;
    }
    else strcpy(out,"?");
}

void bootlog_modules(const char *when)
{
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    MODULEENTRY32W me;
    char ver[64];
    int  n=0;

    bootlog("modules loaded (%s):",when);
    if(snap==INVALID_HANDLE_VALUE)
    {
        bootlog("  CreateToolhelp32Snapshot failed, error %lu",GetLastError());
        return;
    }
    me.dwSize=sizeof(me);
    if(Module32FirstW(snap,&me)) do
    {
        fileversion(me.szExePath,ver);
        bootlog("  %p %8lX %-16s %ls",me.modBaseAddr,me.modBaseSize,ver,me.szExePath);
        n++;
    } while(Module32NextW(snap,&me));
    CloseHandle(snap);
    bootlog("  (%d modules)",n);
}

// Every DLL in the exe's import table, and whether/where it loaded.
static void logimports(void)
{
    BYTE *base=(BYTE*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt=(IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir=&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;

    bootlog("imported DLLs:");
    if(!dir->VirtualAddress) return;
    for(imp=(IMAGE_IMPORT_DESCRIPTOR*)(base+dir->VirtualAddress);imp->Name;imp++)
    {
        const char *name=(const char*)(base+imp->Name);
        HMODULE m=GetModuleHandleA(name);
        wchar_t path[MAX_PATH];
        char    ver[64];
        if(!m)
        {
            bootlog("  %-14s NOT LOADED",name);
            continue;
        }
        GetModuleFileNameW(m,path,MAX_PATH);
        fileversion(path,ver);
        bootlog("  %-14s %-16s %ls",name,ver,path);
    }
}

// Present on the system or not (not loaded): DLLs people ask about.
static void logoptional(const char *name,const char *why)
{
    char path[MAX_PATH];
    if(SearchPathA(NULL,name,NULL,MAX_PATH,path,NULL)) bootlog("  %-14s present %s (%s)",name,path,why);
    else                                              bootlog("  %-14s missing (%s)",name,why);
}

static void logos(void)
{
    typedef LONG (WINAPI *rtlgetversion)(RTL_OSVERSIONINFOW*);
    rtlgetversion rgv=(rtlgetversion)GetProcAddress(GetModuleHandleA("ntdll.dll"),"RtlGetVersion");
    RTL_OSVERSIONINFOW v;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    int  cpu[4];
    char brand[49];
    BOOL wow=FALSE;

    memset(&v,0,sizeof(v));
    v.dwOSVersionInfoSize=sizeof(v);
    if(rgv) rgv(&v);
    IsWow64Process(GetCurrentProcess(),&wow);
    bootlog("windows %lu.%lu build %lu %ls, %d-bit exe%s",v.dwMajorVersion,v.dwMinorVersion,
            v.dwBuildNumber,v.szCSDVersion,(int)sizeof(void*)*8,wow?" under WOW64":"");

    memset(brand,0,sizeof(brand));
    __cpuid(cpu,0x80000000);
    if((unsigned)cpu[0]>=0x80000004)
    {
        __cpuid((int*)brand,0x80000002);
        __cpuid((int*)(brand+16),0x80000003);
        __cpuid((int*)(brand+32),0x80000004);
    }
    GetSystemInfo(&si);
    bootlog("cpu: %s, %lu logical processors",brand,si.dwNumberOfProcessors);
    __cpuid(cpu,1);
    bootlog("cpu features: sse2 %d sse3 %d ssse3 %d sse4.1 %d sse4.2 %d avx %d",
            (cpu[3]>>26)&1,cpu[2]&1,(cpu[2]>>9)&1,(cpu[2]>>19)&1,(cpu[2]>>20)&1,(cpu[2]>>28)&1);

    ms.dwLength=sizeof(ms);
    if(GlobalMemoryStatusEx(&ms))
        bootlog("memory: %llu MB total, %llu MB free, %llu MB free address space",
                ms.ullTotalPhys>>20,ms.ullAvailPhys>>20,ms.ullAvailVirtual>>20);
}

// Registry value under a DeviceKey ("\Registry\Machine\SYSTEM\...")
static void regstring(const char *devicekey,const char *value,char *out,DWORD size)
{
    const char *key=devicekey;
    *out=0;
    if(!_strnicmp(key,"\\Registry\\Machine\\",18)) key+=18;
    // multi-string values (OpenGLDriverName) give their first string
    if(RegGetValueA(HKEY_LOCAL_MACHINE,key,value,RRF_RT_REG_SZ|RRF_RT_REG_MULTI_SZ,
                    NULL,out,&size)!=ERROR_SUCCESS)
        *out=0;
}

static void logdisplays(void)
{
    DISPLAY_DEVICEA dd;
    DEVMODEA dm;
    DWORD i;
    HDC   dc;

    bootlog("display adapters:");
    for(i=0;;i++)
    {
        char drv[128],date[64],icd[MAX_PATH];
        memset(&dd,0,sizeof(dd));
        dd.cb=sizeof(dd);
        if(!EnumDisplayDevicesA(NULL,i,&dd,0)) break;
        regstring(dd.DeviceKey,"DriverVersion",drv,sizeof(drv));
        regstring(dd.DeviceKey,"DriverDate",date,sizeof(date));
        regstring(dd.DeviceKey,"OpenGLDriverName",icd,sizeof(icd));
        bootlog("  %s \"%s\"%s%s driver %s (%s) opengl icd %s",dd.DeviceName,dd.DeviceString,
                (dd.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)?" desktop":"",
                (dd.StateFlags&DISPLAY_DEVICE_PRIMARY_DEVICE)?" primary":"",drv,date,
                *icd?icd:"(none)");
        memset(&dm,0,sizeof(dm));
        dm.dmSize=sizeof(dm);
        if((dd.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) &&
           EnumDisplaySettingsA(dd.DeviceName,ENUM_CURRENT_SETTINGS,&dm))
            bootlog("    mode %lux%lu %lu bpp %lu Hz at %ld,%ld",dm.dmPelsWidth,dm.dmPelsHeight,
                    dm.dmBitsPerPel,dm.dmDisplayFrequency,dm.dmPosition.x,dm.dmPosition.y);
    }
    dc=GetDC(NULL);
    bootlog("desktop dpi %d, %d monitors",GetDeviceCaps(dc,LOGPIXELSX),GetSystemMetrics(SM_CMONITORS));
    ReleaseDC(NULL,dc);
}

// First-chance exceptions during startup: a driver or overlay that catches
// its own fault and then kills the process never reaches the unhandled
// exception filter, but shows up here.
static LONG CALLBACK firstchancehandler(EXCEPTION_POINTERS *ep)
{
    DWORD code=ep->ExceptionRecord->ExceptionCode;
    char  mod[MAX_PATH];
    uintptr_t off;

    if(code==0x406D1388 || code==0x40010006 || code==0x4001000A) // thread name, debug print
        return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedDecrement(&firstchance)<0) return EXCEPTION_CONTINUE_SEARCH;
    modulename(ep->ExceptionRecord->ExceptionAddress,mod,sizeof(mod),&off);
    bootlog("first-chance exception %08lX at %p (%s+0x%llX)",code,
            ep->ExceptionRecord->ExceptionAddress,mod,(unsigned long long)off);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void atexitlog(void)
{
    bootlog("process exit (exit() called)");
}

void bootlog_begin(void)
{
    char  path[MAX_PATH],*p;
    FILE *f;

    InitializeCriticalSection(&loglock);
    starttick=GetTickCount64();

    // exe folder first, %TEMP%\UltraHLE\ when it can't be written
    GetModuleFileNameA(NULL,logdir,sizeof(logdir));
    p=strrchr(logdir,'\\');
    if(p) p[1]=0; else *logdir=0;
    f=fopen(bootlog_path(path,"startup.log"),"w");
    if(!f)
    {
        GetTempPathA(sizeof(logdir),logdir);
        strcat(logdir,"UltraHLE\\");
        CreateDirectoryA(logdir,NULL);
        f=fopen(bootlog_path(path,"startup.log"),"w");
        if(!f) { *logdir=0; return; }
    }
    fclose(f);

    {
        SYSTEMTIME t;
        char cwd[MAX_PATH];
        GetLocalTime(&t);
        GetCurrentDirectoryA(sizeof(cwd),cwd);
        GetModuleFileNameA(NULL,path,sizeof(path));
        bootlog("UltraHLE startup %04d-%02d-%02d %02d:%02d:%02d",t.wYear,t.wMonth,t.wDay,
                t.wHour,t.wMinute,t.wSecond);
        bootlog("exe: %s",path);
        bootlog("log folder: %s",logdir);
        bootlog("working dir: %s",cwd);
        bootlog("command line: %s",GetCommandLineA());
    }
    logos();
    logdisplays();
    logimports();
    bootlog("other graphics DLLs (UltraHLE uses OpenGL only):");
    logoptional("d3d9.dll","DirectX 9, not used");
    logoptional("dxgi.dll","not used");
    logoptional("glide2x.dll","Glide, no longer used");
    bootlog_modules("at startup");

    AddVectoredExceptionHandler(1,firstchancehandler);
    atexit(atexitlog);
}

void bootlog_ready(void)
{
    InterlockedExchange(&firstchance,0);
    bootlog("startup complete");
    bootlog_modules("after startup");
}

void bootlog_crash(EXCEPTION_POINTERS *ep)
{
    char   path[MAX_PATH],mod[MAX_PATH];
    uintptr_t off;
    HANDLE f;

    modulename(ep->ExceptionRecord->ExceptionAddress,mod,sizeof(mod),&off);
    bootlog("CRASH: exception %08lX at %p (%s+0x%llX)",ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress,mod,(unsigned long long)off);
    bootlog_modules("at crash");

    f=CreateFileA(bootlog_path(path,"ultra.dmp"),GENERIC_WRITE,0,NULL,CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL,NULL);
    if(f!=INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId=GetCurrentThreadId();
        mei.ExceptionPointers=ep;
        mei.ClientPointers=FALSE;
        if(MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),f,
                             MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules,&mei,NULL,NULL))
            bootlog("minidump written: %s",path);
        else
            bootlog("minidump failed, error %lu",GetLastError());
        CloseHandle(f);
    }
}
