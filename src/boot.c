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
    // below the header entry point
    print("CIC-%i. ",cart.cic);
    if(cart.cic==6103) cart.codebase-=0x100000;

    if(cart.codesize>4096*1024)
    {
        print("error: codeblock too large");
        return;
    }

    if(mem_read32(0x10000540)!=0) cart.bootloader=1;

    if(cart.bootloader==1)
    {
        print("Alternate boot loader. ");
        cart.codebase&=~0x300000; // fzero
    }

    uint32_t pc = 0;

    if(0)
    { // Load IPL3 into DMEM and see what happens :)  (IPL1 and IPL2 are skipped because they require a PIF-ROM)
        mem_writerangeraw(DMEM_ADDRESS+0x40,0xfc0,cart.data+0x40);
        pc = DMEM_ADDRESS + 0x40;
        RA.d = 0xA0001000;
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

