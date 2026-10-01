// Console commands processor
#include "ultra.h"
#include <direct.h>
#include <vector>
#include <string>
#include <map>

static byte *snap;          // Temporary storage for RDRAM dump
static int   snapsize;      // its size (mem.ramsize when taken)

typedef void (*cmd_handler)(std::vector<std::string>& args);
static std::map<std::string, cmd_handler> cmds;

static char* param(int n, std::vector<std::string>& args)
{
    static char buf[1024];
    if (n >= args.size()) {
        buf[0] = 0;
    }
    else {
        strncpy(buf, args[n].c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    }
    return buf;
}

static qword atoq(char *p)
{
    qword res=0;
    dword x;
    // skip space
    while(*p<=32 && *p) p++;
    // check for hex
    if(p[1]=='x' || p[1]=='X')
    {
        sscanf(p,"%i",&x);
        res=x;
    }
    else
    { // construct number
        while(*p>='0' && *p<='9')
        {
            res*=10;
            res+=(*p)-'0';
            p++;
        }
    }
    // check trailing 'k' or 'm'
    if(*p=='k' || *p=='K') res*=1000;
    else if(*p=='m' || *p=='M') res*=1000000;
    return(res);
}

static void setaddress(char *text,uint32_t *addr)
{
    int l,a,o;
    unsigned int mask;
    l=strlen(text);
    if(l<8) mask=0xffffffffU >> (32-l*4);
    else mask=0xffffffffU;
    sscanf(text,"%X",&a);
    o=*addr;
    *addr=(o&(~mask)) | (a&mask);
//    print("old %08X new %08X mask %08X = %08X\n",o,a,mask,*addr);
}

//------------------------------------------------------------

static void printhelp(void)
{
    printcopyright();
#if !RELEASE
    print(
    "\x1\x3------------------ misc --------------------------------------------------------------\n"
          "+             - execute last command again\n"
          "hide3dfx\n"
          "show3dfx\n"
          "pad 0..3\n"
          "keys 0/1      - enable/disable pad keys\n"
          "zeldajap      - set zelda language to Japanese\n"
          "zeldaeng      - set zelda language to English\n"
          "screen <file> - PNG screenshot (no parameter: snap\\<title>_<time>.png)\n"
          "sound         - toggle sound (slist.c)\n"
          "soundsync     - toggle sound syncronization (will slow gfx)\n"
          "gfxthread     - execute graphics in separate thread\n"
          "graphics      - toggle graphics (dlist.c)\n"
          "memory        - show the big picture on rdram usage\n"
          "camera # # #  - move camera (DLIST hack)\n"
          "resolution #  - change resolution #=512/640/800/1024\n"
          "group <addr>  - disassemble X86 compiled group at <addr>\n"
          "wireframe #   - set gfx wireframe 0/1\n"
          "compile <add> - compile and disassemble (default add=pc)\n"
    "\x1\x3------------------ os and statistics -------------------------------------------------\n"
          "osinfo        - list os tasks/queues/events\n"
          "emptyq        - empty all queues\n"
          "event <num>   - send event; <number>,all,sp\n"
          "swap          - force framesync\n"
          "send <x>      - send 0 to queue x\n"
          "stat          - compiler statistics\n"
          "stat2         - list used opcodes (in compiler)\n"
          "stat3         - list execution profile (must be defined in cpua.c)\n"
    "\x1\x3------------------ memory ------------------------------------------------------------\n"
          "disasm <file> <start> <cnt>    - disasseble memory to file\n"
          "disasmrsp <file> <code> <data> - disasseble memory to file\n"
          "savemem <file> <start> <cnt>   - save memory to file\n"
    "\x1\x3------------------ symbols -----------------------------------------------------------\n"
          "sym           - dump symbols\n"
          "findos        - rescan memory for os routines\n"
          "listos        - list currently found os routines\n"
    "\x1\x3------------------ examine/change ----------------------------------------------------\n"
          ".             - view code at pc\n"
          "u  <addr>     - view code at <addr>\n"
          "d  <addr>     - view data at <addr>\n"
          "e  <addr> <d> - set DWORD in memory\n"
          "eb <addr> <d> - set BYTE in memory\n"
          "ss <str>      - search memory for string\n"
          "ssr <str>     - search memory for string (relative chars)\n"
          "filltst <addr>\n"
          "fill <addr> <cnt> <byte>\n"
          "snap          - take memory snapshot\n"
          "snaph <old> <new> - find changed halfword (new <value>)\n"
          "regs          - toggle integer/fpu regs on display (F6)\n"
          "setreg <r> <v>- set register <r> (0..31) to value <v>\n"
    "\x1\x3----------------- execute------------------------------------------------------------\n"
          "s  [cnt]      - step <cnt> instructions using c-sim\n"
          "f  [cnt]      - step <cnt> instructions using a-sim\n"
          "t  <addr>     - execute until PC=address (execute to)\n"
          "mw <addr>     - execute until memory at <addr> written\n"
          "mr <addr>     - execute until memory at <addr> read\n"
          "ma <addr>     - execute until memory at <addr> accessed\n"
          "mc <addr>     - execute until memory at <addr> changed\n"
          "skip <cnt>    - skip <cnt> instructioons\n"
          "goto <addr>   - set PC=addr\n"
          "n             - execute until next instruction (over calls)\n"
          "nr            - execute until next return (to end of THIS routine)\n"
          "nc            - execute until next call\n"
          "nt            - execute until next thread\n"
          "nf            - execute until next frame\n"
          "nm <queue>    - execute until next message op in queue <queue>\n"
    "\x1\x3------------------ logging and stopping ----------------------------------------------\n"
          "info <0/1>    - toggle generic info (console)\n"
          "trace <0/1>   - toggle trace dumping (console)\n"
          "ops <0/1>     - toggle full operation trace info (console)\n"
          "asm <0/1>     - toggle asm compiler info\n"
          "os <0/1>      - toggle os info (ultra.log)\n"
          "hw <0/1>      - toggle os info (ultra.log)\n"
          "gfx <0/1>     - toggle gfx info (dlist.log)\n"
          "snd <0/1>     - toggle sound info (slist.log)\n"
          "all <0/1>     - toggle all of the above on/off\n"
          "stop <0/1/2>  - stopping: 0=exceptions only, 1=and errors (def.), 2=and warnings\n");
    print(
    "\x1\x3------------------ special keys when NOT executing -----------------------------------\n"
          "F1 - show stats      F5 - go      F9 - step next   \n"
          "F2 - window data     F6 - regs    UP - command history\n"
          "F3 - window code     F7 - fstep   PGUP - scroll console\n"
          "F4 - window console  F8 - step    ESC  - stop scrolling\n"
    "\x1\x3------------------ special keys when executin ----------------------------------------\n"
          "See top of screen for pad keys, additionally:\n"
          "F3 - toggle joystick  F6 - quicksave (ultra.sav)  F8  - pause             ESC - break\n"
          "F4 - toggle mouse     F9 - quickload (ultra.sav)  F12 - toggle 3dfx  \n"

          "Graphics test keys:\n"
          "W - wireframe  Q - textures  E - passes  R - misc test toggle\n"
    "\x1\x3------------------ basic commands ----------------------------------------------------\n"
          "save <file>   - save emulator state to file (about 4MB). Default ULTRA.SAV\n"
          "load <file>   - load emulator state from file. Default ULTRA.SAV.\n"
          "rom <file>    - load a new rom file\n"
          "reset         - reset/restart rom\n"
          "sgo           - slow go! (using c emulator)\n"
          "go, z         - go! (using compiler)\n"
          "exit, x       - exit\n"
          );
#endif
}

static void memorypic(void)
{
    int a,i,x,y,cnt,codecnt,d;
    print("Memory RDRAM contents (4MB): C=code? d=data? e=almostempty .=empty\n");

    print("      ");
    for(x=0;x<64;x+=8)
    {
        print("  +%05X ",x*4096);
    }
    print("\n");

    for(y=0;y<16;y++)
    {
        a=(0+y*64)*4096;
        print("%04X: ",a>>16);
        for(x=0;x<64;x++)
        {
            a=(x+y*64)*4096;
            cnt=codecnt=0;
            for(i=0;i<4096;i+=4)
            {
                d=mem_read32(a+i);
                if(d) cnt++;
                if(OP_OP(d)==35 || OP_OP(d)==43 || OP_OP(d)==15) codecnt++;
            }
            if(!cnt) print(".");
            else if(codecnt*8>cnt) print("C");
            else if(cnt<64) print("e");
            else print("d");
            if((x&7)==7) print(" ");
        }
        print("\n");
    }
}

static void printdumping(void)
{
    const char *t[2]={"off","ON "};
    print("Dumping: ");
    print("info:%i "  ,st.dumpinfo);
    print("os:%i "    ,st.dumpos);
    print("hw:%i "    ,st.dumphw);
    print("gfx:%i " ,st.dumpgfx);
    print("snd:%i " ,st.dumpsnd);
    print("wav:%i "   ,st.dumpwav);
    print("trace:%i " ,st.dumptrace);
    print("ops:%i "   ,st.dumpops);
    print("asm:%i "   ,st.dumpasm);
    print("\n");
}

static void savestate(char *name)
{
    FILE *f1;
    a_clearcodecache(); // will remove GROUP opcodes from mem
    if(st.lleos) cache_flushall(); // RAM gets the dirty D-cache lines
    f1=fopen(name,"wb");
    if(f1)
    {
        putc('U',f1);
        putc('0',f1);
        putc('2',f1);
        putc(0x1a,f1);
        mem_save(f1);
        cpu_save(f1);
        os_save(f1);
        lle_save(f1);
        hw_save(f1);
        fclose(f1);
        print("State saved to %s\n",name);
    }
    else print("Error saving state.\n");
}

static void loadstate(char *name)
{
    FILE *f1;
    f1=fopen(name,"rb");
    if(f1)
    {
        int error=0;
        if(getc(f1)!='U') error=1;
        if(getc(f1)!='0') error=1;
        if(getc(f1)!='2') error=1;
        getc(f1);
        if(error)
        {
            print("State %s not compatible.\n",name);
        }
        else
        {
            int opt;

            // do not change optimize setting when loading
            opt=st.optimize;

            st2.exception=0;
            mem_load(f1);
            if(!st2.exception) cpu_load(f1);
            if(!st2.exception) os_load(f1);
            if(!st2.exception) lle_load(f1);
            if(!st2.exception) hw_load(f1);
            cache_reset(); // the caches aren't in the state (saved clean)
            sync_clockload(); // the game's clock goes on from the state's time
            // LLE games run their own libultra: no os-call patches (as in boot).
            // Not where an overlay has since replaced the matched code.
            if(!st2.exception && !st.lleos) sym_reapplypatches();
            if(!st2.exception) print("State loaded from %s\n",name);
            else print("Error loading state from %s.\n",name);

            st.optimize=opt;
        }
        fclose(f1);
    }
    else print("Error loading state from %s.\n",name);
}

#pragma region "Main commands"

static void cmd_help(std::vector<std::string>& args)
{
    printhelp();
}

static void cmd_exit(std::vector<std::string>& args)
{
    exitnow();
}

static void cmd_reset(std::vector<std::string>& args)
{
    reset();
}

// A state's file: a plain name ("ultra", "ultra.sav") lives in the states
// folder next to the exe, made when missing; a path is used as given
// (the Load/Save State dialogs pass full paths).
static void statefile(char *buf, size_t n, const char *name)
{
    char dir[MAXFILE + 8];
    if (strpbrk(name, "\\/:"))
        snprintf(buf, n - 5, "%s", name);
    else
    {
        snprintf(dir, sizeof(dir), "%sstates", init.rootpath);
        _mkdir(dir);
        snprintf(buf, n - 5, "%s\\%s", dir, name);
    }
    if (!strchr(name, '.')) strcat(buf, ".sav");
}

static void cmd_save(std::vector<std::string>& args)
{
    char buf[1024];
    char *p = param(1, args);
    if (!p || *p <= 32)
    {
        char bak[1024];
        statefile(buf, sizeof(buf), "ultra.sav");
        statefile(bak, sizeof(bak), "ultrabak.sav");
        remove(bak);
        rename(buf, bak);
        remove(buf);
    }
    else statefile(buf, sizeof(buf), p);
    p = buf;
    savestate(p);
    flushdisplay();
    print("note: Saved state to '%s'\n", p);
}

static void cmd_rom(std::vector<std::string>& args)
{
    char *p = param(1, args);
    print("note: Loaded rom image from '%s'\n", p);
    boot(p, 0);
}

static void cmd_load(std::vector<std::string>& args)
{
    char buf[1024];
    char *p = param(1, args);
    if (!p || *p <= 32) p = (char*)"ultra.sav";
    statefile(buf, sizeof(buf), p);
    p = buf;
    loadstate(p);

    st.nextswitch = st.cputime + 10000;
    flushdisplay();
    st.graphicsenable = 1;

    // generate some events to avoid being stuck
    /*
    os_event(OS_EVENT_SI);
    os_event(OS_EVENT_PI);
    os_event(OS_EVENT_DP);
    */
    // No fake SP "task done" here: saving stops emulation first, and the
    // stop (cpu_clearstate2) already signals any pending SP task, so a
    // state never has one in flight. A spurious one crashes schedulers
    // that look up the finished task (Aerogauge: osJamMesg to queue 0).

    print("note: Loaded state from '%s'\n", p);
}

static void cmd_go(std::vector<std::string>& args)
{
    int a = view.showhelp;

    if (godisabled) return;

    view.showhelp = 1; view_changed(VIEW_RESIZE); flushdisplay();

    cpu_clearbp();

    // fast startup
    cpu_exec(CYCLES_INFINITE, 1);

    view.showhelp = a; view_changed(VIEW_RESIZE);
}

static void cmd_sgo(std::vector<std::string>& args)
{
    int a = view.showhelp;

    if (godisabled) return;

    view.showhelp = 1; view_changed(VIEW_RESIZE); flushdisplay();

    cpu_clearbp();

    // slow startup
    cpu_exec(CYCLES_INFINITE, 0);

    view.showhelp = a; view_changed(VIEW_RESIZE);
}

#pragma endregion "Main commands"

#pragma region "Step commands"

static void cmd_skip(std::vector<std::string>& args)
{
    qword cnt;
    char* p = param(1, args);
    cnt = atoq(p);
    if (cnt < 1) cnt = 1;
    st.pc += cnt * 4;
    st.branchdelay = 0;
}

static void cmd_goto(std::vector<std::string>& args)
{
    uint32_t to = st.pc;
    setaddress(param(1, args), &to);
    st.pc = to;
    view.codebase = st.pc;
}

static void cmd_s(std::vector<std::string>& args)
{
    qword cnt;
    char* p = param(1, args);
    cnt = atoq(p);
    if (cnt < 1) cnt = 1;
    cpu_clearbp();
    cpu_exec(cnt, 0);
}

static void cmd_f(std::vector<std::string>& args)
{
    qword cnt;
    char* p = param(1, args);
    cnt = atoq(p);
    if (cnt < 1) cnt = 1;
    cpu_clearbp();
    cpu_exec(cnt, 1);
}

static void cmd_n(std::vector<std::string>& args)
{
    cpu_onebp(BREAK_PC, st.pc + 4, 0); // stop at next instruction
    cpu_addbp(BREAK_FWBRANCH, 0, 0); // stop on a forward branch
    cpu_addbp(BREAK_NEXTRET, 0, 0);  // stop on ret
    st.quietbreak = 1;
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_t(std::vector<std::string>& args)
{
    uint32_t to = st.pc;
    setaddress(param(1, args), &to);
    print("Executing until address %08X\n", to);
    cpu_onebp(BREAK_PC, to, 0);
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_mw(std::vector<std::string>& args)
{
    uint32_t to = 0;
    setaddress(param(1, args), &to);
    view.database = to;
    print("Executing until address %08X written\n", to);
    cpu_onebp(BREAK_MEMW, to, 0);
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_mr(std::vector<std::string>& args)
{
    uint32_t to = 0;
    setaddress(param(1, args), &to);
    view.database = to;
    print("Executing until address %08X written\n", to);
    cpu_onebp(BREAK_MEMR, to, 0);
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_ma(std::vector<std::string>& args)
{
    uint32_t to = 0;
    setaddress(param(1, args), &to);
    view.database = to;
    print("Executing until address %08X accessed\n", to);
    cpu_onebp(BREAK_MEM, to, 0);
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_mc(std::vector<std::string>& args)
{
    uint32_t to = 0;
    setaddress(param(1, args), &to);
    view.database = to;
    print("Executing until address %08X changed\n", to);
    cpu_onebp(BREAK_MEMDATA, to, mem_read32(to));
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_nt(std::vector<std::string>& args)
{
    // nextthread
    cpu_onebp(BREAK_NEXTTHREAD, 0, 0);
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_nf(std::vector<std::string>& args)
{
    // nextframe
    cpu_onebp(BREAK_NEXTFRAME, 0, 0);
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_nm(std::vector<std::string>& args)
{
    // nextmsg
    cpu_onebp(BREAK_MSG, 0, atoi(param(1, args)));
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_nr(std::vector<std::string>& args)
{
    // next ret
    cpu_onebp(BREAK_NEXTRET, 0, 0);
    cpu_exec(CYCLES_STEP, 0);
}

static void cmd_nc(std::vector<std::string>& args)
{
    // next call
    cpu_onebp(BREAK_NEXTCALL, 0, 0);
    cpu_exec(CYCLES_STEP, 0);
}

#pragma endregion "Step commands"

#pragma region "Dump commands"

static void set_dump_info()
{
    if (st.dumpinfo || st.dumpsnd || st.dumpgfx || st.dumphw ||
        st.dumpos || st.dumpops || st.dumpasm || st.dumptrace)
    {
        st.dumpinfo = 1;
    }
}

static void cmd_info(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumpinfo = 0;
    else if (*p == '1') st.dumpinfo = 1;
    else st.dumpinfo ^= 1;
    printdumping();
}

static void cmd_os(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumpos = 0;
    else if (*p == '1') st.dumpos = 1;
    else st.dumpos ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_hw(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumphw = 0;
    else if (*p == '1') st.dumphw = 1;
    else st.dumphw ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_ops(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumpops = 0;
    else if (*p == '1') st.dumpops = 1;
    else st.dumpops ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_asm(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumpasm = 0;
    else if (*p == '1') st.dumpasm = 1;
    else st.dumpasm ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_gfx(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumpgfx = 0;
    else if (*p == '1') st.dumpgfx = 1;
    else st.dumpgfx ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_snd(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumpsnd = 0;
    else if (*p == '1') st.dumpsnd = 1;
    else st.dumpsnd ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_wav(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumpwav = 0;
    else if (*p == '1') st.dumpwav = 1;
    else st.dumpwav ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_trace(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0') st.dumptrace = 0;
    else if (*p == '1') st.dumptrace = 1;
    else st.dumptrace ^= 1;
    set_dump_info();
    printdumping();
}

static void cmd_all(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p != '0' && *p != '1')
    {
        if (st.dumpinfo || st.dumpsnd || st.dumpgfx || st.dumphw ||
            st.dumpos || st.dumpops || st.dumpasm || st.dumptrace) p = (char*)"0";
        else p = (char*)"1";
    }

    if (*p == '0')
    {
        st.dumpinfo = 0;
        st.dumpsnd = 0;
        st.dumpgfx = 0;
        st.dumpos = 0;
        st.dumptrace = 0;
    }
    else if (*p == '1')
    {
        st.dumpinfo = 1;
        st.dumpsnd = 1;
        st.dumpgfx = 1;
        st.dumpos = 1;
        st.dumptrace = 1;
    }
    set_dump_info();
    printdumping();
}

static void cmd_stop(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '0')
    {
        st.stoperror = 0;
        st.stopwarning = 0;
    }
    else if (*p == '1')
    {
        st.stoperror = 1;
        st.stopwarning = 0;
    }
    else if (*p == '2')
    {
        st.stoperror = 1;
        st.stopwarning = 1;
    }
    print("Stopping on: exceptions ");
    if (st.stoperror) print("errors ");
    if (st.stopwarning) print("warnings ");
    print("\n");
    set_dump_info();
    printdumping();
}

#pragma endregion "Dump commands"

#pragma region "Exam commands"

static void cmd_dot(std::vector<std::string>& args)
{
    view.codebase = st.pc - 4 * (view.coderows / 3);
    print("Unassemble at %08X\n", view.codebase);
}

static void cmd_u(std::vector<std::string>& args)
{
    setaddress(param(1, args), &view.codebase);
    print("Unassemble at %08X\n", view.codebase);
}

static void cmd_d(std::vector<std::string>& args)
{
    setaddress(param(1, args), &view.database);
    print("Data at %08X\n", view.database);
}

static void cmd_e(std::vector<std::string>& args)
{
    dword addr, data;
    setaddress(param(1, args), &addr);
    char *p = param(2, args);
    data = atoq(p);
    mem_write32(addr, data);
    view.database = addr;
}

static void cmd_eb(std::vector<std::string>& args)
{
    dword addr, data;
    setaddress(param(1, args), &addr);
    char* p = param(2, args);
    data = atoq(p);
    mem_write8(addr, data);
    view.database = addr;
}

static void cmd_ss(std::vector<std::string>& args)
{
    char* str = param(1, args);
    int i, j, l, n;
    l = strlen(str);
    n = mem.ramsize - l;
    print("Searching for '%s':\n", str);
    for (i = 0; i < n; i++)
    {
        if (mem.ram[i ^ 3] == str[0])
        {
            for (j = 0; j < l; j++)
            {
                if (mem_read8(i + j) != str[j]) break;
            }
            if (j == l)
            {
                print("- %08X\n", i);
            }
        }
    }
}

static void cmd_ssmario(std::vector<std::string>& args)
{
    char* str = param(1, args);
    int i, j, l, n, flag;
    l = strlen(str);
    n = mem.ramsize - l;
    print("Searching for\n", str);
    for (i = 16; i < n - 16; i++)
    {
        if (mem.ram[i] == 37)
        {
            flag = 0;
            for (j = -16; j < 16; j++)
            {
                if (mem.ram[i + j] == 76) flag |= 1;
                if (mem.ram[i + j] == 61) flag |= 2;
                if (mem.ram[i + j] == 68) flag |= 4;
            }
            if (flag == 7)
            {
                print("- %08X\n", i);
            }
        }
    }
}

static void cmd_snap(std::vector<std::string>& args)
{
    if (!snap || snapsize != mem.ramsize)
    {
        free(snap);
        snapsize = mem.ramsize;
        snap = (byte*)malloc(snapsize);
    }
    memcpy(snap, mem.ram, snapsize);
    print("Snapshot taken\n");
}

static void cmd_snaphw(std::vector<std::string>& args)
{
    char* p;
    int xold, xnew;
    p = param(1, args); sscanf(p, "%i", &xold);
    p = param(2, args); sscanf(p, "%i", &xnew);
    if (!snap)
    {
        print("no snapshot to compare to\n");
    }
    else
    {
        short* mo, * mn;
        int i, cnt = 0;
        mo = (short*)snap;
        mn = (short*)mem.ram;
        for (i = 0; i < snapsize / 2 && i < mem.ramsize / 2; i++)
        {
            if (mo[i] == xold && mn[i] == xnew)
            {
                print("- %08X\n", i * 2 ^ 2);
                cnt++;
                if (cnt > 30)
                {
                    print("too many\n");
                    break;
                }
            }
        }
    }
}

static void cmd_fill(std::vector<std::string>& args)
{
    int addr, cnt, data;
    char* p;
    p = param(1, args); sscanf(p, "%i", &addr);
    p = param(2, args); sscanf(p, "%i", &cnt);
    p = param(3, args); sscanf(p, "%i", &data);
    while (cnt-- > 0)
    {
        mem_write8(addr++, data);
    }
}

static void cmd_filltst(std::vector<std::string>& args)
{
    int addr, i;
    char *p = param(1, args); sscanf(p, "%i", &addr);
    for (i = 0; i < 32; i++)
    {
        mem_write16(addr + i * 2, i + 0x30);
    }
}

static void cmd_ssr(std::vector<std::string>& args)
{
    char* str = param(1, args);
    int i, j, l, n, r0;
    l = strlen(str);
    n = mem.ramsize - l;
    print("Searching for '%s' (relative):\n", str);
    for (i = 0; i < n; i++)
    {
        r0 = mem_read8(i);
        for (j = 1; j < l; j++)
        {
            if (mem_read8(i + j) - r0 != str[j] - str[0]) break;
        }
        if (j == l)
        {
            print("- %08X\n", i);
        }
    }
}

static void cmd_regs(std::vector<std::string>& args)
{
    view.showfpu++;
    if (view.showfpu > 2) view.showfpu = 0;
}

#pragma endregion "Exam commands"

#pragma region "Sym commands"

static void cmd_saveoscalls(std::vector<std::string>& args)
{
    sym_demooscalls();
}

static void cmd_findos(std::vector<std::string>& args)
{
    sym_load(cart.symname); // reload symbols
    sym_findoscalls(0, 4 * 1024 * 1024, 0);
    sym_addpatches();
}

static void cmd_listos(std::vector<std::string>& args)
{
    sym_dumposcalls();
}

static void cmd_sym(std::vector<std::string>& args)
{
    sym_dump();
}

static void cmd_addos(std::vector<std::string>& args)
{
/*
    char name[80];
    dword addr = view.codebase;
    setaddress(param(1, args), &addr);
    strcpy(name, param(2, args));
    if (!*name) strcpy(name, "?");
    print("Addos %08X name %s\n", addr, name),
        symfind_saveroutine(addr, name);
*/
}

#pragma endregion "Sym commands"

#pragma region "Misc commands"

static void cmd_pad(std::vector<std::string>& args)
{
    int a;
    a = atoi(param(1, args));
    hw_selectpad(a);
}

// autopad <period> [mask]: press buttons (pad.c CONT_* bits, default A 0080)
// for 3 retraces every <period> retraces; autopad 0 stops
static void cmd_autopad(std::vector<std::string>& args)
{
    char* p;
    pad_autoperiod = atoi(param(1, args));
    p = param(2, args); // param() reuses one buffer: read after the period
    pad_automask = *p ? (int)strtoul(p, NULL, 16) : 0x0080;
    pad_autostickx = atoi(param(3, args));
    pad_autosticky = atoi(param(4, args));
    print("autopad: buttons %04X every %i retraces, stick %i,%i\n", pad_automask, pad_autoperiod, pad_autostickx, pad_autosticky);
}

// combiner 0|1: the old pattern compiler or the N64 combiner in the shader
static void cmd_combiner(std::vector<std::string>& args)
{
    char *p = param(1, args);
    if (*p) rdp_n64combiner = atoi(p) != 0;
    else rdp_n64combiner ^= 1;
    rdp_forcemodechange();
    print("combiner: %s\n", rdp_n64combiner ? "N64 (shader)" : "old pattern compiler");
}

// softrdp 0|1: raw RDP lists (libdragon) drawn by OpenGL or in software
static void cmd_softrdp(std::vector<std::string>& args)
{
    inifile_setsoftrdp(atoi(param(1, args)));
    print("softrdp: %s\n", inifile_softrdp() ? "software" : "OpenGL");
}

static void cmd_keys(std::vector<std::string>& args)
{
    char *p = param(1, args);
    if (*p == '1') st.keyboarddisable = 0;
    else if (*p == '0') st.keyboarddisable = 1;
    else st.keyboarddisable ^= 1;
    print("keyboard: %s\n", !st.keyboarddisable ? "enabled" : "disabled");
}

static void cmd_hide3dfx(std::vector<std::string>& args)
{
    dlist_ignoregraphics(1);
    rdp_closedisplay();
}

static void cmd_show3dfx(std::vector<std::string>& args)
{
    dlist_ignoregraphics(0);
}

static void cmd_swap(std::vector<std::string>& args)
{
    rdp_framestart();
    rdp_frameend();
}

static void cmd_sound(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '1') st.soundenable = 1;
    else if (*p == '0') st.soundenable = 0;
    else st.soundenable ^= 1;
    print("Sound: %s\n", st.soundenable ? "enabled" : "disabled");
}

static void cmd_pimem(std::vector<std::string>& args)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        print("%08X ", WPI[i]);
    }
    print("\n");
}

static void cmd_soundsync(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '1') st.soundslowsgfx = 1;
    else if (*p == '0') st.soundslowsgfx = 0;
    else st.soundslowsgfx ^= 1;
    print("Soundsync: %s\n", st.soundenable ? "enabled" : "disabled");
}

static void cmd_gfxthread(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '1') st.gfxthread = 1;
    else if (*p == '0') st.gfxthread = 0;
    else st.gfxthread ^= 1;
    print("Gfxthread: %s\n", st.gfxthread ? "enabled" : "disabled");
}

static void cmd_graphics(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (*p == '1') st.graphicsenable = 1;
    else if (*p == '0') st.graphicsenable = 0;
    else st.graphicsenable ^= 1;
    print("Graphics: %s\n", st.graphicsenable ? "enabled" : "disabled");
}

static void cmd_zeldajap(std::vector<std::string>& args)
{
    if (cart.iszelda)
    {
        mem_write8(0x8011b9d9, 0);
        print("Zelda-language: Japanese\n");
    }
    else
    {
        print("Cart not Zelda.\n");
    }
}

static void cmd_zeldaeng(std::vector<std::string>& args)
{
    if (cart.iszelda)
    {
        mem_write8(0x8011b9d9, 1);
        print("Zelda-language: English\n");
    }
    else
    {
        print("Cart not Zelda.\n");
    }
}

static void cmd_setreg(std::vector<std::string>& args)
{
    int r, v;
    sscanf(param(1, args), "%i", &r);
    sscanf(param(2, args), "%i", &v);
    st.g[r].d = v;
}

static void cmd_camera(std::vector<std::string>& args)
{
    float x, y, z;
    x = atof(param(1, args));
    y = atof(param(2, args));
    z = atof(param(3, args));
    dlist_cammove(x, y, z);
}

static void cmd_resolution(std::vector<std::string>& args)
{
    int a;
    a = atoi(param(1, args));
    if (a < 320 || a>2048) a = 640;
    init.gfxwid = a;
    init.gfxhig = 480 * a / 640;
    rdp_closedisplay();
}

static void cmd_wireframe(std::vector<std::string>& args)
{
    int a;
    a = atoi(param(1, args));
    showwire = a;
}

static void cmd_boot(std::vector<std::string>& args)
{
    memcpy(sp.dmem, cart.data, DMEM_SIZE);
    cpu_goto(DMEM_ADDRESS + 0x40);
}

static void cmd_memory(std::vector<std::string>& args)
{
    memorypic();
}

static void group_compile(int compile, std::vector<std::string>& args)
{
    dword  x;
    float  ratio = 0.0;
    int    gi;
    Group* g;
    char* p2;
    int    i, ia;

    p2 = param(1, args);
    if (*p2 < '0')
    {
        x = st.pc;
    }
    else
    {
        x = view.codebase;
        setaddress(p2, &x);
    }

    view.codebase = x;
    print("Unassemble group at %08X:", x);

    gi = mem_groupat(x);
    if (gi < 0 && compile)
    {
        print(" compiling:");
        a_compilegroupat(x);
        gi = mem_groupat(x);
    }
    if (gi < 0)
    {
        print(" not found\n");
        i = 0;
    }
    else
    {
        g = mem.group + gi;
        print(" group %i, len %i, code %p\n", gi, g->len, (void*)g->code);
        print("grouptable %p st %p greg %p freg %p\n",
            (void*)mem.group, (void*)&st, (void*)&st.g[0], (void*)&st.f[0]);
        if (g->code)
        {
            byte* code = g->code;
            print("Prefixed DWORDS: pc:%08X j1:%08X j2:%08X\n",
                *(dword*)(code - 14),
                *(dword*)(code - 8),
                *(dword*)(code - 4));
            for (i = 0; i < 1000; i++)
            {
                if (*code == 0xcc) break; // int 3 marks end
                print("%p: " NORMAL "%s\n",
                    (void*)code,
                    disasmx86(code, (int)(intptr_t)code, &ia)); // offset is display-only
                code += ia;
            }
            ratio = (float)(code - g->code) / (g->len * 4);
        }
        else i = 0;
    }
    print("Total %i x86ops, expansion ratio %.2f\n", i, ratio);
}

static void cmd_group(std::vector<std::string>& args)
{
    group_compile(0, args);
}

static void cmd_compile(std::vector<std::string>& args)
{
    group_compile(1, args);
}

static void cmd_disasm(std::vector<std::string>& args)
{
    char  file[256];
    dword base, cnt;
    char* p;
    p = param(1, args);
    strcpy(file, p);
    p = param(2, args);
    sscanf(p, "%i", &base);
    p = param(3, args);
    sscanf(p, "%i", &cnt);
    if (cnt > 0)
    {
        print("Disassembling %08X..%08X to %s\n",
            base, base + cnt, file);
        disasm_dumpcode(file, base, cnt, 0x10000000, 0x40);
    }
    else
    {
        print("usage: disasm <file> <base> <cnt>\n");
    }
}

static void cmd_disasmrsp(std::vector<std::string>& args)
{
    char  file[256];
    dword base, cnt;
    char* p;
    p = param(1, args);
    strcpy(file, p);
    p = param(2, args);
    sscanf(p, "%i", &base);
    p = param(3, args);
    sscanf(p, "%i", &cnt);
    if (base > 0 && cnt > 0)
    {
        print("Disassembling ucode %08X,%08X to %s\n",
            base, base + cnt, file);
        disasm_dumpucode(file, base, 4096, cnt, 4096, 0x80);
    }
    else
    {
        print("usage: disasmrsp <file> <base> <cnt>\n");
    }
}

static void cmd_savemem(std::vector<std::string>& args)
{
    char  file[256];
    dword base, cnt;
    char* p;
    p = param(1, args);
    strcpy(file, p);
    p = param(2, args);
    sscanf(p, "%i", &base);
    p = param(3, args);
    sscanf(p, "%i", &cnt);
    if (cnt > 0)
    {
        FILE* f1;
        print("Saving %08X..%08X to %s\n",
            base, base + cnt, file);
        if(st.lleos) cache_flushall(); // RAM gets the dirty D-cache lines
        f1 = fopen(file, "wb");
        if (f1)
        {
            int i;
            dword x;
            for (i = 0; i < cnt; i += 4)
            {
                x = mem_read32(base + i);
                x = FLIP32(x);
                fwrite(&x, 1, 4, f1);
            }
            fclose(f1);
        }
    }
    else
    {
        print("usage: savemem <file> <base> <cnt>\n");
    }
}

static void cmd_stat(std::vector<std::string>& args)
{
    a_stats();
}

static void cmd_stat2(std::vector<std::string>& args)
{
    a_stats2();
}

static void cmd_stat3(std::vector<std::string>& args)
{
    a_stats3();
}

static void cmd_screen(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (!p || *p <= 32) p = NULL;
    rdp_screenshot(p);
}

static void cmd_osinfo(std::vector<std::string>& args)
{
    os_dumpinfo();
}

// legacypatches 0|1|ini: force the ultra.ini patch= lines off or on (or back
// to the ini setting) for the next rom load
static void cmd_legacypatches(std::vector<std::string>& args)
{
    char* p = param(1, args);
    if (!_stricmp(p, "0")) inifile_forcelegacy(0);
    else if (!_stricmp(p, "1")) inifile_forcelegacy(1);
    else if (!_stricmp(p, "ini")) inifile_forcelegacy(-1);
    else { print("usage: legacypatches 0|1|ini\n"); return; }
    print("legacy ini patches: %s (from the next rom load)\n", p);
}

// osmode auto|lle|hle|ini: run the next rom in LLE or HLE OS mode, let the
// OS search decide (auto), or follow ultra.ini's osmode= again (ini)
static void cmd_osmode(std::vector<std::string>& args)
{
    char* p = param(1, args);
    int m = inifile_parseosmode(p);
    if (!_stricmp(p, "ini")) inifile_forceosmode(-1);
    else if (m >= 0) inifile_forceosmode(m);
    else { print("usage: osmode auto|lle|hle|ini\n"); return; }
    print("OS mode: %s (from the next rom load)\n", p);
}

// sample 0|1: the sampling profiler (timer.c); LLE prints the functions the
// host's time went to with every perf: line
static void cmd_sample(std::vector<std::string>& args)
{
    int on = atoi(param(1, args));
    prof_sample(on);
    print("sampling profiler: %s\n", on ? "on" : "off");
}

// countperop N: ultra.ini's countperop= for the loaded rom, for a test run
static void cmd_countperop(std::vector<std::string>& args)
{
    cart.countperop = atoi(param(1, args));
    print("countperop: %i\n", cart.countperop);
}

// gfxtime N: HLE gfx tasks report done N ms after they start, as the ini's gfxtime=
static void cmd_gfxtime(std::vector<std::string>& args)
{
    cart.gfxtime = atoi(param(1, args));
    print("gfxtime: %i ms\n", cart.gfxtime);
}

// rspgfx 0|1: LLE gfx tasks on the RSP interpreter (raw RDP), as the ini's rspgfx=
static void cmd_rspgfx(std::vector<std::string>& args)
{
    cart.rspgfx = atoi(param(1, args));
    print("rspgfx: %i\n", cart.rspgfx);
}

static void cmd_clearosinfo(std::vector<std::string>& args)
{
    os_clearthreadtime();
}

static void cmd_emptyq(std::vector<std::string>& args)
{
    print("Queues cleared\n");
    os_clearqueues();
}

static void cmd_event(std::vector<std::string>& args)
{
    int cnt, a;
    char* p = param(1, args);
    a = toupper(*p);
    if (a == 'A') cnt = -1;
    else if (a == 'S') cnt = OS_EVENT_SP;
    else if (a == 'R') cnt = OS_EVENT_RETRACE;
    else cnt = atoi(p);
    if (cnt == -1)
    {
        for (cnt = 0; cnt < 10; cnt++)
        {
            print("Generated event %i\n", cnt);
            os_event(cnt);
        }
        cnt = 15;
        print("Generated event %i\n", cnt);
        os_event(cnt);
    }
    else
    {
        print("Generated event %i\n", cnt);
        os_event(cnt);
    }
}

static void cmd_send(std::vector<std::string>& args)
{
    int cnt, data;
    char* p;
    p = param(1, args);
    cnt = atoi(p);
    p = param(2, args);
    data = atoi(p);
    print("Sending %i to queue %i\n", data, cnt);
    os_stuffqueue(cnt, data);
}

#pragma endregion "Misc commands"

extern "C"
void cmd_init()
{
    // Main commands
    cmds["help"] = cmd_help;
    cmds["x"] = cmd_exit;
    cmds["exit"] = cmd_exit;
    cmds["reset"] = cmd_reset;
    cmds["save"] = cmd_save;
    cmds["rom"] = cmd_rom;
    cmds["load"] = cmd_load;
    cmds["go"] = cmd_go;
    cmds["sgo"] = cmd_sgo;

    // Step commands
    cmds["skip"] = cmd_skip;
    cmds["goto"] = cmd_goto;
    cmds["s"] = cmd_s;
    cmds["f"] = cmd_f;
    cmds["n"] = cmd_n;
    cmds["t"] = cmd_t;
    cmds["mw"] = cmd_mw;
    cmds["mr"] = cmd_mr;
    cmds["ma"] = cmd_ma;
    cmds["mc"] = cmd_mc;
    cmds["nt"] = cmd_nt;
    cmds["nf"] = cmd_nf;
    cmds["nm"] = cmd_nm;
    cmds["nr"] = cmd_nr;
    cmds["nc"] = cmd_nc;

    // Dump commands
    cmds["info"] = cmd_info;
    cmds["os"] = cmd_os;
    cmds["hw"] = cmd_hw;
    cmds["ops"] = cmd_ops;
    cmds["asm"] = cmd_asm;
    cmds["gfx"] = cmd_gfx;
    cmds["snd"] = cmd_snd;
    cmds["wav"] = cmd_wav;
    cmds["trace"] = cmd_trace;
    cmds["all"] = cmd_all;
    cmds["stop"] = cmd_stop;

    // Exam commands
    cmds["."] = cmd_dot;
    cmds["u"] = cmd_u;
    cmds["d"] = cmd_d;
    cmds["e"] = cmd_e;
    cmds["eb"] = cmd_eb;
    cmds["ss"] = cmd_ss;
    cmds["ssmario"] = cmd_ssmario;
    cmds["snap"] = cmd_snap;
    cmds["snaphw"] = cmd_snaphw;
    cmds["fill"] = cmd_fill;
    cmds["filltst"] = cmd_filltst;
    cmds["ssr"] = cmd_ssr;
    cmds["regs"] = cmd_regs;

    // Sym commands
    cmds["saveoscalls"] = cmd_saveoscalls;
    cmds["findos"] = cmd_findos;
    cmds["listos"] = cmd_listos;
    cmds["sym"] = cmd_sym;
    //cmds["addos"] = cmd_addos;

    // Misc commands
    cmds["pad"] = cmd_pad;
    cmds["keys"] = cmd_keys;
    cmds["autopad"] = cmd_autopad;
    cmds["combiner"] = cmd_combiner;
    cmds["softrdp"] = cmd_softrdp;
    cmds["hide3dfx"] = cmd_hide3dfx;
    cmds["show3dfx"] = cmd_show3dfx;
    cmds["swap"] = cmd_swap;
    cmds["sound"] = cmd_sound;
    cmds["pimem"] = cmd_pimem;
    cmds["soundsync"] = cmd_soundsync;
    cmds["gfxthread"] = cmd_gfxthread;
    cmds["graphics"] = cmd_graphics;
    cmds["zeldajap"] = cmd_zeldajap;
    cmds["zeldaeng"] = cmd_zeldaeng;
    cmds["setreg"] = cmd_setreg;
    cmds["camera"] = cmd_camera;
    cmds["resolution"] = cmd_resolution;
    cmds["wireframe"] = cmd_wireframe;
    cmds["boot"] = cmd_boot;
    cmds["memory"] = cmd_memory;
    cmds["group"] = cmd_group;
    cmds["compile"] = cmd_compile;
    cmds["disasm"] = cmd_disasm;
    cmds["disasmrsp"] = cmd_disasmrsp;
    cmds["savemem"] = cmd_savemem;
    cmds["stat"] = cmd_stat;
    cmds["stat2"] = cmd_stat2;
    cmds["stat3"] = cmd_stat3;
    cmds["screen"] = cmd_screen;
    cmds["osinfo"] = cmd_osinfo;
    cmds["legacypatches"] = cmd_legacypatches;
    cmds["osmode"] = cmd_osmode;
    cmds["sample"] = cmd_sample;
    cmds["countperop"] = cmd_countperop;
    cmds["rspgfx"] = cmd_rspgfx;
    cmds["gfxtime"] = cmd_gfxtime;
    cmds["clearosinfo"] = cmd_clearosinfo;
    cmds["emptyq"] = cmd_emptyq;
    cmds["event"] = cmd_event;
    cmds["send"] = cmd_send;
}

void get_autocomplete_list(std::vector<std::string>& cmdlist)
{
    cmdlist.clear();
    for (auto it = cmds.begin(); it != cmds.end(); ++it)
    {
        cmdlist.push_back(it->first);
    }
}

static void tokenize(const char* text, std::vector<std::string>& args)
{
    #define endl    ( text[p] == 0 )
    #define space   ( text[p] == 0x20 )
    #define quot    ( text[p] == '\'' )
    #define dquot   ( text[p] == '\"' )

    int p, start, end;
    p = start = end = 0;

    args.clear();

    // while not end line
    while (!endl)
    {
        // skip space first, if any
        while (space) p++;
        if (!endl && (quot || dquot))
        {   // quotation: ends only at the same kind of quote, so a
            // "double-quoted path" may contain apostrophes (Majora's Mask)
            char q = text[p];
            p++;
            start = p;
            while (1)
            {
                if (endl)
                {
                    throw "Open quotation";
                    return;
                }

                if (text[p] == q)
                {
                    end = p;
                    p++;
                    break;
                }
                else p++;
            }

            args.push_back(std::string(text + start, end - start));
        }
        else if (!endl)
        {
            start = p;
            while (1)
            {
                if (endl || space) // quotes inside a word are part of it
                {
                    end = p;
                    break;
                }

                p++;
            }

            if (args.size() == 0) {
                // The name of the command is always in lower case
                char temp[0x100]{};
                strncpy(temp, text + start, end - start);
                _strlwr(temp);
                args.push_back(std::string(temp));
            }
            else {
                args.push_back(std::string(text + start, end - start));
            }
        }
    }
    #undef space
    #undef quot
    #undef dquot
    #undef endl
}

extern "C"
void command(char *cmd)
{
    char *cs,*cd;
    char  buf[1024]{};

    view_clear_cmdline();

    cs=cmd;
    while(*cs)
    {
        // copy next command (until eos or ;)
        cd=buf;
        while(*cs && *cs<=32) cs++;
        while(*cs && *cs!=';')
        {
            *cd++=*cs++;
        }
        *cd=0;
        if(*cs==';') cs++;
        while(*cs && *cs<=32) cs++;
        
        // parse arguments (command name = arg[0])
        std::vector<std::string> args;
        tokenize(buf, args);
        if (args.size() == 0) {
            continue;
        }

        // execute
        auto it = cmds.find(args[0]);
        if (it != cmds.end()) {
            it->second(args);
        }
        else {
            print("Unknown command '%s', try help.\n", args[0].c_str());
        }
    }

    print(NULL);
}
