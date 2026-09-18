#pragma once

#include <stdint.h>

int NvmeInit(uint64_t Bar0);
int NvmeReadSectors(uint64_t Lba, uint32_t Count, void *Buffer);
