#include <stdint.h>
#include "usbmsc.h"
#include "blockdev.h"
#include "idt.h"
#include "klog.h"

#define MAX_LUNS 64
#define MAX_LUN_PER_DEVICE 8
#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u

typedef struct {
    int Used;
    int Alive;
    USB_DEVICE *Dev;
    uint8_t Lun;
    uint32_t SectorSize;
    uint64_t Sectors;
    int Block;
} MSC_LUN;

static MSC_LUN Luns[MAX_LUNS];
static uint32_t TagCounter = 1;

static void
Put32(uint8_t *P, uint32_t V)
{
    P[0] = (uint8_t)V;
    P[1] = (uint8_t)(V >> 8);
    P[2] = (uint8_t)(V >> 16);
    P[3] = (uint8_t)(V >> 24);
}

static uint32_t
Get32(const uint8_t *P)
{
    return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

static uint32_t
GetBe32(const uint8_t *P)
{
    return ((uint32_t)P[0] << 24) | ((uint32_t)P[1] << 16) | ((uint32_t)P[2] << 8) | (uint32_t)P[3];
}

static void
PutBe32(uint8_t *P, uint32_t V)
{
    P[0] = (uint8_t)(V >> 24);
    P[1] = (uint8_t)(V >> 16);
    P[2] = (uint8_t)(V >> 8);
    P[3] = (uint8_t)V;
}

static void
ResetRecovery(USB_DEVICE *D)
{
    KLog("USB: bulk-only reset recovery\n");
    XhciControl(D, 0x21, 0xFF, 0, D->Interface, 0, 0);
    Sleep(20);
    XhciClearHalt(D, 1);
    XhciClearHalt(D, 0);
}

static int
Bot(MSC_LUN *L, const uint8_t *Cdb, uint8_t CdbLen, void *Data, uint32_t Length, int In, uint32_t *Got)
{
    USB_DEVICE *D = L->Dev;
    uint8_t Cbw[31];
    uint8_t Csw[13];
    uint32_t Act = 0;
    uint32_t Tag = TagCounter++;

    for (int i = 0; i < 31; i++) {
        Cbw[i] = 0;
    }

    Put32(Cbw, CBW_SIG);
    Put32(Cbw + 4, Tag);
    Put32(Cbw + 8, Length);
    Cbw[12] = In ? 0x80 : 0x00;
    Cbw[13] = L->Lun;
    Cbw[14] = CdbLen;

    for (int i = 0; i < CdbLen && i < 16; i++) {
        Cbw[15 + i] = Cdb[i];
    }

    int St = XhciBulk(D, 0, Cbw, 31, 0);

    if (St == XHCI_STALL) {
        XhciClearHalt(D, 0);
        St = XhciBulk(D, 0, Cbw, 31, 0);
    }

    if (St != XHCI_OK) {
        ResetRecovery(D);
        return 0;
    }

    if (Got) {
        *Got = 0;
    }

    if (Length) {
        St = XhciBulk(D, In, Data, Length, &Act);

        if (Got) {
            *Got = Act;
        }

        if (St == XHCI_STALL) {
            XhciClearHalt(D, In);
        } else if (St != XHCI_OK) {
            ResetRecovery(D);
            return 0;
        }
    }

    St = XhciBulk(D, 1, Csw, 13, &Act);

    if (St == XHCI_STALL) {
        XhciClearHalt(D, 1);
        St = XhciBulk(D, 1, Csw, 13, &Act);
    }

    if (St != XHCI_OK || Act != 13 || Get32(Csw) != CSW_SIG || Get32(Csw + 4) != Tag) {
        ResetRecovery(D);
        return 0;
    }

    if (Csw[12] == 0) {
        return 1;
    }

    if (Csw[12] == 1) {
        return -1;
    }

    ResetRecovery(D);

    return 0;
}

static int
Sense(MSC_LUN *L, uint8_t *Key, uint8_t *Asc, uint8_t *Ascq)
{
    uint8_t Cdb[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t Buf[18];

    for (int i = 0; i < 18; i++) {
        Buf[i] = 0;
    }

    if (Bot(L, Cdb, 6, Buf, 18, 1, 0) != 1) {
        return 0;
    }

    *Key = Buf[2] & 0x0F;
    *Asc = Buf[12];
    *Ascq = Buf[13];

    return 1;
}

static int
ReadCapacity(MSC_LUN *L)
{
    uint8_t Cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t Buf[8];

    if (Bot(L, Cdb, 10, Buf, 8, 1, 0) != 1) {
        return 0;
    }

    uint32_t Last = GetBe32(Buf);
    L->SectorSize = GetBe32(Buf + 4);
    L->Sectors = (uint64_t)Last + 1;

    if (Last == 0xFFFFFFFFu) {
        uint8_t Cdb16[16] = {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
        uint8_t Big[32];

        if (Bot(L, Cdb16, 16, Big, 32, 1, 0) != 1) {
            return 0;
        }

        uint64_t Hi = GetBe32(Big);
        uint64_t Lo = GetBe32(Big + 4);

        L->Sectors = ((Hi << 32) | Lo) + 1;
        L->SectorSize = GetBe32(Big + 8);
    }

    return 1;
}

static int
MscRead(void *Context, uint64_t Lba, uint32_t Count, void *Buffer)
{
    MSC_LUN *L = (MSC_LUN *)Context;

    if (!L->Alive || Count == 0) {
        return 0;
    }

    uint32_t Length = Count * L->SectorSize;
    uint8_t Cdb[16];

    for (int i = 0; i < 16; i++) {
        Cdb[i] = 0;
    }

    uint8_t CdbLen;

    if (Lba + Count > 0xFFFFFFFFULL) {
        Cdb[0] = 0x88;
        PutBe32(Cdb + 2, (uint32_t)(Lba >> 32));
        PutBe32(Cdb + 6, (uint32_t)Lba);
        PutBe32(Cdb + 10, Count);
        CdbLen = 16;
    } else {
        Cdb[0] = 0x28;
        PutBe32(Cdb + 2, (uint32_t)Lba);
        Cdb[7] = (uint8_t)(Count >> 8);
        Cdb[8] = (uint8_t)Count;
        CdbLen = 10;
    }

    for (int Attempt = 0; Attempt < 3; Attempt++) {
        uint32_t Got = 0;
        int St = Bot(L, Cdb, CdbLen, Buffer, Length, 1, &Got);

        if (St == 1 && Got == Length) {
            return 1;
        }

        if (St == -1) {
            uint8_t Key = 0;
            uint8_t Asc = 0;
            uint8_t Ascq = 0;

            if (Sense(L, &Key, &Asc, &Ascq)) {
                KLog("USB: read failed, sense ");
                KLogHex(Key, 1);
                KLog("/");
                KLogHex(Asc, 2);
                KLog("/");
                KLogHex(Ascq, 2);
                KLog("\n");

                if (Key == 6) {
                    continue;
                }
            }

            return 0;
        }
    }

    return 0;
}

static int
ProbeLun(USB_DEVICE *D, uint8_t Lun, int *Block)
{
    int Slot = -1;

    for (int i = 0; i < MAX_LUNS; i++) {
        if (!Luns[i].Used) {
            Slot = i;
            break;
        }
    }

    if (Slot < 0) {
        KLog("USB: lun table full\n");
        return 0;
    }

    MSC_LUN *L = &Luns[Slot];
    L->Used = 1;
    L->Alive = 0;
    L->Dev = D;
    L->Lun = Lun;
    L->Block = -1;

    uint8_t Inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t Buf[36];
    int Ok = 0;

    for (int t = 0; t < 3 && !Ok; t++) {
        for (int i = 0; i < 36; i++) {
            Buf[i] = 0;
        }
        Ok = Bot(L, Inquiry, 6, Buf, 36, 1, 0) == 1;
    }

    if (!Ok) {
        KLog("USB: inquiry failed on lun ");
        KLogUInt(Lun);
        KLog("\n");
        L->Used = 0;
        return 0;
    }

    uint8_t Type = Buf[0] & 0x1F;

    if (Type != 0x00 && Type != 0x0E) {
        KLog("USB: lun ");
        KLogUInt(Lun);
        KLog(" is not a disk, type ");
        KLogHex(Type, 2);
        KLog("\n");
        L->Used = 0;
        return 0;
    }

    int Ready = 0;
    uint8_t Tur[6] = {0, 0, 0, 0, 0, 0};

    for (int t = 0; t < 25; t++) {
        int St = Bot(L, Tur, 6, 0, 0, 0, 0);

        if (St == 1) {
            Ready = 1;
            break;
        }

        uint8_t Key = 0;
        uint8_t Asc = 0;
        uint8_t Ascq = 0;

        if (St == -1 && Sense(L, &Key, &Asc, &Ascq) && Asc == 0x3A) {
            KLog("USB: lun ");
            KLogUInt(Lun);
            KLog(" has no medium\n");
            break;
        }

        Sleep(200);
    }

    if (!Ready) {
        L->Used = 0;
        return 0;
    }

    if (!ReadCapacity(L)) {
        KLog("USB: read capacity failed\n");
        L->Used = 0;
        return 0;
    }

    if (L->SectorSize != 512 && L->SectorSize != 1024 && L->SectorSize != 2048 && L->SectorSize != 4096) {
        KLog("USB: unsupported sector size ");
        KLogUInt(L->SectorSize);
        KLog("\n");
        L->Used = 0;
        return 0;
    }

    uint32_t MaxCount = XhciMaxBulk() / L->SectorSize;

    if (MaxCount == 0) {
        MaxCount = 1;
    }

    if (MaxCount > 1024) {
        MaxCount = 1024;
    }

    L->Alive = 1;

    int Index = BlockRegisterNext("usb", MscRead, L, L->SectorSize, L->Sectors, MaxCount);

    if (Index < 0) {
        KLog("USB: cannot register block device\n");
        L->Alive = 0;
        L->Used = 0;
        return 0;
    }

    L->Block = Index;

    KLog("USB: ");
    KLog(BlockDeviceName(Index));
    KLog(" lun ");
    KLogUInt(Lun);
    KLog(", sector ");
    KLogUInt(L->SectorSize);
    KLog(", sectors ");
    KLogUInt(L->Sectors);
    KLog("\n");

    *Block = Index;

    return 1;
}

static int
AttachDevice(USB_DEVICE *D, int *Blocks, int Max)
{
    uint8_t MaxLun = 0;
    uint8_t Tmp = 0;

    if (XhciControl(D, 0xA1, 0xFE, 0, D->Interface, &Tmp, 1) == XHCI_OK) {
        MaxLun = Tmp;
    }

    if (MaxLun >= MAX_LUN_PER_DEVICE) {
        MaxLun = MAX_LUN_PER_DEVICE - 1;
    }

    int Count = 0;

    for (uint8_t Lun = 0; Lun <= MaxLun; Lun++) {
        int Block = -1;

        if (ProbeLun(D, Lun, &Block)) {
            if (Count < Max) {
                Blocks[Count] = Block;
            }
            Count++;
        }
    }

    return Count;
}

static int
DetachDevice(USB_DEVICE *D)
{
    int Removed = 0;

    for (int i = 0; i < MAX_LUNS; i++) {
        if (Luns[i].Used && Luns[i].Dev == D) {
            if (Luns[i].Alive && Luns[i].Block >= 0) {
                BlockRemove(Luns[i].Block);
                Removed++;
            }
            Luns[i].Alive = 0;
            Luns[i].Used = 0;
        }
    }

    XhciRelease(D);

    return Removed;
}

int
UsbScan(int *NewBlocks, int MaxNew, int *Removed)
{
    *Removed = 0;

    if (XhciRescan() < 0) {
        return -1;
    }

    int Gone = 0;

    for (int i = 0; i < XhciDeviceCount(); i++) {
        USB_DEVICE *D = XhciDevice(i);

        if (D && D->Gone) {
            *Removed += DetachDevice(D);
            Gone = 1;
        }
    }

    if (Gone) {
        XhciRescan();
    }

    int Total = 0;

    for (int i = 0; i < XhciDeviceCount(); i++) {
        USB_DEVICE *D = XhciDevice(i);

        if (!D || !D->IsMass || D->Attached) {
            continue;
        }

        int Found = AttachDevice(D, NewBlocks + (Total < MaxNew ? Total : MaxNew), MaxNew > Total ? MaxNew - Total : 0);

        if (Found > 0) {
            D->Attached = 1;
        }

        Total += Found;
    }

    return Total > MaxNew ? MaxNew : Total;
}

int
UsbMscBlocks(const USB_DEVICE *Dev, char *Out, int Max)
{
    int Pos = 0;
    int Count = 0;

    Out[0] = 0;

    for (int i = 0; i < MAX_LUNS; i++) {
        if (!Luns[i].Used || !Luns[i].Alive || Luns[i].Dev != Dev) {
            continue;
        }

        const char *Name = BlockDeviceName(Luns[i].Block);

        if (Count > 0 && Pos < Max - 1) {
            Out[Pos++] = ' ';
        }

        for (int k = 0; Name[k] && Pos < Max - 1; k++) {
            Out[Pos++] = Name[k];
        }

        Out[Pos] = 0;
        Count++;
    }

    return Count;
}
