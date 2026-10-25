#pragma once

#include <stdint.h>
#include "fat32.h"

#define FS_OK 1
#define FS_ERR_NOVOLUME -1
#define FS_ERR_NOTFOUND -2
#define FS_ERR_NOTDIR -3
#define FS_ERR_IO -4
#define FS_ERR_PATH -5
#define FS_ERR_UNSUPPORTED -6

#define FS_PATH_MAX 512

typedef struct {
    int Volume;
    int IsDir;
    FAT_ENTRY Entry;
    char Path[FS_PATH_MAX];
} FS_NODE;

typedef int (*FS_LIST_CB)(const FAT_ENTRY *Entry, void *Context);

int FsInit(void);
int FsMountDevice(int Device);
int FsVolumeCount(void);
const char *FsVolumeName(int Index);
int FsVolumePartition(int Index);
int FsResolve(const char *Input, FS_NODE *Node);
int FsList(const FS_NODE *Dir, FS_LIST_CB Callback, void *Context);
uint32_t FsRead(const FS_NODE *File, uint32_t Offset, void *Buffer, uint32_t Length);
int FsChdir(const char *Input);
int FsHasCwd(void);
const char *FsCwdVolume(void);
const char *FsCwdPath(void);
const char *FsErrorText(int Code);
