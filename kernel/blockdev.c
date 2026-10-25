#include <stdint.h>
#include "blockdev.h"

#define MAX_BLOCK_DEVICES 64
#define NAME_SIZE 16
#define BOUNCE_SIZE 4096
#define LEGACY_MAX_COUNT 8

typedef struct {
    BLOCK_READ_FN Legacy;
    BLOCK_READ_EX_FN Read;
    void *Context;
    uint32_t SectorSize;
    uint32_t MaxCount;
    uint64_t SectorCount;
    int Online;
    char Name[NAME_SIZE];
} BLOCK_DEVICE;

static BLOCK_DEVICE Devices[MAX_BLOCK_DEVICES];
static int DeviceCount;
static uint8_t Bounce[BOUNCE_SIZE] __attribute__((aligned(16)));

static int
ValidSectorSize(uint32_t Size)
{
    return Size == 512 || Size == 1024 || Size == 2048 || Size == 4096;
}

static void
CopyName(char *Dst, const char *Src)
{
    int i = 0;
    while (Src[i] && i < NAME_SIZE - 1) {
        Dst[i] = Src[i];
        i++;
    }
    Dst[i] = 0;
}

static int
NameEq(const char *A, const char *B)
{
    int i = 0;
    while (A[i] && A[i] == B[i]) {
        i++;
    }
    return A[i] == B[i];
}

static int
AddDevice(const char *Name, BLOCK_READ_FN Legacy, BLOCK_READ_EX_FN Read, void *Context, uint32_t SectorSize, uint64_t SectorCount, uint32_t MaxCount)
{
    if (DeviceCount >= MAX_BLOCK_DEVICES || !Name || !Name[0] || (!Legacy && !Read)) {
        return -1;
    }

    if (!ValidSectorSize(SectorSize)) {
        return -1;
    }

    char Local[NAME_SIZE];
    CopyName(Local, Name);

    if (BlockFind(Local) >= 0) {
        return -1;
    }

    if (MaxCount == 0) {
        MaxCount = SectorSize == 512 ? LEGACY_MAX_COUNT : 1;
    }

    BLOCK_DEVICE *D = &Devices[DeviceCount];
    D->Legacy = Legacy;
    D->Read = Read;
    D->Context = Context;
    D->SectorSize = SectorSize;
    D->SectorCount = SectorCount;
    D->MaxCount = MaxCount;
    D->Online = 1;
    CopyName(D->Name, Local);

    return DeviceCount++;
}

int
BlockRegister(const char *Name, BLOCK_READ_FN ReadFn)
{
    return AddDevice(Name, ReadFn, 0, 0, 512, 0, LEGACY_MAX_COUNT);
}

int
BlockRegisterEx(const char *Name, BLOCK_READ_EX_FN ReadFn, void *Context, uint32_t SectorSize, uint64_t SectorCount, uint32_t MaxCount)
{
    return AddDevice(Name, 0, ReadFn, Context, SectorSize, SectorCount, MaxCount);
}

int
BlockRegisterNext(const char *Prefix, BLOCK_READ_EX_FN ReadFn, void *Context, uint32_t SectorSize, uint64_t SectorCount, uint32_t MaxCount)
{
    if (!Prefix || !Prefix[0]) {
        return -1;
    }

    char Name[NAME_SIZE];
    int Len = 0;

    while (Prefix[Len] && Len < NAME_SIZE - 4) {
        Name[Len] = Prefix[Len];
        Len++;
    }

    for (int N = 0; N < 100; N++) {
        int Pos = Len;

        if (N >= 10) {
            Name[Pos++] = (char)('0' + N / 10);
        }
        Name[Pos++] = (char)('0' + N % 10);
        Name[Pos] = 0;

        if (BlockFind(Name) < 0) {
            return AddDevice(Name, 0, ReadFn, Context, SectorSize, SectorCount, MaxCount);
        }
    }

    return -1;
}

