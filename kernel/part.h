#pragma once

#include <stdint.h>

#define PART_SCHEME_RAW 0
#define PART_SCHEME_MBR 1
#define PART_SCHEME_GPT 2

#define FS_UNKNOWN 0
#define FS_FAT32 1
#define FS_FAT16 2
#define FS_NTFS 3
#define FS_EXFAT 4
#define FS_BITLOCKER 5
#define FS_EXT 6

typedef struct {
    int Device;
    int Number;
    uint64_t StartLba;
    uint64_t SectorCount;
    int Scheme;
    uint8_t MbrType;
    uint8_t TypeGuid[16];
    char Name[24];
    int Fs;
} PART_INFO;

int PartScanDevice(int Device);
int PartCount(void);
const PART_INFO *PartGet(int Index);
const char *PartTypeName(const PART_INFO *Part);
const char *PartFsName(int Fs);
