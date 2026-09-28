#include "ultra.h"

#define LOGH if(st.dumphw) logh

void hw_checkoften(void)
{
    // Frequently used hardware checks here. Probably things like
    //   interrupts, dma, pads
    // to avoid the cpu having to wait for them too long. This
    // routine is called every CYCLE_CHECKOFTEN cycles (was 5000)

    // PI registers
    // 0: DRAM address
    // 1: Cart address
    // 2: read DRAM length
    // 3: write DRAM length
}

void hw_checkseldom(void)
{
    // Seldom used hardware checks here, to avoid spending too much
    // time on checking them. This routine is called every
    // CYCLE_CHECKSELDOM cycles (was 50000)

    // SI registers
    // 0: DRAM address
    // 1: read 64B
    // 2: write 64B
    // 3: status

    /*
    if(WPI[2]!=NULLFILL)
    {
        logh("hw: SI Dma Transfer (dram:%08X rd:%X wr:%X st:%X)\n",
            WPI[0],WPI[1],WPI[2],WPI[3]);
        WPI[2]=NULLFILL;
        os_event(5);
    }
    */

    // SP registers
    // 0: DW/IW address
    // 1: DRAM address
    // 2: read (dram->mem)
    // 3: write (mem->dram)
    // 4: status/signals
    // 5: DMA full bit
    // 6: DMA busy bit
    // 7: SP semaphore

    // VI registers
    // 0: status/control
    // 1: origin
    // 2: width
    // 3: vertical interrupt
    // 4: current scanline
    //..

    // make the current scanline change (Zelda Waits for it at boot). HLE
    // only: index 10 is V_VIDEO, and LLE keeps its own VI_CURRENT (lle.c).
    // In LLE it overwrote Rogue Squadron's 0x23-0x1FD with 0/0x32/0x64, so
    // the screen height stuck at the logos' 480 and the 224-line game
    // filled half the window.
    if(!st.lleos)
    {
        RVI[10]+=50;
        if(RVI[10]>480)
        {
            RVI[10]&=1;
            RVI[10]^=1;
        }
    }
}

void hw_check(void)
{
    static qword lastcheck=0;
    static qword nextcheck1=0;
    static qword nextcheck2=0;

    if(st.cputime<lastcheck)
    {
        // loaded a state with a smaller cputime than we had
        nextcheck1=0;
        nextcheck2=0;
    }
    lastcheck=st.cputime;

    if(st.cputime>nextcheck1)
    {
        hw_checkoften();
        nextcheck1=st.cputime+CYCLES_CHECKOFTEN;
    }

    if(st.cputime>nextcheck2)
    {
        hw_checkseldom();
        nextcheck2=st.cputime+CYCLES_CHECKSELDOM;
    }
}

/********************************************************************
** PI emulation
*/

void hw_pi_dma(void)
{
    if(WPI[3])
    {
        logh("hw: PI Dma Transfer (dram:%08X cart:%08X rd:%X wr:%X st:%X)\n",
            WPI[0],WPI[1],WPI[2],WPI[3],WPI[4]);
        osPiStartDma(0,0,0,WPI[1],WPI[0],WPI[3]+1,0);
        WPI[3]=0;
    }
}

/********************************************************************
** SP emulation
*/

static dword    sptaskpos;
static OSTask_t sptask;
// SP_STATUS as the CPU reads it (the RSP interpreter updates it in place too)
#define spstatus RSP[4]
static int      sploaded;
static int      spgfxexecuting; // 0=not, 1=yup, 2=done, 3=list paused (dlist_resume)
static int      spgfxyielded;   // the paused list was yielded: it waits for its restart

// NOT USED RIGHT NOW
void hw_gfxthread(void)
{
    if(!st.gfxthread)
    {
        x_sleep(100);
        return;
    }

    if(spgfxexecuting==1)
    {
        //print("gfxthread-start (%i)\n",timer_ms(&st2.timer));
        dlist_execute(&st2.gfxtask);
        //print("gfxthread-end   (%i)\n",timer_ms(&st2.timer));
        spgfxexecuting=2;
    }

    x_sleep(1); // always sleep a little bit
}

void hw_sp_endgfx(void)
{
    os_event(OS_EVENT_SP);
    if(st2.gfxdpsyncpending)
    {
        sync_gfxframedone();
        os_event(OS_EVENT_DP);
    }
    st2.gfxdpsyncpending=0;
    spgfxexecuting=0;
    spgfxyielded=0;
}

void hw_sp_startgfx(void)
{
    if(st.graphicsenable<=0)
    {
        hw_sp_endgfx();
        return;
    }

    // Gauntlet Legends restarts a yielded list from its start (no
    // OS_TASK_YIELDED flag), so a new task clears the yield
    spgfxyielded=0;
    if(!st.gfxthread)
    {
        // run in the same thread; a list the game is still writing
        // waits, and hw_sp_check resumes it
        if(dlist_execute(&st2.gfxtask)) spgfxexecuting=3;
        else hw_sp_endgfx();
    }
    else
    {
        // run in a separate thread
        spgfxexecuting=1;
    }
}

