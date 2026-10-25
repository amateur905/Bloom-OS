#pragma once

#include <stdint.h>

typedef int (*BLOCK_READ_FN)(uint64_t Lba, uint32_t Count, void *Buffer);
typedef int (*BLOCK_READ_EX_FN)(void *Context, uint64_t Lba, uint32_t Count, void *Buffer);

int BlockRegister(const char *Name, BLOCK_READ_FN ReadFn);
int BlockRegisterEx(const char *Name, BLOCK_READ_EX_FN ReadFn, void *Context, uint32_t SectorSize, uint64_t SectorCount, uint32_t MaxCount);
int BlockRegisterNext(const char *Prefix, BLOCK_READ_EX_FN ReadFn, void *Context, uint32_t SectorSize, uint64_t SectorCount, uint32_t MaxCount);
int BlockRemove(int Index);
int BlockFind(const char *Name);
int BlockDeviceCount(void);
const char *BlockDeviceName(int Index);
int BlockIsOnline(int Index);
uint32_t BlockSectorSize(int Index);
uint64_t BlockSectorCount(int Index);
uint64_t BlockSize512(int Index);
int BlockReadFrom(int Index, uint64_t Lba, uint32_t Count, void *Buffer);
