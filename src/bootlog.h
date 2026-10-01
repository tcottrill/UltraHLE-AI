// Startup trace: startup.log next to the exe, or %TEMP%\UltraHLE\ when the
// exe folder is not writable. Each line is appended and the file closed, so
// the trace survives a crash or a hard kill at any point. Used to find why
// the program dies on other machines before ultra.log exists.

#ifndef _BOOTLOG_H_
#define _BOOTLOG_H_

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void  bootlog_begin(void);             // pick the folder, write the system report
void  bootlog(const char *fmt,...);    // one timestamped line
void  bootlog_modules(const char *when); // every module loaded in the process
char *bootlog_path(char *out,const char *name); // name in the log folder
void  bootlog_crash(EXCEPTION_POINTERS *ep);    // crash line, modules, ultra.dmp
void  bootlog_ready(void);             // startup done: stop first-chance logging

#ifdef __cplusplus
};
#endif

#endif // _BOOTLOG_H_