// an HLE task has ended: SP_STATUS halt. In LLE mode it waits for the task's
// SP interrupt (lle.c lle_irqfired), which sets it: a game polling halt
// would otherwise start its next task before that interrupt (Ogre Battle 64)
static void sp_sethalt(void)
{
    if(st.lleos && lle_sptaskpending()) spstatus&=~1;
    else                                spstatus|=1;
}

// once per game (hw_init): Rush 2049 asks for a yield of each HLE task it
// can no longer yield, dozens a second
static int yieldwarned;

void hw_sp_yield(void)
{
    if(!spgfxexecuting)
    {
        LOGH("\n");
        if(!yieldwarned++) warning("hw: spYield with no executing task (not shown again this game)");
        // hack
//        os_event(OS_EVENT_SP);
    }
    else
    {
        // this is a YIELD request
        if(spgfxexecuting)
        {
            spstatus|=(1<<8);  // still executing, raise signal 1
        }
        if(spgfxexecuting==3) spgfxyielded=1;
        // sp task done
        os_event(OS_EVENT_SP);
    }
    // remove signal 0
    spstatus&=~(1<<7);
    // mark that task has halted
    sp_sethalt();
    RSP[4]=spstatus;
}

void hw_sp_start(void)
{
    logh("hw-sp: task %08X (type=%04X,flag=%04X,ucode=%08X/%04X,data=%08X/%04X,boot=%08X/%04X,yield=%08X/%04X)\n",
            sptaskpos,
            sptask.type,sptask.flags,
            sptask.m_ucode,sptask.ucode_size,
            sptask.m_data_ptr,sptask.data_size,
            sptask.m_ucode_boot,sptask.ucode_boot_size,
            sptask.m_yield_data_ptr,sptask.yield_data_size);

    if(sptask.type==1)
    {
        // GFX task
        if(sptask.flags&1)
        {
            // ignore yield continues, we have all the
            // info from the first exec. Keep spexecuting=1
            // since we are still processing (a paused list resumes)
            spgfxyielded=0;
            return;
        }
        else
        {
            memcpy(&st2.gfxtask,&sptask,sizeof(OSTask_t));
            hw_sp_startgfx();
        }
    }
    else
    {
        // NON-GFX task
        if(sptask.type==2 && slist_hle(&sptask))
        {
            memcpy(&st2.audiotask,&sptask,sizeof(OSTask_t));
            prof_begin(PROF_AUDIO);
            if(st.soundenable) slist_execute(&st2.audiotask);
            prof_end(PROF_AUDIO);
            os_event(OS_EVENT_SP);
        }
        else if(sptask.type==4 && cart.iszelda)
        {
            zlist_uncompress(&sptask);
            os_event(OS_EVENT_SP); // sp task done
        }
        else if(st.lleos)
        {
            // no HLE, LLE OS: the game's osSpTaskLoad already put the boot
            // code, the OSTask and SP_PC in place. The RSP runs from there
            // and its break interrupts through the LLE MI (a faked SP event
            // here crashed Pokemon Stadium's scheduler on a null task).
            // A task longer than this slice keeps running from
            // lle_burststart; it halts itself with its BREAK. (Halting it
            // here froze TWINE's MusyX audio mid-task once the music got
            // busy, and the game waited for it forever.)
            RSP[4]=spstatus&~3u;
            rsp_runfor(20000);
        }
        else
        {
            rsp_runtask(&sptask); // no HLE: the RSP interpreter (JPEG decoders...)
            os_event(OS_EVENT_SP); // sp task done
        }
    }
    sploaded=0;
}

// LLE OS mode: a finished task leaves the RSP halted and broken, with
// signal 2 (task done) set, as the real microcode does on exit
void hw_sp_taskdone(void)
{
    spstatus|=0x001|0x002|0x200;
    RSP[4]=spstatus;
}

void hw_sp_check(void)
{
    if(spgfxexecuting==2)
    {
        // gfx task ended
        hw_sp_endgfx();
        sp_sethalt();
        RSP[4]=spstatus;
    }
    // a paused list (not while yielded): has the CPU written more of it?
    if(spgfxexecuting==3 && !spgfxyielded && !(spstatus&1) && !dlist_resume())
    {
        hw_sp_endgfx();
        sp_sethalt();
        RSP[4]=spstatus;
    }
}

// LLE OS mode, a task with no OSTask header (libdragon): the RSP interpreter
// owns SP_STATUS, and the signal bits are the game's, not libultra's yield
static int sprsp;

