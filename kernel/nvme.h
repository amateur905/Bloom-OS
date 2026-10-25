#pragma once

#include <stdint.h>

int NvmeInit(uint64_t Bar0);
int NvmeReadSectors(uint64_t Lba, uint32_t Count, void *Buffer);

extern int NvmeDbgStage;
extern uint64_t NvmeDbgCap;
extern uint32_t NvmeDbgCsts;
extern uint32_t NvmeDbgStatus;
