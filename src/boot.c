#include "ultra.h"

// note: cart must be loaded first

void boot_boot(void)
{
    int a;

    // memory init (Must do before cpuinit!)
    mem_init(inifile_rdramsize());
    if(mem.ramsize!=RDRAMSIZE) print("RDRAM %iMB (Expansion Pak)\n",mem.ramsize>>20);

    // cpu/compiler init
    cpu_init();

    // rsp
    rsp_init();

    // os emulator structures
    os_init();

    // map cart into memory (read only)
    for(a=0;a<cart.size;a+=4096)
    {
        mem_mapexternal(0x10000000+a,MAP_R,cart.data+a);
        mem_mapexternal(0x90000000+a,MAP_R,cart.data+a);
        mem_mapexternal(0xb0000000+a,MAP_R,cart.data+a);
    }

    // CIC from the sum of the IPL3 words (Mupen64Plus cic.c, Project64
    // GetCicChipID); 6102 if unknown
    {
        unsigned long long sum=0;
        for(a=0x40;a<0x1000;a+=4) sum+=mem_read32(0x10000000+a);
        switch(sum)
        {
        case 0xD0027FDF31ULL:
        case 0xCFFB631223ULL: cart.cic=6101; break;
        case 0xD6497E414BULL: cart.cic=6103; break;
        case 0x11A49F60E96ULL: cart.cic=6105; break;
        case 0xD6D5BE5580ULL: cart.cic=6106; break;
        default:              cart.cic=6102; break;
        }
    }

    // IPL3 copies its second stage to RDRAM and runs it there: the 6105
    // one is IPL3 0x554..0x887 at RDRAM 4 (N64-IPL ipl3.s, block17s to
    // pifipl3e), and what nothing overwrites stays. Donkey Kong 64 reads
    // 0xA00002E8 (IPL3 0x838, 0xC86E2000) at 80611730 and stops freeing
    // memory when it isn't there: the DK Rap froze once its list of blocks
    // to free was full. Before the words at 0x300, which that stage writes
    // over its own end
    if(cart.cic==6105)
    {
        for(a=0x554;a<0x888;a+=4)
            mem_write32(0xA0000004+(a-0x554),mem_read32(0x10000000+a));
    }

    // The 6106 one is IPL3 0x4F0..0x7AB (175 words) at RDRAM 0, encrypted in
    // the ROM: each word is XORed with a key that starts at seed*0x260BCD5+1
    // (the 6106 seed is 0x85) and is multiplied by 0x260BCD5 per word (N64-IPL
    // ipl3.s, load_ipl3 for IPL3_X106). Cruis'n World's start-up compares
    // four of its words (RDRAM 0x164, 0x1BC, 0x234, 0x2B8) and stops in an
    // endless loop at 802E0D98 when one differs: no thread ever drew anything
    if(cart.cic==6106)
    {
        dword key=0x85u*0x260BCD5u+1;
        for(a=0x4F0;a<0x7AC;a+=4)
        {
            mem_write32(0xA0000000+(a-0x4F0),mem_read32(0x10000000+a)^key);
            key*=0x260BCD5u;
        }
    }

    // what IPL3 leaves in low memory, HLE and LLE alike (decompals N64-IPL
    // ipl3.s): osTvType from the PIF (Mupen64Plus get_tv_type, by the header
    // country code), osVersion 0 as in Mupen64Plus, osCicId only from the
    // 6103 and 6105 IPL3s
    {
        int tv=1; // NTSC
        switch(mem_read8(0x1000003E))
        {
        case 'D': case 'F': case 'I': case 'P':
        case 'S': case 'U': case 'X': case 'Y': tv=0; break; // PAL
        case 'B': tv=2; break;                                // MPAL
        }
        mem_write32(0x80000300,tv);         // osTvType
    }
    mem_write32(0x80000304,0);          // osRomType: cartridge
    mem_write32(0x80000308,0xB0000000); // osRomBase
    mem_write32(0x8000030C,0);          // osResetType: cold
    mem_write32(0x80000310,cart.cic==6106?6104:(cart.cic==6103 || cart.cic==6105)?cart.cic:0); // osCicId
    mem_write32(0x80000314,0);          // osVersion
    mem_write32(0x80000318,mem.ramsize); // osMemSize

    // The 6105 boot also leaves two of IPL3's own instructions high in
    // RDRAM (its RSP program's DMAs), and libultra's boot RAM tests read
    // them back (osBootRamTest1/2_6105 in the Jet Force Gemini decomp;
    // Daedalus writes the second for Donkey Kong 64). Without them
    // Banjo-Tooie took itself for a copy: the right intro map loaded, but
    // its cutscene never ran
    if(cart.cic==6105)
    {
        mem_write32(0xA02FB1F4,0xAD090010); // sw t1,0x10(t0)  osCicId
        mem_write32(0xA02FE1C0,0xAD170014); // sw s7,0x14(t0)  osVersion
    }

    // IPL3 clears the RSP memory before jumping to the game (N64-IPL ipl3.s;
    // the 6106 part is XOR-encrypted there, decrypted with seed 0x85): 6103
    // and 6106 fill DMEM and IMEM with -1 and put a CIC number in IMEM[0],
    // 6105 fills both with 0xA4002000, the others with 0. Yoshi's Story
    // checks for DMEM[0]==-1 and IMEM[0]==6104 and never starts without them
    {
        dword fill=0,imem0;
        switch(cart.cic)
        {
        case 6103: fill=0xFFFFFFFF; break;
        case 6105: fill=0xA4002000; break;
        case 6106: fill=0xFFFFFFFF; break;
        }
        for(a=0;a<0x2000;a+=4) mem_write32(DMEM_ADDRESS+a,fill);
        imem0=cart.cic==6103?6103:cart.cic==6106?6104:fill;
        mem_write32(IMEM_ADDRESS,imem0);
    }

    // the RI as IPL3 leaves it after initializing RDRAM (Mupen64Plus's boot
    // without IPL3). libdragon's own IPL3 (Flappy Bird) redoes the whole
    // RDRAM init when RI_SELECT reads 0, which needs RDRAM registers
    // UltraHLE doesn't have, and then measured 0 bytes of memory
    RRI[0]=0x0E;       // RI_MODE
    RRI[1]=0x40;       // RI_CONFIG
    RRI[3]=0x14;       // RI_SELECT
    RRI[4]=0x00063634; // RI_REFRESH

    // copy pif rom
#ifdef PIFROM
    memcpy(RPIF,pifRomImage,0x7c0);
    memcpy(WPIF,pifRomImage,0x7c0);
#endif

    cart.codebase=mem_read32(0x10000008);
    cart.codesize=0x100000; // guess, always same?

    // CIC-6103 boot code (Paper Mario, Banjo-Kazooie) loads the game 1MB
    // below the header entry point, CIC-6106 (F-Zero X, Yoshi's Story,
    // Cruis'n World) 2MB below it
    print("CIC-%i. ",cart.cic);
    if(cart.cic==6103) cart.codebase-=0x100000;
    if(cart.cic==6106) cart.codebase-=0x200000;

    if(cart.codesize>4096*1024)
    {
        print("error: codeblock too large");
        return;
    }

    if(mem_read32(0x10000540)!=0) cart.bootloader=1;

    if(cart.bootloader==1)
    {
        print("Alternate boot loader. ");
        // an IPL3 the sum above doesn't know (PAL chips): the old guess,
        // right while the game loads below 1MB. A known chip has its exact
        // offset: Cruis'n World's header says 804AD400, which the mask left
        // alone, above the 4MB of RDRAM, and the game never started
        if(cart.cic!=6103 && cart.cic!=6106) cart.codebase&=~0x300000;
    }

    uint32_t pc = 0;

    if(0)
    { // Load IPL3 into DMEM and see what happens :)  (IPL1 and IPL2 are skipped because they require a PIF-ROM)
        mem_writerangeraw(DMEM_ADDRESS+0x40,0xfc0,cart.data+0x40);
        pc = DMEM_ADDRESS + 0x40;
        RA.q = (qword)(qint)(int)0xA0001000;
    }
    else
    { // C-Boot: IPL3 copies 1MB after the header; a smaller ROM (libdragon
      // homebrew, Flappy Bird is 256K) only has what it has, and reading
      // cart.data past its end crashed UltraHLE right after this point
        int n=cart.codesize;
        if(n>cart.size-0x1000) n=(cart.size-0x1000)&~3;
        if(n>0) mem_writerangeraw(cart.codebase,n,cart.data+0x1000);
        pc = cart.codebase;
    }

    cpu_goto(pc);
    sym_load(cart.symname);

    st.framesync=cart.framesync;

    view.codebase=pc;
    view_changed(VIEW_CODE);
}

void boot(char *cartname,int nomemmap)
{
    view.hidestuff=1; view_changed(VIEW_RESIZE); flushdisplay();

    st.pc=0; // pc displayed in exceptions generated by load

    view_status("loading rom");
    flushdisplay();

    if(!cartname) cart_dummy();
    else cart_open(cartname,!nomemmap);

    reset();
}

void reset(void)
{
    view_status("booting rom");
    flushdisplay();

    view_status("loading ultra.ini");
    inifile_read(cart.title);

    boot_boot();

    // have to load a second time... boot_boot overwrites some stuff
    // should be fixed
    view_status("loading ultra.ini");
    inifile_read(cart.title);

    view.hidestuff=0; view_changed(VIEW_RESIZE); flushdisplay();

    pif_reset(); // this rom's Controller Pak image
    flash_reset(); // and FlashRAM image

    sym_findfirstos();
    lle_decide(sym_important,sym_importanttotal,inifile_hasrescan());
    if(cart.isdocalls)
    {
        print("Creating OSCALLS list.\n");
        sym_demooscalls(); // generate oscalls
    }
    else if(!st.lleos)
    {
        sym_addpatches();
    }

    inifile_patches(0);
}

