#include <stdint.h>
#include "part.h"
#include "blockdev.h"
#include "fat32.h"
#include "klog.h"
#include "pmm.h"

#define PAGE_BYTES 4096
#define PARTS_PER_PAGE (PAGE_BYTES / sizeof(PART_INFO))
#define MAX_PART_PAGES 64
#define MAX_SCANNED 256
#define MAX_EBR_CHAIN 4096
#define MAX_GPT_ENTRIES 16384

static PART_INFO *PartPages[MAX_PART_PAGES];
static int PartTotal;
static uint8_t *Sector;
static uint8_t Table[4096];
static uint8_t Scanned[MAX_SCANNED];

static PART_INFO *
PartAt(int Index)
{
    return &PartPages[Index / PARTS_PER_PAGE][Index % PARTS_PER_PAGE];
}

static int
ReadNative(int Device, uint64_t NativeLba, uint32_t Ratio, uint8_t *Dst)
{
    return BlockReadFrom(Device, NativeLba * Ratio, Ratio, Dst);
}

static uint32_t
Le32(const uint8_t *P)
{
    return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

static uint64_t
Le64(const uint8_t *P)
{
    return (uint64_t)Le32(P) | ((uint64_t)Le32(P + 4) << 32);
}

static int
MemEq(const uint8_t *A, const uint8_t *B, int Length)
{
    for (int i = 0; i < Length; i++) {
        if (A[i] != B[i]) {
            return 0;
        }
    }
    return 1;
}

static int
TextEq(const uint8_t *A, const char *B, int Length)
{
    for (int i = 0; i < Length; i++) {
        if (A[i] != (uint8_t)B[i]) {
            return 0;
        }
    }
    return 1;
}

static int
ProbeFs(int Device, uint64_t StartLba)
{
    if (!BlockReadFrom(Device, StartLba, 1, Sector)) {
        return FS_UNKNOWN;
    }

    if (TextEq(Sector + 3, "NTFS    ", 8)) {
        return FS_NTFS;
    }

    if (TextEq(Sector + 3, "EXFAT   ", 8)) {
        return FS_EXFAT;
    }

    if (TextEq(Sector + 3, "-FVE-FS-", 8)) {
        return FS_BITLOCKER;
    }

    if (FatBootSectorValid(Sector)) {
        return FS_FAT32;
    }

    if (Sector[510] == 0x55 && Sector[511] == 0xAA &&
        (TextEq(Sector + 54, "FAT16   ", 8) || TextEq(Sector + 54, "FAT12   ", 8) || TextEq(Sector + 54, "FAT     ", 8))) {
        return FS_FAT16;
    }

    if (BlockReadFrom(Device, StartLba + 2, 1, Sector) && Sector[56] == 0x53 && Sector[57] == 0xEF) {
        return FS_EXT;
    }

    return FS_UNKNOWN;
}

static void
MakeName(PART_INFO *P, int Device, int Number)
{
    const char *Base = BlockDeviceName(Device);
    int Pos = 0;

    while (Base[Pos] && Pos < 16) {
        P->Name[Pos] = Base[Pos];
        Pos++;
    }

    P->Name[Pos++] = 'p';

    char Digits[8];
    int N = 0;
    int Value = Number;

    do {
        Digits[N++] = (char)('0' + Value % 10);
        Value /= 10;
    } while (Value > 0 && N < 7);

    while (N > 0) {
        P->Name[Pos++] = Digits[--N];
    }

    P->Name[Pos] = 0;
}

static PART_INFO *
AddPart(int Device, int Number, uint64_t Start, uint64_t Count, int Scheme)
{
    int Page = PartTotal / PARTS_PER_PAGE;

    if (Page >= MAX_PART_PAGES) {
        return 0;
    }

    if (!PartPages[Page]) {
        PartPages[Page] = (PART_INFO *)AllocPage();

        if (!PartPages[Page]) {
            return 0;
        }
    }

    PART_INFO *P = PartAt(PartTotal++);

    P->Device = Device;
    P->Number = Number;
    P->StartLba = Start;
    P->SectorCount = Count;
    P->Scheme = Scheme;
    P->MbrType = 0;
    for (int i = 0; i < 16; i++) {
        P->TypeGuid[i] = 0;
    }
    MakeName(P, Device, Number);
    P->Fs = ProbeFs(Device, Start);

    return P;
}

static void
LogPart(const PART_INFO *P)
{
    KLog("PART: ");
    KLog(P->Name);
    KLog(" start ");
    KLogUInt(P->StartLba);
    KLog(" sectors ");
    KLogUInt(P->SectorCount);
    KLog(" type ");
    KLog(PartTypeName(P));
    KLog(" fs ");
    KLog(PartFsName(P->Fs));
    KLog("\n");
}

static int
ScanGpt(int Device, uint32_t Ratio)
{
    if (!ReadNative(Device, 1, Ratio, Table) || !TextEq(Table, "EFI PART", 8)) {
        return 0;
    }

    uint64_t EntriesLba = Le64(Table + 72);
    uint32_t EntryCount = Le32(Table + 80);
    uint32_t EntrySize = Le32(Table + 84);
    uint32_t NativeSize = Ratio * 512;

    if (EntrySize < 128 || EntrySize > NativeSize || (NativeSize % EntrySize) != 0 || EntryCount == 0 || EntryCount > MAX_GPT_ENTRIES) {
        return 0;
    }

    uint32_t PerSector = NativeSize / EntrySize;
    uint32_t Sectors = (EntryCount + PerSector - 1) / PerSector;
    int Found = 0;

    for (uint32_t s = 0; s < Sectors; s++) {
        if (!ReadNative(Device, EntriesLba + s, Ratio, Table)) {
            break;
        }

        for (uint32_t e = 0; e < PerSector; e++) {
            uint32_t Index = s * PerSector + e;
            const uint8_t *Entry = Table + e * EntrySize;

            if (Index >= EntryCount) {
                break;
            }

            int Empty = 1;
            for (int i = 0; i < 16; i++) {
                if (Entry[i] != 0) {
                    Empty = 0;
                    break;
                }
            }

            if (Empty) {
                continue;
            }

            uint64_t First = Le64(Entry + 32);
            uint64_t Last = Le64(Entry + 40);

            if (Last < First) {
                continue;
            }

            PART_INFO *P = AddPart(Device, (int)Index + 1, First * Ratio, (Last - First + 1) * Ratio, PART_SCHEME_GPT);

            if (!P) {
                return Found;
            }

            for (int i = 0; i < 16; i++) {
                P->TypeGuid[i] = Entry[i];
            }

            LogPart(P);
            Found++;
        }
    }

    return Found;
}

static int
ScanMbr(int Device, const uint8_t *Mbr, uint32_t Ratio)
{
    uint8_t Entries[64];
    for (int i = 0; i < 64; i++) {
        Entries[i] = Mbr[446 + i];
    }

    int Found = 0;
    int Logical = 5;

    for (int i = 0; i < 4; i++) {
        const uint8_t *E = Entries + i * 16;
        uint8_t Type = E[4];
        uint32_t Start = Le32(E + 8);
        uint32_t Count = Le32(E + 12);

        if (Type == 0 || Count == 0) {
            continue;
        }

        if (Type == 0x05 || Type == 0x0F || Type == 0x85) {
            uint64_t ExtStart = Start;
            uint64_t Ebr = Start;

            for (int n = 0; n < MAX_EBR_CHAIN; n++) {
                if (!ReadNative(Device, Ebr, Ratio, Sector) || Sector[510] != 0x55 || Sector[511] != 0xAA) {
                    break;
                }

                uint8_t Ebr0[16];
                uint8_t Ebr1[16];
                for (int k = 0; k < 16; k++) {
                    Ebr0[k] = Sector[446 + k];
                    Ebr1[k] = Sector[462 + k];
                }

                uint32_t LStart = Le32(Ebr0 + 8);
                uint32_t LCount = Le32(Ebr0 + 12);

                if (Ebr0[4] != 0 && LCount != 0) {
                    PART_INFO *P = AddPart(Device, Logical++, (Ebr + LStart) * Ratio, (uint64_t)LCount * Ratio, PART_SCHEME_MBR);
                    if (!P) {
                        return Found;
                    }
                    P->MbrType = Ebr0[4];
                    LogPart(P);
                    Found++;
                }

                uint32_t Next = Le32(Ebr1 + 8);

                if (Ebr1[4] == 0 || Next == 0 || ExtStart + Next <= Ebr) {
                    break;
                }

                Ebr = ExtStart + Next;
            }

            continue;
        }

        PART_INFO *P = AddPart(Device, i + 1, (uint64_t)Start * Ratio, (uint64_t)Count * Ratio, PART_SCHEME_MBR);

        if (!P) {
            return Found;
        }

        P->MbrType = Type;
        LogPart(P);
        Found++;
    }

    return Found;
}

int
PartScanDevice(int Device)
{
    if (Device < 0 || Device >= MAX_SCANNED || Device >= BlockDeviceCount() || Scanned[Device]) {
        return 0;
    }

    if (!Sector) {
        Sector = (uint8_t *)AllocPage();
        if (!Sector) {
            return 0;
        }
    }

    Scanned[Device] = 1;

    uint32_t Ratio = BlockSectorSize(Device) / 512;

    if (Ratio == 0) {
        Ratio = 1;
    }

    if (!BlockReadFrom(Device, 0, 1, Sector)) {
        KLog("PART: ");
        KLog(BlockDeviceName(Device));
        KLog(" sector 0 read failed\n");
        return 0;
    }

    uint8_t Mbr[512];
    for (int i = 0; i < 512; i++) {
        Mbr[i] = Sector[i];
    }

    if (FatBootSectorValid(Mbr)) {
        PART_INFO *P = AddPart(Device, 1, 0, BlockSize512(Device), PART_SCHEME_RAW);
        if (P) {
            LogPart(P);
            return 1;
        }
        return 0;
    }

    if (Mbr[510] != 0x55 || Mbr[511] != 0xAA) {
        KLog("PART: ");
        KLog(BlockDeviceName(Device));
        KLog(" no partition table\n");
        return 0;
    }

    if (Mbr[446 + 4] == 0xEE || Mbr[462 + 4] == 0xEE || Mbr[478 + 4] == 0xEE || Mbr[494 + 4] == 0xEE) {
        int Count = ScanGpt(Device, Ratio);
        if (Count == 0) {
            KLog("PART: ");
            KLog(BlockDeviceName(Device));
            KLog(" gpt header invalid\n");
        }
        return Count;
    }

    return ScanMbr(Device, Mbr, Ratio);
}

int
PartCount(void)
{
    return PartTotal;
}

const PART_INFO *
PartGet(int Index)
{
    if (Index < 0 || Index >= PartTotal) {
        return 0;
    }
    return PartAt(Index);
}

static const uint8_t GuidEfi[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const uint8_t GuidMsData[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
static const uint8_t GuidMsReserved[16] = {0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D, 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE};
static const uint8_t GuidWinRe[16] = {0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D, 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC};
static const uint8_t GuidLinuxFs[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
static const uint8_t GuidLinuxSwap[16] = {0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43, 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F};

const char *
PartTypeName(const PART_INFO *P)
{
    if (P->Scheme == PART_SCHEME_RAW) {
        return "Whole disk";
    }

    if (P->Scheme == PART_SCHEME_MBR) {
        switch (P->MbrType) {
        case 0x01: return "FAT12";
        case 0x04:
        case 0x06:
        case 0x0E: return "FAT16";
        case 0x0B:
        case 0x0C: return "FAT32";
        case 0x07: return "NTFS/exFAT";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0xEF: return "EFI System";
        default: return "MBR partition";
        }
    }

    if (MemEq(P->TypeGuid, GuidEfi, 16)) return "EFI System";
    if (MemEq(P->TypeGuid, GuidMsData, 16)) return "Microsoft basic data";
    if (MemEq(P->TypeGuid, GuidMsReserved, 16)) return "Microsoft reserved";
    if (MemEq(P->TypeGuid, GuidWinRe, 16)) return "Windows recovery";
    if (MemEq(P->TypeGuid, GuidLinuxFs, 16)) return "Linux filesystem";
    if (MemEq(P->TypeGuid, GuidLinuxSwap, 16)) return "Linux swap";

    return "Unknown type";
}

const char *
PartFsName(int Fs)
{
    switch (Fs) {
    case FS_FAT32: return "FAT32";
    case FS_FAT16: return "FAT12/16";
    case FS_NTFS: return "NTFS";
    case FS_EXFAT: return "exFAT";
    case FS_BITLOCKER: return "BitLocker";
    case FS_EXT: return "ext";
    default: return "unknown";
    }
}
