// FlashRAM save chip (see flash.h). Follows Mupen64Plus flashram.c: writes
// to 0x08010000 are commands, 0x08000000 is the status register in status
// mode, PI DMA reads the silicon ID or the array and loads the 128-byte
// page buffer. Chip MX29L1100 like Mupen64Plus: array DMA starts at twice
// the cart offset. The image is kept in N64 byte order.
//
// SRAM lives at the same address. Mupen64Plus picks one from its rom
// database; here the game's first access decides: FlashRAM code always
// writes a command before its first DMA, SRAM code just DMAs. SRAM as in
// Mupen64Plus sram.c: 32KB, erased 0xff, a .sra file in N64 byte order.

#include "ultra.h"
#include "flash.h"
#include <stdio.h>

#define FLASH_TYPE_ID 0x11118001
#define FLASH_CHIP_ID 0x00c2001e // MX29L1100
#define SRAM_SIZE     0x8000

enum { MODE_READARRAY,MODE_SILICONID,MODE_STATUS,MODE_SECTORERASE,
       MODE_CHIPERASE,MODE_PAGEPROGRAM };

static byte  flash[FLASH_SIZE];
static byte  pagebuf[128];
static dword status;
static dword erasepage;
static int   mode;
static char  flashfile[MAXFILE+48];

static byte  sram[SRAM_SIZE];
static char  sramfile[MAXFILE+48];
static int   savetype; // 0 not yet known, 1 FlashRAM, -1 SRAM

static void sram_save(void)
{
    FILE *f=fopen(sramfile,"wb");
    if(!f) { warning("sram: can't write %s",sramfile); return; }
    fwrite(sram,1,sizeof(sram),f);
    fclose(f);
}

// the first DMA before any FlashRAM command: this cart has SRAM
static int issram(void)
{
    if(!savetype)
    {
        savetype=-1;
        print("note: save chip is SRAM\n");
    }
    return(savetype<0);
}

// the status register reads back only in status mode (0 otherwise, the
// dummy read before a DMA)
static void flash_updateread(void)
{
    mem.io[IO_FLASH][IO_R][0]=(mode==MODE_STATUS)?status:0;
}

static void flash_save(void)
{
    FILE *f=fopen(flashfile,"wb");
    if(!f) { warning("flash: can't write %s",flashfile); return; }
    fwrite(flash,1,sizeof(flash),f);
    fclose(f);
}

void flash_reset(void)
{
    FILE *f;

    mode=MODE_READARRAY;
    status=0;
    erasepage=0;
    memset(pagebuf,0xff,sizeof(pagebuf));
    flash_updateread();

    pif_savename(flashfile,"fla");
    memset(flash,0xff,sizeof(flash)); // erased; the file is created on the first write
    f=fopen(flashfile,"rb");
    if(f)
    {
        if(fread(flash,1,sizeof(flash),f)!=sizeof(flash)) warning("flash: %s is short",flashfile);
        fclose(f);
    }

    savetype=0;
    pif_savename(sramfile,"sra");
    memset(sram,0xff,sizeof(sram)); // erased; the file is created on the first write
    f=fopen(sramfile,"rb");
    if(f)
    {
        if(fread(sram,1,sizeof(sram),f)!=sizeof(sram)) warning("sram: %s is short",sramfile);
        fclose(f);
    }
}

void flash_savestate(FILE *f1)
{
    fwrite(pagebuf,1,sizeof(pagebuf),f1);
    fwrite(&status,1,4,f1);
    fwrite(&erasepage,1,4,f1);
    fwrite(&mode,1,4,f1);
}

void flash_loadstate(FILE *f1)
{
    fread(pagebuf,1,sizeof(pagebuf),f1);
    fread(&status,1,4,f1);
    fread(&erasepage,1,4,f1);
    fread(&mode,1,4,f1);
    flash_updateread();
}

int flash_contains(dword phys)
{
    phys&=0x1fffffff;
    return(phys>=FLASH_BASE && phys<FLASH_BASE+0x20000);
}

static void flash_command(dword cmd)
{
    dword off;
    switch(cmd&0xff000000)
    {
    case 0x3c000000: mode=MODE_CHIPERASE; break;
    case 0x4b000000: mode=MODE_SECTORERASE; erasepage=cmd&0xffff; break;
    case 0x78000000: // erase
        if(mode==MODE_SECTORERASE)
        {
            off=(erasepage&0xff80)*128;
            if(off+128*128<=FLASH_SIZE) memset(flash+off,0xff,128*128);
        }
        else if(mode==MODE_CHIPERASE) memset(flash,0xff,sizeof(flash));
        else warning("flash: erase in mode %i",mode);
        flash_save();
        status=(status&~2u)|8; // erase done
        mode=MODE_STATUS;
        break;
    case 0xa5000000: // program the page buffer
        off=(cmd&0xffff)*128;
        if(off+128<=FLASH_SIZE) memcpy(flash+off,pagebuf,128);
        flash_save();
        status=(status&~1u)|4; // program done
        mode=MODE_STATUS;
        break;
    case 0xb4000000: mode=MODE_PAGEPROGRAM; break;
    case 0xd2000000: mode=MODE_STATUS; break;
    case 0xe1000000: mode=MODE_SILICONID; status|=1; break;
    case 0xf0000000: mode=MODE_READARRAY; break;
    default: warning("flash: unknown command %08X",cmd);
    }
    flash_updateread();
}

void flash_hwwrite(dword phys)
{
    phys&=0x1fffffff;
    if(!savetype) savetype=1; // a register write: FlashRAM
    if(phys==FLASH_BASE && mode==MODE_STATUS)
    {
        status=mem.io[IO_FLASH][IO_W][0]&0xff;
        flash_updateread();
    }
    else if(phys==FLASH_CMD)
    {
        flash_command(mem.io[IO_FLASHCMD][IO_W][0]);
    }
}

// cart -> RDRAM
void flash_dmatoram(dword cart,dword dram,int len)
{
    int i;
    if(issram())
    {
        cart&=0xffff;
        for(i=0;i<len && cart+i<SRAM_SIZE;i++) mem_write8(dram+i,sram[cart+i]);
        return;
    }
    cart&=0x1ffff;
    if(cart==0 && len==8 && mode==MODE_SILICONID)
    {
        mem_write32(dram,FLASH_TYPE_ID);
        mem_write32(dram+4,FLASH_CHIP_ID);
    }
    else if(cart<0x10000 && mode==MODE_READARRAY)
    {
        cart=(cart&0xffff)*2; // MX29L1100 addresses the array in halves
        for(i=0;i<len && cart+i<FLASH_SIZE;i++) mem_write8(dram+i,flash[cart+i]);
    }
    else warning("flash: DMA to RAM %05X len %X in mode %i",cart,len,mode);
}

// RDRAM -> cart
void flash_dmafromram(dword cart,dword dram,int len)
{
    int i;
    if(issram())
    {
        int changed=0;
        cart&=0xffff;
        for(i=0;i<len && cart+i<SRAM_SIZE;i++)
        {
            byte b=(byte)mem_read8(dram+i);
            if(sram[cart+i]!=b) { sram[cart+i]=b; changed=1; }
        }
        if(changed) sram_save();
        return;
    }
    cart&=0x1ffff;
    if(cart==0 && len==128 && mode==MODE_PAGEPROGRAM)
    {
        for(i=0;i<128;i++) pagebuf[i]=(byte)mem_read8(dram+i);
    }
    else warning("flash: DMA from RAM %05X len %X in mode %i",cart,len,mode);
}
