#include <stdint.h>
#include "fs.h"
#include "part.h"
#include "blockdev.h"
#include "klog.h"
#include "pmm.h"

#define PAGE_BYTES 4096
#define MAX_VOLUME_PAGES 64
#define MAX_COMPONENTS 128
#define NAME_SIZE 24

typedef struct {
    char Name[NAME_SIZE];
    int Partition;
    FAT_VOLUME Fat;
} FS_VOLUME;

#define VOLUMES_PER_PAGE (PAGE_BYTES / sizeof(FS_VOLUME))

static FS_VOLUME *VolumePages[MAX_VOLUME_PAGES];
static int VolumeTotal;
static int PartCursor;
static int CwdVolume = -1;
static char CwdPathText[FS_PATH_MAX] = "/";

static FS_VOLUME *
VolumeAt(int Index)
{
    return &VolumePages[Index / VOLUMES_PER_PAGE][Index % VOLUMES_PER_PAGE];
}

static FS_VOLUME *
VolumeNext(void)
{
    int Page = VolumeTotal / VOLUMES_PER_PAGE;

    if (Page >= MAX_VOLUME_PAGES) {
        return 0;
    }

    if (!VolumePages[Page]) {
        VolumePages[Page] = (FS_VOLUME *)AllocPage();

        if (!VolumePages[Page]) {
            return 0;
        }
    }

    return &VolumePages[Page][VolumeTotal % VOLUMES_PER_PAGE];
}

static char Components[MAX_COMPONENTS][FAT_NAME_MAX];
static int ComponentCount;

