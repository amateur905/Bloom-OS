#pragma once

#include <stdint.h>

typedef int (*BLOCK_READ_FN)(uint64_t Lba, uint32_t Count, void *Buffer);

void RegisterBlockDevice(BLOCK_READ_FN ReadFn, const char *Name);
int BlockDeviceReady(void);
int BlockReadSectors(uint64_t Lba, uint32_t Count, void *Buffer);
const char *BlockDeviceName(void);
