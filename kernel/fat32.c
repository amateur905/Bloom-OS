#include <stdint.h>
#include "fat32.h"
#include "blockdev.h"
#include "pmm.h"

#define FAT_EOC_MIN 0x0FFFFFF8
#define MAX_LFN_ENTRIES 20
#define MAX_CHAIN 1048576

static uint16_t
Le16(const uint8_t *P)
{
    return (uint16_t)(P[0] | (P[1] << 8));
}

static uint32_t
Le32(const uint8_t *P)
{
    return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

int
FatBootSectorValid(const uint8_t *S)
{
    if (S[510] != 0x55 || S[511] != 0xAA) {
        return 0;
    }

    uint8_t Spc = S[0x0D];

    uint16_t Bps = Le16(S + 0x0B);

    if (Bps != 512 && Bps != 1024 && Bps != 2048 && Bps != 4096) {
        return 0;
    }

    if (Spc == 0 || (Spc & (Spc - 1)) != 0 || Spc > 128) {
        return 0;
    }

    if (Le16(S + 0x0E) == 0 || S[0x10] == 0 || S[0x10] > 2) {
        return 0;
    }

    if (Le16(S + 0x11) != 0 || Le32(S + 0x24) == 0 || Le32(S + 0x2C) < 2) {
        return 0;
    }

    return 1;
}

int
FatMount(FAT_VOLUME *V, int Device, uint64_t StartLba)
{
    uint8_t *SectorBuffer = (uint8_t *)AllocPage();
    uint8_t *FatBuffer = (uint8_t *)AllocPage();

    if (!SectorBuffer || !FatBuffer) {
        if (SectorBuffer) {
            FreePage(SectorBuffer);
        }
        if (FatBuffer) {
            FreePage(FatBuffer);
        }
        return 0;
    }

    if (!BlockReadFrom(Device, StartLba, 1, SectorBuffer) || !FatBootSectorValid(SectorBuffer)) {
        FreePage(SectorBuffer);
        FreePage(FatBuffer);
        return 0;
    }

    const uint8_t *S = SectorBuffer;
    uint32_t Scale = (uint32_t)Le16(S + 0x0B) / 512;

    V->SectorBuffer = SectorBuffer;
    V->FatBuffer = FatBuffer;
    V->Device = Device;
    V->StartLba = StartLba;
    V->SectorsPerCluster = (uint16_t)(S[0x0D] * Scale);
    V->FatStart = (uint32_t)Le16(S + 0x0E) * Scale;
    V->FirstData = V->FatStart + (uint32_t)S[0x10] * Le32(S + 0x24) * Scale;
    V->RootCluster = Le32(S + 0x2C);
    V->FatBufferValid = 0;
    V->CursorFirst = 0;
    V->CursorIndex = 0;
    V->CursorCluster = 0;

    return 1;
}

static uint64_t
ClusterLba(const FAT_VOLUME *V, uint32_t Cluster)
{
    return V->StartLba + V->FirstData + (uint64_t)(Cluster - 2) * V->SectorsPerCluster;
}

static uint32_t
NextCluster(FAT_VOLUME *V, uint32_t Cluster)
{
    uint32_t ByteOffset = Cluster * 4;
    uint64_t Lba = V->StartLba + V->FatStart + ByteOffset / 512;

    if (!V->FatBufferValid || V->FatBufferLba != Lba) {
        if (!BlockReadFrom(V->Device, Lba, 1, V->FatBuffer)) {
            V->FatBufferValid = 0;
            return FAT_EOC_MIN;
        }
        V->FatBufferLba = Lba;
        V->FatBufferValid = 1;
    }

    return Le32(V->FatBuffer + (ByteOffset % 512)) & 0x0FFFFFFF;
}

static int
ClusterValid(uint32_t Cluster)
{
    return Cluster >= 2 && Cluster < FAT_EOC_MIN;
}

static uint8_t
ShortChecksum(const uint8_t *Name)
{
    uint8_t Sum = 0;

    for (int i = 0; i < 11; i++) {
        Sum = (uint8_t)(((Sum & 1) << 7) + (Sum >> 1) + Name[i]);
    }

    return Sum;
}

static char
LowerAscii(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int
NamesEqual(const char *A, const char *B)
{
    while (*A && *B) {
        if (LowerAscii(*A) != LowerAscii(*B)) {
            return 0;
        }
        A++;
        B++;
    }
    return *A == *B;
}

static void
BuildShortName(const uint8_t *E, char *Out)
{
    int Pos = 0;
    int LowerBase = (E[12] & 0x08) != 0;
    int LowerExt = (E[12] & 0x10) != 0;

    for (int i = 0; i < 8 && E[i] != ' '; i++) {
        char c = (char)E[i];
        if (i == 0 && E[0] == 0x05) {
            c = (char)0xE5;
        }
        Out[Pos++] = LowerBase ? LowerAscii(c) : c;
    }

    if (E[8] != ' ') {
        Out[Pos++] = '.';
        for (int i = 8; i < 11 && E[i] != ' '; i++) {
            char c = (char)E[i];
            Out[Pos++] = LowerExt ? LowerAscii(c) : c;
        }
    }

    Out[Pos] = 0;
}

static const uint8_t LfnOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

int
FatListDir(FAT_VOLUME *V, uint32_t DirCluster, FAT_DIR_CB Callback, void *Context)
{
    static uint16_t Lfn[MAX_LFN_ENTRIES * 13];
    static FAT_ENTRY Entry;

    int LfnValid = 0;
    uint8_t LfnSum = 0;
    int LfnCount = 0;
    uint32_t Cluster = DirCluster;
    uint32_t Guard = 0;

    while (ClusterValid(Cluster) && Guard++ < MAX_CHAIN) {
        for (uint32_t s = 0; s < V->SectorsPerCluster; s++) {
            if (!BlockReadFrom(V->Device, ClusterLba(V, Cluster) + s, 1, V->SectorBuffer)) {
                return 0;
            }

            for (int e = 0; e < 16; e++) {
                const uint8_t *E = V->SectorBuffer + e * 32;

                if (E[0] == 0x00) {
                    return 1;
                }

                if (E[0] == 0xE5) {
                    LfnValid = 0;
                    continue;
                }

                if (E[11] == 0x0F) {
                    int Seq = E[0] & 0x1F;

                    if (E[0] & 0x40) {
                        LfnValid = 1;
                        LfnSum = E[13];
                        LfnCount = Seq;
                        for (int i = 0; i < MAX_LFN_ENTRIES * 13; i++) {
                            Lfn[i] = 0xFFFF;
                        }
                    }

                    if (LfnValid && Seq >= 1 && Seq <= MAX_LFN_ENTRIES && E[13] == LfnSum) {
                        for (int k = 0; k < 13; k++) {
                            Lfn[(Seq - 1) * 13 + k] = Le16(E + LfnOffsets[k]);
                        }
                    } else {
                        LfnValid = 0;
                    }
                    continue;
                }

                if (E[11] & 0x08) {
                    LfnValid = 0;
                    continue;
                }

                if (LfnValid && ShortChecksum(E) == LfnSum) {
                    int Pos = 0;
                    int Max = LfnCount * 13;
                    if (Max > MAX_LFN_ENTRIES * 13) {
                        Max = MAX_LFN_ENTRIES * 13;
                    }
                    for (int i = 0; i < Max && Pos < FAT_NAME_MAX - 1; i++) {
                        if (Lfn[i] == 0x0000 || Lfn[i] == 0xFFFF) {
                            break;
                        }
                        Entry.Name[Pos++] = Lfn[i] < 0x80 ? (char)Lfn[i] : '?';
                    }
                    Entry.Name[Pos] = 0;
                } else {
                    BuildShortName(E, Entry.Name);
                }

                LfnValid = 0;

                if (Entry.Name[0] == '.' && (Entry.Name[1] == 0 || (Entry.Name[1] == '.' && Entry.Name[2] == 0))) {
                    continue;
                }

                Entry.Attr = E[11];
                Entry.Size = Le32(E + 28);
                Entry.Cluster = ((uint32_t)Le16(E + 20) << 16) | Le16(E + 26);

                if (Callback(&Entry, Context)) {
                    return 1;
                }
            }
        }

        Cluster = NextCluster(V, Cluster);
    }

    return 1;
}

typedef struct {
    const char *Want;
    FAT_ENTRY *Out;
    int Found;
} LOOKUP_CONTEXT;

static int
LookupCallback(const FAT_ENTRY *Entry, void *Context)
{
    LOOKUP_CONTEXT *Ctx = (LOOKUP_CONTEXT *)Context;

    if (NamesEqual(Entry->Name, Ctx->Want)) {
        *Ctx->Out = *Entry;
        Ctx->Found = 1;
        return 1;
    }

    return 0;
}

int
FatLookup(FAT_VOLUME *V, uint32_t DirCluster, const char *Name, FAT_ENTRY *Out)
{
    LOOKUP_CONTEXT Ctx;
    Ctx.Want = Name;
    Ctx.Out = Out;
    Ctx.Found = 0;

    FatListDir(V, DirCluster, LookupCallback, &Ctx);

    return Ctx.Found;
}

uint32_t
FatReadAt(FAT_VOLUME *V, const FAT_ENTRY *File, uint32_t Offset, void *Buffer, uint32_t Length)
{
    if ((File->Attr & FAT_ATTR_DIRECTORY) || Offset >= File->Size || !ClusterValid(File->Cluster)) {
        return 0;
    }

    if (Length > File->Size - Offset) {
        Length = File->Size - Offset;
    }

    uint32_t ClusterBytes = (uint32_t)V->SectorsPerCluster * 512;
    uint32_t WantIndex = Offset / ClusterBytes;
    uint32_t Index = 0;
    uint32_t Cluster = File->Cluster;

    if (V->CursorFirst == File->Cluster && V->CursorIndex <= WantIndex && ClusterValid(V->CursorCluster)) {
        Index = V->CursorIndex;
        Cluster = V->CursorCluster;
    }

    while (Index < WantIndex) {
        Cluster = NextCluster(V, Cluster);
        if (!ClusterValid(Cluster)) {
            return 0;
        }
        Index++;
    }

    uint8_t *Out = (uint8_t *)Buffer;
    uint32_t Done = 0;

    while (Done < Length) {
        uint32_t InCluster = (Offset + Done) % ClusterBytes;
        uint32_t SectorInCluster = InCluster / 512;
        uint32_t SectorOffset = InCluster % 512;
        uint32_t SectorsLeft = V->SectorsPerCluster - SectorInCluster;
        uint32_t Chunk = SectorsLeft > 8 ? 8 : SectorsLeft;

        if (!BlockReadFrom(V->Device, ClusterLba(V, Cluster) + SectorInCluster, Chunk, V->SectorBuffer)) {
            break;
        }

        uint32_t Available = Chunk * 512 - SectorOffset;
        uint32_t Take = Length - Done < Available ? Length - Done : Available;

        for (uint32_t i = 0; i < Take; i++) {
            Out[Done + i] = V->SectorBuffer[SectorOffset + i];
        }

        Done += Take;

        if (Done < Length && (Offset + Done) % ClusterBytes == 0) {
            uint32_t Next = NextCluster(V, Cluster);
            if (!ClusterValid(Next)) {
                break;
            }
            Cluster = Next;
            Index++;
        }
    }

    V->CursorFirst = File->Cluster;
    V->CursorIndex = Index;
    V->CursorCluster = Cluster;

    return Done;
}