static char
LowerAscii(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static void
CopyText(char *Dst, const char *Src, int Max)
{
    int i = 0;
    while (Src[i] && i < Max - 1) {
        Dst[i] = Src[i];
        i++;
    }
    Dst[i] = 0;
}

static int
MountNewPartitions(void)
{
    int Added = 0;

    while (PartCursor < PartCount()) {
        int i = PartCursor++;
        const PART_INFO *P = PartGet(i);

        if (P->Fs != FS_FAT32) {
            continue;
        }

        FS_VOLUME *V = VolumeNext();

        if (!V) {
            KLog("FS: volume table full\n");
            break;
        }

        if (!FatMount(&V->Fat, P->Device, P->StartLba)) {
            KLog("FS: mount failed ");
            KLog(P->Name);
            KLog("\n");
            continue;
        }

        CopyText(V->Name, P->Name, NAME_SIZE);
        V->Partition = i;
        VolumeTotal++;
        Added++;

        KLog("FS: mounted ");
        KLog(P->Name);
        KLog(" as FAT32 read-only\n");
    }

    if (VolumeTotal > 0 && CwdVolume < 0) {
        CwdVolume = 0;
        CwdPathText[0] = '/';
        CwdPathText[1] = 0;
    }

    return Added;
}

int
FsInit(void)
{
    for (int d = 0; d < BlockDeviceCount(); d++) {
        PartScanDevice(d);
    }

    MountNewPartitions();

    return VolumeTotal;
}

int
FsMountDevice(int Device)
{
    PartScanDevice(Device);

    return MountNewPartitions();
}

int
FsVolumeCount(void)
{
    return VolumeTotal;
}

const char *
FsVolumeName(int Index)
{
    if (Index < 0 || Index >= VolumeTotal) {
        return "";
    }
    return VolumeAt(Index)->Name;
}

int
FsVolumePartition(int Index)
{
    if (Index < 0 || Index >= VolumeTotal) {
        return -1;
    }
    return VolumeAt(Index)->Partition;
}

static int
FindVolume(const char *Name, int Length)
{
    for (int v = 0; v < VolumeTotal; v++) {
        int i = 0;
        while (i < Length && VolumeAt(v)->Name[i] && LowerAscii(VolumeAt(v)->Name[i]) == LowerAscii(Name[i])) {
            i++;
        }
        if (i == Length && VolumeAt(v)->Name[i] == 0) {
            return v;
        }
    }
    return -1;
}

static int
PartitionExists(const char *Name, int Length)
{
    for (int p = 0; p < PartCount(); p++) {
        const PART_INFO *P = PartGet(p);
        int i = 0;
        while (i < Length && P->Name[i] && LowerAscii(P->Name[i]) == LowerAscii(Name[i])) {
            i++;
        }
        if (i == Length && P->Name[i] == 0) {
            return 1;
        }
    }
    return 0;
}

static void
PushComponent(const char *Text, int Length)
{
    if (ComponentCount >= MAX_COMPONENTS) {
        return;
    }

    if (Length > FAT_NAME_MAX - 1) {
        Length = FAT_NAME_MAX - 1;
    }

    for (int i = 0; i < Length; i++) {
        Components[ComponentCount][i] = Text[i];
    }
    Components[ComponentCount][Length] = 0;
    ComponentCount++;
}

static void
SplitInto(const char *Path)
{
    const char *P = Path;

    while (*P) {
        while (*P == '/' || *P == '\\') {
            P++;
        }

        const char *Start = P;
        while (*P && *P != '/' && *P != '\\') {
            P++;
        }

        int Length = (int)(P - Start);

        if (Length == 0) {
            continue;
        }

        if (Length == 1 && Start[0] == '.') {
            continue;
        }

        if (Length == 2 && Start[0] == '.' && Start[1] == '.') {
            if (ComponentCount > 0) {
                ComponentCount--;
            }
            continue;
        }

        PushComponent(Start, Length);
    }
}

static int
ParseInput(const char *Input, int *VolumeOut)
{
    const char *Colon = 0;

    for (const char *P = Input; *P && *P != '/' && *P != '\\'; P++) {
        if (*P == ':') {
            Colon = P;
            break;
        }
    }

    ComponentCount = 0;

    if (Colon) {
        int Length = (int)(Colon - Input);
        int V = FindVolume(Input, Length);

        if (V < 0) {
            return PartitionExists(Input, Length) ? FS_ERR_UNSUPPORTED : FS_ERR_NOVOLUME;
        }

        *VolumeOut = V;
        SplitInto(Colon + 1);
        return FS_OK;
    }

    if (Input[0] == '/' || Input[0] == '\\') {
        if (CwdVolume < 0) {
            return FS_ERR_NOVOLUME;
        }
        *VolumeOut = CwdVolume;
        SplitInto(Input);
        return FS_OK;
    }

    if (CwdVolume < 0) {
        return FS_ERR_NOVOLUME;
    }

    *VolumeOut = CwdVolume;
    SplitInto(CwdPathText);
    SplitInto(Input);
    return FS_OK;
}

int
FsResolve(const char *Input, FS_NODE *Node)
{
    int V = -1;
    int Status = ParseInput(Input, &V);

    if (Status != FS_OK) {
        return Status;
    }

    FAT_VOLUME *Fat = &VolumeAt(V)->Fat;

    Node->Volume = V;
    Node->IsDir = 1;
    Node->Entry.Name[0] = 0;
    Node->Entry.Attr = FAT_ATTR_DIRECTORY;
    Node->Entry.Size = 0;
    Node->Entry.Cluster = Fat->RootCluster;

    int PathPos = 0;
    Node->Path[0] = 0;

    for (int i = 0; i < ComponentCount; i++) {
        if (!Node->IsDir) {
            return FS_ERR_NOTDIR;
        }

        FAT_ENTRY Found;

        if (!FatLookup(Fat, Node->Entry.Cluster, Components[i], &Found)) {
            return FS_ERR_NOTFOUND;
        }

        Node->Entry = Found;
        Node->IsDir = (Found.Attr & FAT_ATTR_DIRECTORY) != 0;

        if (PathPos < FS_PATH_MAX - 2) {
            Node->Path[PathPos++] = '/';
            for (int k = 0; Found.Name[k] && PathPos < FS_PATH_MAX - 1; k++) {
                Node->Path[PathPos++] = Found.Name[k];
            }
        }
        Node->Path[PathPos] = 0;
    }

    if (PathPos == 0) {
        Node->Path[0] = '/';
        Node->Path[1] = 0;
    }

    return FS_OK;
}

int
FsList(const FS_NODE *Dir, FS_LIST_CB Callback, void *Context)
{
    if (!Dir->IsDir) {
        return FS_ERR_NOTDIR;
    }

    if (!FatListDir(&VolumeAt(Dir->Volume)->Fat, Dir->Entry.Cluster, Callback, Context)) {
        return FS_ERR_IO;
    }

    return FS_OK;
}

uint32_t
FsRead(const FS_NODE *File, uint32_t Offset, void *Buffer, uint32_t Length)
{
    if (File->IsDir) {
        return 0;
    }

    return FatReadAt(&VolumeAt(File->Volume)->Fat, &File->Entry, Offset, Buffer, Length);
}

int
FsChdir(const char *Input)
{
    static FS_NODE Node;
    int Status = FsResolve(Input, &Node);

    if (Status != FS_OK) {
        return Status;
    }

    if (!Node.IsDir) {
        return FS_ERR_NOTDIR;
    }

    CwdVolume = Node.Volume;
    CopyText(CwdPathText, Node.Path, FS_PATH_MAX);
    return FS_OK;
}

int
FsHasCwd(void)
{
    return CwdVolume >= 0;
}

const char *
FsCwdVolume(void)
{
    return CwdVolume >= 0 ? VolumeAt(CwdVolume)->Name : "";
}

const char *
FsCwdPath(void)
{
    return CwdPathText;
}

const char *
FsErrorText(int Code)
{
    switch (Code) {
    case FS_ERR_NOVOLUME: return "no such volume or no volume selected";
    case FS_ERR_NOTFOUND: return "no such file or directory";
    case FS_ERR_NOTDIR: return "not a directory";
    case FS_ERR_IO: return "read error";
    case FS_ERR_PATH: return "bad path";
    case FS_ERR_UNSUPPORTED: return "filesystem is not supported yet";
    default: return "error";
    }
}
