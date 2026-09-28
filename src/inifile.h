#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void inifile_command(char* cmd);
void inifile_read(char* cartnamep);
void inifile_readtemp(char* cartnamep);
void inifile_patches(int dmanum);
int  inifile_hasrescan(void); // the game's ini entry rescans for OS routines later
void inifile_forcelegacy(int on); // legacy patch= lines: 1 on, 0 off, -1 as ultra.ini says

// OS mode choice (ultra.ini osmode=, 'osmode' command)
#define OSMODE_AUTO 0 // the boot OS search decides (lle_decide)
#define OSMODE_LLE  1
#define OSMODE_HLE  2
int  inifile_osmode(void);
void inifile_forceosmode(int mode); // OSMODE_*, -1 = as ultra.ini says
int  inifile_parseosmode(const char *s); // "auto"/"lle"/"hle" -> OSMODE_*, -1 unknown

int  inifile_rdramsize(void); // bytes: RDRAMSIZE, or RDRAMSIZE_PAK with rdram=8
int  inifile_softrdp(void);   // softrdp=1: raw RDP lists drawn in software
void inifile_setsoftrdp(int on);

#ifdef __cplusplus
};
#endif