// SP_STATUS write for the RSP interpreter: every bit as the hardware does
// (the MI interrupt bits 3/4 are in lle_hwwrite), and the RSP runs when
// halt is clear afterwards (Mupen64Plus also waits for broke; hardware
// doesn't: n64-systemtest "RSP running in parallel to the CPU")
static void hw_sp_rspstatuswrite(dword status)
{
    int n;
    // each flag has a clear and a set bit; writing both changes nothing
    // (n64-systemtest "SP Set/Clear Signal", ares io.cpp)
    static const dword flag[11]={1,0x20,0x40,0x80,0x100,0x200,0x400,0x800,
                                 0x1000,0x2000,0x4000};
    static const int   clrbit[11]={0,5,7,9,11,13,15,17,19,21,23};
    if(status&0x04) spstatus&=~2u;     // clear broke
    for(n=0;n<11;n++)
    {   // halt, single step, interrupt on break, signals 0..7
        dword two=(status>>clrbit[n])&3;
        if(two==1) spstatus&=~flag[n];
        if(two==2) spstatus|= flag[n];
    }
    LOGH("hw-sp: rsp command %08X status=%08X\n",status,spstatus);
    // only HALT stops the RSP; BROKE is a flag left by the last BREAK
    // (n64-systemtest restarts it with 0x89, BROKE still set)
    if(!(spstatus&1) && ((status&5) || rsp_pending())) rsp_run();
}

void hw_sp_statuswrite(void)
{
    dword status;
    int   go;

    status=WSP[4];
    WSP[4]=0;
    // LLE: clearing halt with no OSTask loaded starts the game's own
    // microcode on the RSP interpreter (libdragon sets SP_PC itself)
    if(st.lleos && status && status!=0x70707070 &&
       (sprsp || ((status&1) && !sploaded)))
    {
        sprsp=1;
        hw_sp_rspstatuswrite(status);
        return;
    }

    if(status && status!=0x70707070)
    {
        go=0;
        LOGH("hw-sp: command %08X ",status,spstatus);

        if(status&(1<<0))
        { // clear halt, start exec
            LOGH("go! ");
            if(sploaded) go=1;
            else
            {
                LOGH("\n");
                warning("hw: spGo without loaded task");
                os_event(OS_EVENT_SP);
            }
        }
        if(status&(1<<1))
        { // set halt, stop exec
            warning("hw: spHalt\n");
        }
        if(status&(1<<2)) spstatus&=~(1<<1);
        /*
        if(status&(1<<3)) ; // clear intr
        if(status&(1<<4)) ; // set intr
        if(status&(1<<5)) ; // clear sstop
        if(status&(1<<6)) ; // set sstep
        if(status&(1<<7)) ; // clear intr-on-break
        if(status&(1<<8)) ; // set intr-on-break
        */
        if(status&(1<< 9))
        {
            spstatus&=~(1<<7);  // clear signal 0
        }
        if(status&(1<<10))
        {
            spstatus|= (1<<7);  // set   signal 0
            LOGH("yield! ");
            hw_sp_yield();
        }
        if(status&(1<<11)) spstatus&=~(1<<8);  // clear signal 1
        if(status&(1<<12)) spstatus|= (1<<8);  // set   signal 1
        if(status&(1<<13)) spstatus&=~(1<<9);  // clear signal 2
        if(status&(1<<14)) spstatus|= (1<<9);  // set   signal 2
        {   // signals 3..7 and interrupt on break: libdragon's rspq_init
            // sets its BUFDONE signals before the RSP first runs
            int n;
            for(n=3;n<8;n++)
            {
                if(status&(0x200u<<(2*n))) spstatus&=~(0x80u<<n);
                if(status&(0x400u<<(2*n))) spstatus|= (0x80u<<n);
            }
            if(status&0x080) spstatus&=~0x40u;
            if(status&0x100) spstatus|= 0x40u;
        }
        LOGH(" status=%08X\n",spstatus);

        if(go)
        {
            hw_sp_start();
        }
    }

    // a task still on the RSP interpreter (LLE) stays running
    if((spgfxexecuting && !spgfxyielded) || (st.lleos && rsp_pending())) spstatus&=~1;
    else               sp_sethalt();
    RSP[4]=spstatus;
}

