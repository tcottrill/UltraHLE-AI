// FlashRAM save chip at cart domain 2 (0x08000000), 128KB, as Paper Mario
// uses it. Command/status registers and PI DMA, after Mupen64Plus
// device/cart/flashram.c. Backed by <title>.fla in the save folder.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FLASH_BASE 0x08000000 // status register (read/write)
#define FLASH_CMD  0x08010000 // command register (write)
#define FLASH_SIZE 0x20000

void flash_reset(void);                          // at boot: load the image
int  flash_contains(dword phys);                 // phys in the chip's range?
void flash_hwwrite(dword phys);                  // CPU store to a register
void flash_dmatoram(dword cart,dword dram,int len);   // PI_WR_LEN
void flash_dmafromram(dword cart,dword dram,int len); // PI_RD_LEN
void flash_savestate(FILE *f1);                  // chip registers (the image is in the .fla)
void flash_loadstate(FILE *f1);

#ifdef __cplusplus
};
#endif
