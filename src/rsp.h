// Everything about RSP emulation is here

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define DMEM_ADDRESS 0xa4000000
#define IMEM_ADDRESS 0xa4001000

#define DMEM_SIZE 0x1000		// bytes
#define IMEM_SIZE 0x1000		// bytes

// RSP state. DMEM and IMEM are the real SP memory: the CPU reads back what
// it wrote, SP DMA copies to and from them, and the RSP interpreter (cxd4,
// rsp_cxd4.c) runs from them. Stored as host-order 32-bit words like RDRAM.
// One 8K block, DMEM first: cxd4's SP DMA reaches IMEM as DMEM+0x1000.
typedef struct _SPState
{

	uint8_t dmem[DMEM_SIZE];
	uint8_t imem[IMEM_SIZE];

} SPState;

extern SPState sp;

int  rsp_init();

// SP DMA from the SP_MEM_ADDR/SP_DRAM_ADDR/length registers the CPU wrote
// (WSP[0..3]); toram=0: RDRAM -> SP memory (SP_RD_LEN), 1: SP_WR_LEN
void rsp_dma(int toram);

// LLE OS mode: a task UltraHLE has no HLE for. Runs the RSP interpreter from
// SP_PC until it halts; RSP interrupts go to the LLE MI (lle.c).
void rsp_run(void);
void rsp_runfor(int instructions); // rsp_run with that many instructions
int  rsp_pending(void);  // the RSP stopped polling SP_STATUS, run it again
int  rsp_sliced(void);   // it stopped at the end of its slice: run it next burst

// HLE OS mode: runs an OSTask (16 host-order words as in RDRAM) that has no
// HLE on the RSP interpreter, to completion
void rsp_runtask(const void *task);

// DP command list START..END the CPU or the RSP gave (DPC registers in RDP)
void rsp_dpclist(void);

#ifdef __cplusplus
};
#endif
