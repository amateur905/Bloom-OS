#pragma once

#include <stdint.h>

#define FAT_NAME_MAX 256

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_DIRECTORY 0x10

typedef struct {
    char Name[FAT_NAME_MAX];
    uint8_t Attr;
    uint32_t Size;
    uint32_t Cluster;
} FAT_ENTRY;

typedef struct {
    int Device;
    uint64_t StartLba;
    uint16_t SectorsPerCluster;
    uint32_t FatStart;
    uint32_t FirstData;
    uint32_t RootCluster;
    uint8_t *SectorBuffer;
    uint8_t *FatBuffer;
    uint64_t FatBufferLba;
    int FatBufferValid;
    uint32_t CursorFirst;
    uint32_t CursorIndex;
    uint32_t CursorCluster;
} FAT_VOLUME;

typedef int (*FAT_DIR_CB)(const FAT_ENTRY *Entry, void *Context);

int FatBootSectorValid(const uint8_t *Sector);
int FatMount(FAT_VOLUME *Volume, int Device, uint64_t StartLba);
int FatListDir(FAT_VOLUME *Volume, uint32_t DirCluster, FAT_DIR_CB Callback, void *Context);
int FatLookup(FAT_VOLUME *Volume, uint32_t DirCluster, const char *Name, FAT_ENTRY *Out);
uint32_t FatReadAt(FAT_VOLUME *Volume, const FAT_ENTRY *File, uint32_t Offset, void *Buffer, uint32_t Length);