void hw_sp_dmawrite(void)
{
    dword from,to,cnt;

    to  =WSP[0];
    from=WSP[1];
    cnt =WSP[2];

    if(!cart.first_rcp)
    {
        print("note: first rcp access\n");
        cart.first_rcp=1;
    }

    LOGH("hw-sp: dma %08X->%08X,%-4X ",from,to,cnt);

    if((to&0xffff)==0x0FC0)
    {
        // detected load if OSTask structure
        sptaskpos=from;
        mem_readrangeraw(from,sizeof(sptask),(char *)&sptask);
        sploaded=1;
        // libultra task: HLE. rspgfx=1 (LLE): gfx tasks run their own
        // microcode on the interpreter, the RDP list drawn raw (Rogue
        // Squadron's Factor 5 microcode has no HLE)
        if(sptask.type==1 && cart.rspgfx && st.lleos) sprsp=1;
        else if(sptask.type==1 || sptask.type==2) sprsp=0;
        rsp_audiotask=(sptask.type==2); // its RSP time counts as sound

        LOGH(" Taskload (type=%i, flags=%i) ",sptask.type,sptask.flags);
    }
    LOGH("\n");

    rsp_dma(0); // the copy itself, for the RSP interpreter

    WSP[0]=0;
    WSP[1]=0;
    WSP[2]=0;
}

/********************************************************************
** Old RSP execution (when directsp=0)
*/

void hw_rspcheck(void)
{
    // this is only used when osSp calls are done directly
    if(st2.audiopending)
    {
        if(st2.gfxdpsyncpending)
        {
            os_event(OS_EVENT_DP); // dp full sync interrupt
            st2.gfxdpsyncpending=0;
        }
        if(st.soundenable)
        {
            slist_execute(&st2.audiotask);
        }
        os_event(OS_EVENT_SP);
        st2.audiopending=0;
    }

    if(st2.gfxpending && !st2.gfxdpsyncpending && !st2.gfxfinishpending && !st2.gfxthread_execute)
    {
        if(st.graphicsenable>0)
        {
            dlist_execute(&st2.gfxtask);
        }

        st2.gfxdpsyncpending=1;
        st2.gfxfinishpending=1;

        st2.gfxpending=0;

        if(st2.gfxfinishpending)
        {
            os_event(OS_EVENT_SP);
            sync_gfxframedone();
            st2.gfxfinishpending=0;
        }

        if(st2.gfxdpsyncpending)
        {
            os_event(OS_EVENT_DP); // dp full sync interrupt
            st2.gfxdpsyncpending=0;
        }
    }
}

/********************************************************************
** SI (pads)
*/

static int selectpad=0;

void hw_selectpad(int pad)
{
    selectpad=pad;
}


void hw_si_pads(int write)
{
    dword base=WSI[0];
    int i;

    logh("hw-si: dma %08X (write=%i)\n",base,write);
    if(!cart.first_pad)
    {
        print("note: first pad access\n");
        cart.first_pad=1;
    }

    if(write)
    { // RDRAM -> PIF: the game's joybus commands
        for(i=0;i<16;i++)
        {
            RPIF[0x1f0+i]=mem_read32(base+i*4);
        }
        pif_write(selectpad);
    }
    else
    { // PIF -> RDRAM: replies (run again so pad data is current)
        pif_read(selectpad);
        for(i=0;i<16;i++)
        {
            mem_write32(base+i*4,RPIF[0x1f0+i]);
        }
    }

    /*
    for(i=0;i<16;i++)
    {
        print("%08X ",RPIF[0x1f0+i]);
        if((i&3)==3) print("\n");
    }
    */

    WSI[1]=NULLFILL;
    WSI[4]=NULLFILL;

    RSI[0]=WSI[0];
    RSI[6]=0; // not busy

    os_event(OS_EVENT_SI);
}

/********************************************************************
** Memory IO Detected entrypoint
**
** a LUI with x40xxxxx was loaded recently, memory IO may have happened.
** This may be called from a_exec, so st.pc is not current.
*/

void hw_init(void)
{
    // init reg data we don't want to be 0 (default init value)
    WSI[1]=NULLFILL;
    WSI[4]=NULLFILL;
    yieldwarned=0;
}

void hw_memio(void)
{
    st.memiodetected=0;
    st2.memiocheck++;

//    print("hw: memio (pc=%08X,memio=%08X,spstat=%08X)\n",st.pc,st.memiodetected,RSP[4]);

    hw_sp_check(); // detectes thread finishes
    if(WSP[4]) hw_sp_statuswrite();
    if(WSP[2]) hw_sp_dmawrite();

    if(WPI[3]) hw_pi_dma();

    if(WSI[1]!=NULLFILL) hw_si_pads(0); // write
    if(WSI[4]!=NULLFILL) hw_si_pads(1); // read
}

// return 1 if the 64K memory page address given should be alerted
// to hw_memio when LUI loads it. This is called from cpuc.c and
// from cpua.c when compiling. It is not called when executing compiled
// code, so speed is not that important here.
int hw_ismemiorange(dword addr)
{
    addr&=0x1fff0000;
    if(addr==0x04040000) return(1); // SP registers are handled
    if(addr==0x04600000) return(1); // PI registers are handled
    if(addr==0x04800000) return(1); // PI registers are handled
    return(0);
}

