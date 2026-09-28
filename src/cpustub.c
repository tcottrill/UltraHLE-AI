// x64 build: the dynamic recompiler (cpua.c, cpuanew.c, cpuautil.c) emits
// 32-bit x86 machine code and is left out of the build; the portable C
// interpreter (cpuc.c) executes everything. These stand in for the
// recompiler's entry points used elsewhere. Empty in the x86 build.

#include "ultra.h"

#ifdef _M_X64

void a_exec(void)              { c_exec(); } // "fast" mode runs the interpreter
void a_clearcodecache(void)    { mem.groupnum=0; mem.codeused=0; }
void a_cleardeadgroups(void)   { }
void a_compilegroupat(dword x) { (void)x; print("x64 build has no recompiler.\n"); }
void a_stats(void)             { print("x64 build has no recompiler.\n"); }
void a_stats2(void)            { a_stats(); }
void a_stats3(void)            { a_stats(); }

#endif
