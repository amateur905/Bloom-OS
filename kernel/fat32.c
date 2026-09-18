#include "fat32.h"
#include "blockdev.h"
#include "pmm.h"

#define DIR_ATTR_LONG_NAME 0x0F
#define FAT_EOC_MIN 0x0FFFFFF8

static uint16_t BytesPerSector;
static uint8_t SectorsPerCluster;
static uint16_t ReservedSectorCount;
static uint8_t NumFats;
static uint32_t FatSize32;
static uint32_t RootCluster;
static uint32_t FirstDataSector;
static uint32_t FatStartLba;

static uint8_t *ScratchSector;
static uint8_t *ClusterBuffer;

static uint16_t
ReadU16(const uint8_t *Buffer, int Offset)
{
    return (uint16_t)(Buffer[Offset] | (Buffer[Offset + 1] << 8));
}

static uint32_t
ReadU32(const uint8_t *Buffer, int Offset)
{
    return (uint32_t)(Buffer[Offset] |
        (Buffer[Offset + 1] << 8) |
        (Buffer[Offset + 2] << 16) |
        (Buffer[Offset + 3] << 24));
}

static uint32_t
ClusterToLba(uint32_t Cluster)
{
    return FirstDataSector + (Cluster - 2) * SectorsPerCluster;
}

static uint32_t
NextCluster(uint32_t Cluster)
{
    uint32_t FatByteOffset = Cluster * 4;
    uint32_t FatSectorIndex = FatByteOffset / BytesPerSector;
    uint32_t OffsetInSector = FatByteOffset % BytesPerSector;

    BlockReadSectors(FatStartLba + FatSectorIndex, 1, ScratchSector);

    return ReadU32(ScratchSector, (int)OffsetInSector) & 0x0FFFFFFF;
}

int
FatInit(void)
{
    ScratchSector = (uint8_t *)AllocPage();
    ClusterBuffer = (uint8_t *)AllocPage();

    if (!ScratchSector || !ClusterBuffer) {
        return 0;
    }

    if (!BlockReadSectors(0, 1, ScratchSector)) {
        return 0;
    }

    BytesPerSector = ReadU16(ScratchSector, 0x0B);
    SectorsPerCluster = ScratchSector[0x0D];
    ReservedSectorCount = ReadU16(ScratchSector, 0x0E);
    NumFats = ScratchSector[0x10];
    FatSize32 = ReadU32(ScratchSector, 0x24);
    RootCluster = ReadU32(ScratchSector, 0x2C);

    if (ScratchSector[510] != 0x55 || ScratchSector[511] != 0xAA) {
        return 0;
    }

    FatStartLba = ReservedSectorCount;
    FirstDataSector = ReservedSectorCount + (NumFats * FatSize32);

    return 1;
}

static int
NamesMatch(const uint8_t *DirEntryName, const char *ShortName)
{
    for (int i = 0; i < 11; i++) {
        if (DirEntryName[i] != (uint8_t)ShortName[i]) {
            return 0;
        }
    }

    return 1;
}

FAT_FILE
FatFindFile(const char *ShortName)
{
    FAT_FILE Result;
    Result.Found = 0;
    Result.Cluster = 0;
    Result.Size = 0;

    uint32_t Cluster = RootCluster;

    while (Cluster < FAT_EOC_MIN) {
        uint32_t Lba = ClusterToLba(Cluster);

        for (uint8_t s = 0; s < SectorsPerCluster; s++) {
            BlockReadSectors(Lba + s, 1, ClusterBuffer);

            for (int e = 0; e < 512; e += 32) {
                uint8_t FirstByte = ClusterBuffer[e];

                if (FirstByte == 0x00) {
                    return Result;
                }

                if (FirstByte == 0xE5) {
                    continue;
                }

                uint8_t Attr = ClusterBuffer[e + 11];

                if (Attr == DIR_ATTR_LONG_NAME) {
                    continue;
                }

                if (NamesMatch(&ClusterBuffer[e], ShortName)) {
                    uint16_t HighCluster = ReadU16(&ClusterBuffer[e], 20);
                    uint16_t LowCluster = ReadU16(&ClusterBuffer[e], 26);

                    Result.Found = 1;
                    Result.Cluster = ((uint32_t)HighCluster << 16) | LowCluster;
                    Result.Size = ReadU32(&ClusterBuffer[e], 28);

                    return Result;
                }
            }
        }

        Cluster = NextCluster(Cluster);
    }

    return Result;
}

int
FatReadFile(FAT_FILE File, void *Buffer, uint32_t MaxBytes)
{
    if (!File.Found) {
        return 0;
    }

    uint8_t *Out = (uint8_t *)Buffer;
    uint32_t Cluster = File.Cluster;
    uint32_t BytesRemaining = File.Size;
    uint32_t BytesWritten = 0;

    while (Cluster < FAT_EOC_MIN && BytesRemaining > 0) {
        uint32_t Lba = ClusterToLba(Cluster);

        for (uint8_t s = 0; s < SectorsPerCluster && BytesRemaining > 0; s++) {
            BlockReadSectors(Lba + s, 1, ScratchSector);

            uint32_t ChunkSize = BytesRemaining < BytesPerSector ? BytesRemaining : BytesPerSector;

            if (BytesWritten + ChunkSize > MaxBytes) {
                ChunkSize = MaxBytes - BytesWritten;
            }

            for (uint32_t i = 0; i < ChunkSize; i++) {
                Out[BytesWritten + i] = ScratchSector[i];
            }

            BytesWritten += ChunkSize;
            BytesRemaining -= (BytesRemaining < BytesPerSector ? BytesRemaining : BytesPerSector);

            if (BytesWritten >= MaxBytes) {
                return (int)BytesWritten;
            }
        }

        Cluster = NextCluster(Cluster);
    }

    return (int)BytesWritten;
}
