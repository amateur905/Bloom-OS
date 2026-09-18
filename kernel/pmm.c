#include "pmm.h"

#define PAGE_SIZE 4096
#define EFI_CONVENTIONAL_MEMORY 7
#define EFI_MEMORY_MAPPED_IO 11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12
#define EFI_PAL_CODE 13

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEM_DESC;

static uint8_t *Bitmap;
static uint64_t BitmapPages;
static uint64_t TotalPages;
static uint64_t FreePages;

static void
SetBit(uint64_t Page)
{
    Bitmap[Page / 8] |= (uint8_t)(1 << (Page % 8));
}

static void
ClearBit(uint64_t Page)
{
    Bitmap[Page / 8] &= (uint8_t)~(1 << (Page % 8));
}

static int
TestBit(uint64_t Page)
{
    return (Bitmap[Page / 8] >> (Page % 8)) & 1;
}

static int
IsBackedRam(uint32_t Type)
{
    switch (Type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 9:
    case 10:
    case 14:
        return 1;
    default:
        return 0;
    }
}

void
InitPmm(BOOT_INFO *Info)
{
    uint64_t HighestAddress = 0;
    uint64_t EntryCount = Info->MemoryMapSize / Info->MemoryMapDescriptorSize;
    uint8_t *MapBytes = (uint8_t *)(uintptr_t)Info->MemoryMapBase;

    for (uint64_t i = 0; i < EntryCount; i++) {
        EFI_MEM_DESC *Desc = (EFI_MEM_DESC *)(MapBytes + i * Info->MemoryMapDescriptorSize);
        if (!IsBackedRam(Desc->Type)) {
            continue;
        }
        uint64_t End = Desc->PhysicalStart + Desc->NumberOfPages * PAGE_SIZE;
        if (End > HighestAddress) {
            HighestAddress = End;
        }
    }

    TotalPages = HighestAddress / PAGE_SIZE;
    uint64_t BitmapSize = (TotalPages + 7) / 8;
    BitmapPages = (BitmapSize + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t BitmapPhysAddr = 0;
    for (uint64_t i = 0; i < EntryCount; i++) {
        EFI_MEM_DESC *Desc = (EFI_MEM_DESC *)(MapBytes + i * Info->MemoryMapDescriptorSize);
        if (Desc->Type == EFI_CONVENTIONAL_MEMORY && Desc->NumberOfPages >= BitmapPages) {
            BitmapPhysAddr = Desc->PhysicalStart;
            break;
        }
    }

    Bitmap = (uint8_t *)(uintptr_t)BitmapPhysAddr;

    for (uint64_t i = 0; i < TotalPages; i++) {
        SetBit(i);
    }

    FreePages = 0;

    for (uint64_t i = 0; i < EntryCount; i++) {
        EFI_MEM_DESC *Desc = (EFI_MEM_DESC *)(MapBytes + i * Info->MemoryMapDescriptorSize);
        if (Desc->Type != EFI_CONVENTIONAL_MEMORY) {
            continue;
        }
        uint64_t StartPage = Desc->PhysicalStart / PAGE_SIZE;
        for (uint64_t p = 0; p < Desc->NumberOfPages; p++) {
            ClearBit(StartPage + p);
            FreePages++;
        }
    }

    uint64_t BitmapStartPage = BitmapPhysAddr / PAGE_SIZE;
    for (uint64_t p = 0; p < BitmapPages; p++) {
        SetBit(BitmapStartPage + p);
        FreePages--;
    }
}

void *
AllocPage(void)
{
    for (uint64_t i = 0; i < TotalPages; i++) {
        if (!TestBit(i)) {
            SetBit(i);
            FreePages--;
            return (void *)(uintptr_t)(i * PAGE_SIZE);
        }
    }
    return (void *)0;
}

void *
AllocPageBelow4G(void)
{
    uint64_t Limit = 0x100000000ULL / PAGE_SIZE;
    if (Limit > TotalPages) {
        Limit = TotalPages;
    }

    for (uint64_t i = 0; i < Limit; i++) {
        if (!TestBit(i)) {
            SetBit(i);
            FreePages--;
            return (void *)(uintptr_t)(i * PAGE_SIZE);
        }
    }
    return (void *)0;
}

void
FreePage(void *Addr)
{
    uint64_t Page = (uint64_t)(uintptr_t)Addr / PAGE_SIZE;
    if (TestBit(Page)) {
        ClearBit(Page);
        FreePages++;
    }
}

uint64_t
GetFreePageCount(void)
{
    return FreePages;
}

uint64_t
GetTotalPageCount(void)
{
    return TotalPages;
}