int
BlockRemove(int Index)
{
    if (Index < 0 || Index >= DeviceCount) {
        return 0;
    }
    Devices[Index].Online = 0;
    return 1;
}

int
BlockFind(const char *Name)
{
    if (!Name) {
        return -1;
    }

    for (int i = 0; i < DeviceCount; i++) {
        if (NameEq(Devices[i].Name, Name)) {
            return i;
        }
    }

    return -1;
}

int
BlockDeviceCount(void)
{
    return DeviceCount;
}

const char *
BlockDeviceName(int Index)
{
    if (Index < 0 || Index >= DeviceCount) {
        return "none";
    }
    return Devices[Index].Name;
}

int
BlockIsOnline(int Index)
{
    if (Index < 0 || Index >= DeviceCount) {
        return 0;
    }
    return Devices[Index].Online;
}

uint32_t
BlockSectorSize(int Index)
{
    if (Index < 0 || Index >= DeviceCount) {
        return 0;
    }
    return Devices[Index].SectorSize;
}

uint64_t
BlockSectorCount(int Index)
{
    if (Index < 0 || Index >= DeviceCount) {
        return 0;
    }
    return Devices[Index].SectorCount;
}

uint64_t
BlockSize512(int Index)
{
    if (Index < 0 || Index >= DeviceCount) {
        return 0;
    }
    return Devices[Index].SectorCount * (Devices[Index].SectorSize / 512);
}

static int
DeviceRead(BLOCK_DEVICE *D, uint64_t Lba, uint32_t Count, void *Buffer)
{
    if (D->Legacy) {
        return D->Legacy(Lba, Count, Buffer);
    }
    return D->Read(D->Context, Lba, Count, Buffer);
}

int
BlockReadFrom(int Index, uint64_t Lba, uint32_t Count, void *Buffer)
{
    if (Index < 0 || Index >= DeviceCount || !Buffer) {
        return 0;
    }

    BLOCK_DEVICE *D = &Devices[Index];

    if (!D->Online) {
        return 0;
    }

    if (Count == 0) {
        return 1;
    }

    uint32_t Ratio = D->SectorSize / 512;

    if (D->SectorCount != 0) {
        uint64_t Total = D->SectorCount * Ratio;

        if (Lba >= Total || Count > Total - Lba) {
            return 0;
        }
    }

    uint8_t *Out = (uint8_t *)Buffer;

    if (Ratio == 1) {
        while (Count > 0) {
            uint32_t Chunk = Count > D->MaxCount ? D->MaxCount : Count;

            if (!DeviceRead(D, Lba, Chunk, Out)) {
                return 0;
            }

            Lba += Chunk;
            Out += (uint64_t)Chunk * 512;
            Count -= Chunk;
        }

        return 1;
    }

    while (Count > 0) {
        uint64_t Native = Lba / Ratio;
        uint32_t Offset = (uint32_t)(Lba % Ratio);

        if (Offset == 0 && Count >= Ratio) {
            uint32_t Whole = Count / Ratio;

            if (Whole > D->MaxCount) {
                Whole = D->MaxCount;
            }

            if (!DeviceRead(D, Native, Whole, Out)) {
                return 0;
            }

            uint32_t Done = Whole * Ratio;
            Lba += Done;
            Out += (uint64_t)Done * 512;
            Count -= Done;
        } else {
            if (!DeviceRead(D, Native, 1, Bounce)) {
                return 0;
            }

            uint32_t Take = Ratio - Offset;

            if (Take > Count) {
                Take = Count;
            }

            const uint8_t *Src = Bounce + (uint64_t)Offset * 512;
            uint32_t Bytes = Take * 512;

            for (uint32_t i = 0; i < Bytes; i++) {
                Out[i] = Src[i];
            }

            Lba += Take;
            Out += Bytes;
            Count -= Take;
        }
    }

    return 1;
}
