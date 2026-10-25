#include <stdint.h>
#include "xhci.h"
#include "pci.h"
#include "pmm.h"
#include "idt.h"
#include "klog.h"

#define MAX_CONTROLLERS 4
#define RING_SIZE 256
#define EVT_SIZE 256
#define BOUNCE_PAGES 16
#define SCRATCH_CTRL 256
#define SCRATCH_CTRL_MAX 3840

#define PORTSC_CCS 0x00000001u
#define PORTSC_PED 0x00000002u
#define PORTSC_PR 0x00000010u
#define PORTSC_PP 0x00000200u
#define PORTSC_CSC 0x00020000u
#define PORTSC_WRC 0x00080000u
#define PORTSC_PRC 0x00200000u
#define PORTSC_WPR 0x80000000u
#define PORTSC_CHANGE 0x00FE0000u
#define PORTSC_KEEP 0x0E00C200u

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_EVALUATE_CTX 13
#define TRB_RESET_EP 14
#define TRB_STOP_EP 15
#define TRB_SET_DEQ 16
#define EVT_TRANSFER 32
#define EVT_COMMAND 33

typedef struct {
    uint64_t Ptr;
    uint32_t Status;
    uint32_t Control;
} EVENT;

typedef struct {
    int Used;
    volatile uint8_t *Cap;
    volatile uint8_t *Op;
    volatile uint8_t *Rt;
    volatile uint32_t *Db;
    uint32_t CtxSize;
    int MaxSlots;
    int MaxPorts;
    uint64_t *Dcbaa;
    XHCI_RING Cmd;
    volatile XHCI_TRB *Evt;
    uint64_t EvtPhys;
    uint32_t EvtDeq;
    uint32_t EvtCycle;
    uint8_t *Scratch;
    uint8_t *Bounce;
    uint32_t BounceSize;
    uint8_t PortMajor[256];
} XHCI;

static XHCI Ctrls[MAX_CONTROLLERS];
static int CtrlCount;
static int Probed;
static USB_DEVICE Devices[USB_MAX_DEVICES];

static inline void
Barrier(void)
{
    __asm__ __volatile__("mfence" : : : "memory");
}

static inline uint32_t
Rd32(volatile uint8_t *Base, uint32_t Off)
{
    return *(volatile uint32_t *)(Base + Off);
}

static inline void
Wr32(volatile uint8_t *Base, uint32_t Off, uint32_t Value)
{
    *(volatile uint32_t *)(Base + Off) = Value;
}

static void
Wr64(volatile uint8_t *Base, uint32_t Off, uint64_t Value)
{
    Wr32(Base, Off, (uint32_t)Value);
    Wr32(Base, Off + 4, (uint32_t)(Value >> 32));
}

static uint64_t
PhysOf(const void *P)
{
    return (uint64_t)(uintptr_t)P;
}

static void
Zero(void *P, uint32_t Bytes)
{
    uint8_t *B = (uint8_t *)P;
    for (uint32_t i = 0; i < Bytes; i++) {
        B[i] = 0;
    }
}

static void
Copy(void *Dst, const void *Src, uint32_t Bytes)
{
    uint8_t *D = (uint8_t *)Dst;
    const uint8_t *S = (const uint8_t *)Src;
    for (uint32_t i = 0; i < Bytes; i++) {
        D[i] = S[i];
    }
}

static void *
DmaPage(void)
{
    void *P = AllocPageBelow4G();

    if (P) {
        Zero(P, 4096);
    }

    return P;
}

static uint8_t *
DmaContiguous(uint32_t Pages, uint32_t *Got)
{
    uint8_t *All[96];
    int N = 0;
    int BestStart = 0;
    int BestLen = 0;
    int RunStart = 0;
    int RunLen = 0;

    while (N < 96 && BestLen < (int)Pages) {
        uint8_t *P = (uint8_t *)AllocPageBelow4G();

        if (!P) {
            break;
        }

        All[N] = P;

        if (N > 0 && P == All[N - 1] + 4096) {
            RunLen++;
        } else {
            RunStart = N;
            RunLen = 1;
        }

        if (RunLen > BestLen) {
            BestLen = RunLen;
            BestStart = RunStart;
        }

        N++;
    }

    if (BestLen > (int)Pages) {
        BestLen = (int)Pages;
    }

    for (int i = 0; i < N; i++) {
        if (i < BestStart || i >= BestStart + BestLen) {
            FreePage(All[i]);
        }
    }

    if (BestLen == 0) {
        *Got = 0;
        return 0;
    }

    Zero(All[BestStart], (uint32_t)BestLen * 4096);
    *Got = (uint32_t)BestLen;
    return All[BestStart];
}

static int
RingInit(XHCI_RING *R)
{
    R->Base = (volatile XHCI_TRB *)DmaPage();

    if (!R->Base) {
        return 0;
    }

    R->Phys = PhysOf((const void *)R->Base);
    R->Enq = 0;
    R->Cycle = 1;
    R->Size = RING_SIZE;

    return 1;
}

static void
RingFree(XHCI_RING *R)
{
    if (R->Base) {
        FreePage((void *)R->Base);
        R->Base = 0;
    }
}

static uint64_t
RingPush(XHCI_RING *R, uint32_t P0, uint32_t P1, uint32_t S, uint32_t C)
{
    volatile XHCI_TRB *T = &R->Base[R->Enq];

    T->P0 = P0;
    T->P1 = P1;
    T->S = S;
    Barrier();
    T->C = (C & ~1u) | R->Cycle;

    uint64_t Phys = R->Phys + (uint64_t)R->Enq * 16;

    R->Enq++;

    if (R->Enq == R->Size - 1) {
        volatile XHCI_TRB *L = &R->Base[R->Enq];

        L->P0 = (uint32_t)R->Phys;
        L->P1 = (uint32_t)(R->Phys >> 32);
        L->S = 0;
        Barrier();
        L->C = ((uint32_t)TRB_LINK << 10) | 2u | (C & 0x10u) | R->Cycle;
        R->Enq = 0;
        R->Cycle ^= 1;
    }

    return Phys;
}

static int
PopEvent(XHCI *X, EVENT *E, uint32_t TimeoutMs)
{
    uint64_t Deadline = TickCount + TimeoutMs;
    uint64_t Spin = 0;

    for (;;) {
        volatile XHCI_TRB *T = &X->Evt[X->EvtDeq];
        uint32_t C = T->C;

        if ((C & 1u) == X->EvtCycle) {
            Barrier();
            E->Ptr = ((uint64_t)T->P1 << 32) | T->P0;
            E->Status = T->S;
            E->Control = T->C;

            X->EvtDeq++;

            if (X->EvtDeq == EVT_SIZE) {
                X->EvtDeq = 0;
                X->EvtCycle ^= 1;
            }

            Wr64(X->Rt, 0x20 + 0x18, (X->EvtPhys + (uint64_t)X->EvtDeq * 16) | 8u);
            return 1;
        }

        if (TickCount >= Deadline || ++Spin > 200000000ULL) {
            return 0;
        }

        __asm__ __volatile__("pause");
    }
}

static int
Command(XHCI *X, uint32_t P0, uint32_t P1, uint32_t Control, uint32_t *SlotOut)
{
    uint64_t Phys = RingPush(&X->Cmd, P0, P1, 0, Control);
    EVENT E;

    Barrier();
    X->Db[0] = 0;

    for (;;) {
        if (!PopEvent(X, &E, 3000)) {
            KLog("XHCI: command timeout, type ");
            KLogUInt((Control >> 10) & 63);
            KLog("\n");
            return -1;
        }

        if (((E.Control >> 10) & 63) == EVT_COMMAND && E.Ptr == Phys) {
            if (SlotOut) {
                *SlotOut = E.Control >> 24;
            }
            return (int)(E.Status >> 24);
        }
    }
}

static volatile uint32_t *
CtxAt(uint8_t *Base, uint32_t CtxSize, int Index)
{
    return (volatile uint32_t *)(Base + (uint32_t)Index * CtxSize);
}

static int
DciOf(uint8_t Addr)
{
    return (Addr & 0x0F) * 2 + ((Addr & 0x80) ? 1 : 0);
}

static void
LogCode(const char *Text, int Code)
{
    KLog("XHCI: ");
    KLog(Text);
    KLog(", code ");
    KLogUInt((uint64_t)(Code < 0 ? 999 : Code));
    KLog("\n");
}

static int
ControllerInit(XHCI *X, PCI_DEVICE_INFO *Info)
{
    KLogPci("XHCI", Info->Bus, Info->Device, Info->Function, Info->VendorId, Info->DeviceId);

    PciEnableDevice(Info->Bus, Info->Device, Info->Function);

    if (Info->Bar0 == 0) {
        KLog("XHCI: bar0 is zero\n");
        return 0;
    }

    KLog("XHCI: bar0 ");
    KLogHex(Info->Bar0, 8);
    KLog("\n");

    X->Cap = (volatile uint8_t *)(uintptr_t)Info->Bar0;

    uint32_t CapLen = Rd32(X->Cap, 0) & 0xFF;
    uint32_t Version = Rd32(X->Cap, 0) >> 16;
    uint32_t Hcs1 = Rd32(X->Cap, 4);
    uint32_t Hcs2 = Rd32(X->Cap, 8);
    uint32_t Hcc1 = Rd32(X->Cap, 0x10);
    uint32_t DbOff = Rd32(X->Cap, 0x14) & ~3u;
    uint32_t RtsOff = Rd32(X->Cap, 0x18) & ~0x1Fu;

    if (Version == 0 || Version == 0xFFFF) {
        KLog("XHCI: bad version register\n");
        return 0;
    }

    X->Op = X->Cap + CapLen;
    X->Rt = X->Cap + RtsOff;
    X->Db = (volatile uint32_t *)(X->Cap + DbOff);
    X->MaxSlots = (int)(Hcs1 & 0xFF);
    X->MaxPorts = (int)((Hcs1 >> 24) & 0xFF);
    X->CtxSize = (Hcc1 & 4u) ? 64 : 32;

    if (X->MaxSlots > 64) {
        X->MaxSlots = 64;
    }

    KLog("XHCI: version ");
    KLogHex(Version, 4);
    KLog(", slots ");
    KLogUInt((uint64_t)X->MaxSlots);
    KLog(", ports ");
    KLogUInt((uint64_t)X->MaxPorts);
    KLog(", ctx ");
    KLogUInt(X->CtxSize);
    KLog("\n");

    for (int i = 0; i < 256; i++) {
        X->PortMajor[i] = 0;
    }

    uint32_t Xecp = (Hcc1 >> 16) & 0xFFFF;

    if (Xecp) {
        uint32_t Off = Xecp * 4;

        for (int Guard = 0; Guard < 64; Guard++) {
            uint32_t V = Rd32(X->Cap, Off);
            uint32_t Id = V & 0xFF;

            if (Id == 1) {
                if (V & (1u << 16)) {
                    KLog("XHCI: bios owns controller, requesting handoff\n");
                    Wr32(X->Cap, Off, V | (1u << 24));

                    for (int t = 0; t < 1000 && (Rd32(X->Cap, Off) & (1u << 16)); t++) {
                        Sleep(1);
                    }

                    if (Rd32(X->Cap, Off) & (1u << 16)) {
                        KLog("XHCI: bios handoff timed out\n");
                    }
                }

                uint32_t Ctl = Rd32(X->Cap, Off + 4);
                Ctl &= ~((3u << 1) | (0xFFu << 5) | (7u << 17));
                Ctl |= (7u << 29);
                Wr32(X->Cap, Off + 4, Ctl);
            } else if (Id == 2) {
                uint32_t Major = (V >> 24) & 0xFF;
                uint32_t Second = Rd32(X->Cap, Off + 8);
                uint32_t First = Second & 0xFF;
                uint32_t Count = (Second >> 8) & 0xFF;

                for (uint32_t p = First; p < First + Count && p < 256; p++) {
                    X->PortMajor[p] = (uint8_t)Major;
                }
            }

            uint32_t Next = (V >> 8) & 0xFF;

            if (Next == 0) {
                break;
            }

            Off += Next * 4;
        }
    }

    if (!(Rd32(X->Op, 0x04) & 1u)) {
        Wr32(X->Op, 0x00, Rd32(X->Op, 0x00) & ~1u);

        for (int t = 0; t < 200 && !(Rd32(X->Op, 0x04) & 1u); t++) {
            Sleep(1);
        }
    }

    Wr32(X->Op, 0x00, Rd32(X->Op, 0x00) | 2u);

    int Ready = 0;

    for (int t = 0; t < 1000; t++) {
        if (!(Rd32(X->Op, 0x00) & 2u) && !(Rd32(X->Op, 0x04) & (1u << 11))) {
            Ready = 1;
            break;
        }
        Sleep(1);
    }

    if (!Ready) {
        KLog("XHCI: reset timed out\n");
        return 0;
    }

    Wr32(X->Op, 0x38, (uint32_t)X->MaxSlots);

    X->Dcbaa = (uint64_t *)DmaPage();

    if (!X->Dcbaa) {
        KLog("XHCI: no memory for dcbaa\n");
        return 0;
    }

    uint32_t Scratch = (((Hcs2 >> 21) & 0x1F) << 5) | ((Hcs2 >> 27) & 0x1F);

    if (Scratch > 0) {
        if (Scratch > 500) {
            KLog("XHCI: too many scratchpad buffers\n");
            return 0;
        }

        uint64_t *Array = (uint64_t *)DmaPage();

        if (!Array) {
            return 0;
        }

        for (uint32_t i = 0; i < Scratch; i++) {
            void *Page = DmaPage();

            if (!Page) {
                KLog("XHCI: no memory for scratchpad\n");
                return 0;
            }

            Array[i] = PhysOf(Page);
        }

        X->Dcbaa[0] = PhysOf(Array);

        KLog("XHCI: scratchpad buffers ");
        KLogUInt(Scratch);
        KLog("\n");
    }

    Wr64(X->Op, 0x30, PhysOf(X->Dcbaa));

    if (!RingInit(&X->Cmd)) {
        return 0;
    }

    Wr64(X->Op, 0x18, X->Cmd.Phys | 1u);

    X->Evt = (volatile XHCI_TRB *)DmaPage();
    uint64_t *Erst = (uint64_t *)DmaPage();

    if (!X->Evt || !Erst) {
        return 0;
    }

    X->EvtPhys = PhysOf((const void *)X->Evt);
    X->EvtDeq = 0;
    X->EvtCycle = 1;

    Erst[0] = X->EvtPhys;
    Erst[1] = EVT_SIZE;

    Wr32(X->Rt, 0x20 + 0x08, 1);
    Wr64(X->Rt, 0x20 + 0x18, X->EvtPhys);
    Wr64(X->Rt, 0x20 + 0x10, PhysOf(Erst));

    X->Scratch = (uint8_t *)DmaPage();

    if (!X->Scratch) {
        return 0;
    }

    uint32_t Got = 0;
    X->Bounce = DmaContiguous(BOUNCE_PAGES, &Got);

    if (!X->Bounce) {
        KLog("XHCI: no memory for bounce buffer\n");
        return 0;
    }

    X->BounceSize = Got * 4096;

    Barrier();
    Wr32(X->Op, 0x00, Rd32(X->Op, 0x00) | 1u);

    for (int t = 0; t < 200 && (Rd32(X->Op, 0x04) & 1u); t++) {
        Sleep(1);
    }

    if (Rd32(X->Op, 0x04) & 1u) {
        KLog("XHCI: controller did not start\n");
        return 0;
    }

    for (int p = 1; p <= X->MaxPorts; p++) {
        uint32_t Off = 0x400 + 0x10 * (uint32_t)(p - 1);
        uint32_t Sc = Rd32(X->Op, Off);

        if (!(Sc & PORTSC_PP)) {
            Wr32(X->Op, Off, (Sc & PORTSC_KEEP) | PORTSC_PP);
        }
    }

    Sleep(150);

    KLog("XHCI: running\n");
    X->Used = 1;

    return 1;
}

static int
WaitPortBit(XHCI *X, uint32_t Off, uint32_t Mask, uint32_t Timeout)
{
    for (uint32_t t = 0; t < Timeout; t++) {
        if (Rd32(X->Op, Off) & Mask) {
            return 1;
        }
        Sleep(1);
    }

    return 0;
}

static int
ResetPort(XHCI *X, int Port)
{
    uint32_t Off = 0x400 + 0x10 * (uint32_t)(Port - 1);
    uint32_t Sc = Rd32(X->Op, Off);

    if (X->PortMajor[Port] == 3 && (Sc & PORTSC_PED) && ((Sc >> 5) & 0xF) == 0) {
        Wr32(X->Op, Off, (Sc & PORTSC_KEEP) | PORTSC_CHANGE);
        return 1;
    }

    Wr32(X->Op, Off, (Sc & PORTSC_KEEP) | PORTSC_PR);

    if (!WaitPortBit(X, Off, PORTSC_PRC, 600)) {
        if (X->PortMajor[Port] == 3) {
            Wr32(X->Op, Off, (Rd32(X->Op, Off) & PORTSC_KEEP) | PORTSC_WPR);

            if (!WaitPortBit(X, Off, PORTSC_WRC, 600)) {
                KLog("XHCI: port reset timed out\n");
                return 0;
            }
        } else {
            KLog("XHCI: port reset timed out\n");
            return 0;
        }
    }

    Sleep(30);

    Sc = Rd32(X->Op, Off);
    Wr32(X->Op, Off, (Sc & PORTSC_KEEP) | PORTSC_CHANGE);

    Sc = Rd32(X->Op, Off);

    return (Sc & PORTSC_CCS) && (Sc & PORTSC_PED);
}

static void
RecoverEndpoint(USB_DEVICE *D, int Dci, int Stop)
{
    XHCI *X = &Ctrls[D->Controller];
    uint32_t Ctl = ((uint32_t)D->Slot << 24) | ((uint32_t)Dci << 16);

    if (Stop) {
        Command(X, 0, 0, ((uint32_t)TRB_STOP_EP << 10) | Ctl, 0);
    }

    Command(X, 0, 0, ((uint32_t)TRB_RESET_EP << 10) | Ctl, 0);

    XHCI_RING *R = &D->Ring[Dci];
    uint64_t Deq = R->Phys + (uint64_t)R->Enq * 16;

    int Code = Command(X, (uint32_t)Deq | R->Cycle, (uint32_t)(Deq >> 32), ((uint32_t)TRB_SET_DEQ << 10) | Ctl, 0);

    if (Code != 1) {
        LogCode("set dequeue failed", Code);
    }
}

int
XhciControl(USB_DEVICE *D, uint8_t Type, uint8_t Req, uint16_t Value, uint16_t Index, void *Data, uint16_t Length)
{
    XHCI *X = &Ctrls[D->Controller];
    XHCI_RING *R = &D->Ring[1];
    uint8_t *Buf = X->Scratch + SCRATCH_CTRL;
    int In = (Type & 0x80) != 0;

    if (Length > SCRATCH_CTRL_MAX) {
        return XHCI_FAIL;
    }

    if (!In && Length) {
        Copy(Buf, Data, Length);
    }

    uint32_t Trt = Length ? (In ? 3u : 2u) : 0u;

    RingPush(R,
        (uint32_t)Type | ((uint32_t)Req << 8) | ((uint32_t)Value << 16),
        (uint32_t)Index | ((uint32_t)Length << 16),
        8,
        ((uint32_t)TRB_SETUP << 10) | (1u << 6) | (Trt << 16));

    uint64_t DataPhys = 0;

    if (Length) {
        DataPhys = RingPush(R,
            (uint32_t)PhysOf(Buf),
            (uint32_t)(PhysOf(Buf) >> 32),
            Length,
            ((uint32_t)TRB_DATA << 10) | (In ? ((1u << 16) | (1u << 2)) : 0u));
    }

    uint64_t StatusPhys = RingPush(R, 0, 0, 0,
        ((uint32_t)TRB_STATUS << 10) | (1u << 5) | ((Length && In) ? 0u : (1u << 16)));

    Barrier();
    X->Db[D->Slot] = 1;

    EVENT E;
    int Result = XHCI_FAIL;
    int TimedOut = 0;

    for (;;) {
        if (!PopEvent(X, &E, 3000)) {
            KLog("XHCI: control transfer timeout\n");
            TimedOut = 1;
            break;
        }

        if (((E.Control >> 10) & 63) != EVT_TRANSFER) {
            continue;
        }

        if (((E.Control >> 24) & 0xFF) != (uint32_t)D->Slot || ((E.Control >> 16) & 0x1F) != 1) {
            continue;
        }

        if (E.Ptr != StatusPhys && (!Length || E.Ptr != DataPhys)) {
            continue;
        }

        int Code = (int)(E.Status >> 24);

        if (E.Ptr == DataPhys && Length && Code == 13) {
            continue;
        }

        if (E.Ptr == StatusPhys && (Code == 1 || Code == 13)) {
            Result = XHCI_OK;
        } else if (Code == 6) {
            Result = XHCI_STALL;
        } else {
            LogCode("control transfer failed", Code);
        }

        break;
    }

    if (Result == XHCI_OK) {
        if (In && Length) {
            Copy(Data, Buf, Length);
        }
        return XHCI_OK;
    }

    RecoverEndpoint(D, 1, TimedOut);

    return Result;
}

static int
BulkTd(XHCI *X, USB_DEVICE *D, int In, uint8_t *Buf, uint32_t Length, uint32_t *Actual)
{
    uint8_t Addr = In ? D->BulkInAddr : D->BulkOutAddr;
    uint32_t Mps = In ? D->BulkInMps : D->BulkOutMps;
    int Dci = DciOf(Addr);
    XHCI_RING *R = &D->Ring[Dci];

    if (!In) {
        Copy(X->Bounce, Buf, Length);
    }

    uint64_t Addr64 = PhysOf(X->Bounce);
    uint32_t Left = Length;
    uint64_t TrbPhys[4];
    uint32_t TrbLen[4];
    int Count = 0;

    while (Left > 0 && Count < 4) {
        uint32_t Room = 0x10000u - (uint32_t)(Addr64 & 0xFFFF);
        uint32_t Chunk = Left < Room ? Left : Room;
        int Last = (Left == Chunk);
        uint32_t Remaining = Left - Chunk;
        uint32_t Packets = Mps ? (Remaining + Mps - 1) / Mps : 0;

        if (Packets > 31) {
            Packets = 31;
        }

        uint32_t C = ((uint32_t)TRB_NORMAL << 10) | (In ? (1u << 2) : 0u) | (Last ? (1u << 5) : (1u << 4));

        TrbPhys[Count] = RingPush(R, (uint32_t)Addr64, (uint32_t)(Addr64 >> 32), Chunk | (Packets << 17), C);
        TrbLen[Count] = Chunk;
        Count++;

        Addr64 += Chunk;
        Left -= Chunk;
    }

    Barrier();
    X->Db[D->Slot] = (uint32_t)Dci;

    EVENT E;
    int Result = XHCI_FAIL;
    int TimedOut = 0;

    *Actual = 0;

    for (;;) {
        if (!PopEvent(X, &E, 6000)) {
            KLog("XHCI: bulk transfer timeout\n");
            TimedOut = 1;
            break;
        }

        if (((E.Control >> 10) & 63) != EVT_TRANSFER) {
            continue;
        }

        if (((E.Control >> 24) & 0xFF) != (uint32_t)D->Slot || ((E.Control >> 16) & 0x1F) != (uint32_t)Dci) {
            continue;
        }

        int Hit = -1;

        for (int k = 0; k < Count; k++) {
            if (TrbPhys[k] == E.Ptr) {
                Hit = k;
                break;
            }
        }

        if (Hit < 0) {
            continue;
        }

        int Code = (int)(E.Status >> 24);
        uint32_t Residue = E.Status & 0xFFFFFF;

        if (Code == 1 || Code == 13) {
            uint32_t Before = 0;

            for (int k = 0; k < Hit; k++) {
                Before += TrbLen[k];
            }

            *Actual = Before + (TrbLen[Hit] > Residue ? TrbLen[Hit] - Residue : 0);

            if (Code == 1 && Hit == Count - 1) {
                *Actual = Length;
            }

            Result = XHCI_OK;
        } else if (Code == 6) {
            Result = XHCI_STALL;
        } else {
            LogCode("bulk transfer failed", Code);
        }

        break;
    }

    if (Result == XHCI_OK) {
        if (In) {
            Copy(Buf, X->Bounce, *Actual);
        }
        return XHCI_OK;
    }

    if (Result == XHCI_FAIL) {
        RecoverEndpoint(D, Dci, TimedOut);
    }

    return Result;
}

int
XhciBulk(USB_DEVICE *D, int In, void *Buffer, uint32_t Length, uint32_t *Actual)
{
    XHCI *X = &Ctrls[D->Controller];
    uint8_t *B = (uint8_t *)Buffer;
    uint32_t Total = 0;

    if (Length == 0) {
        if (Actual) {
            *Actual = 0;
        }
        return XHCI_OK;
    }

    while (Length > 0) {
        uint32_t N = Length > X->BounceSize ? X->BounceSize : Length;
        uint32_t Got = 0;
        int St = BulkTd(X, D, In, B, N, &Got);

        if (St != XHCI_OK) {
            if (Actual) {
                *Actual = Total + Got;
            }
            return St;
        }

        Total += Got;
        B += Got;
        Length -= Got;

        if (Got < N) {
            break;
        }
    }

    if (Actual) {
        *Actual = Total;
    }

    return XHCI_OK;
}

int
XhciClearHalt(USB_DEVICE *D, int In)
{
    uint8_t Addr = In ? D->BulkInAddr : D->BulkOutAddr;

    RecoverEndpoint(D, DciOf(Addr), 0);

    return XhciControl(D, 0x02, 1, 0, Addr, 0, 0);
}

uint32_t
XhciMaxBulk(void)
{
    for (int i = 0; i < CtrlCount; i++) {
        if (Ctrls[i].Used) {
            return Ctrls[i].BounceSize;
        }
    }

    return 4096;
}

static int
GetDescriptor(USB_DEVICE *D, uint8_t Type, uint8_t Index, void *Buf, uint16_t Length)
{
    return XhciControl(D, 0x80, 6, (uint16_t)(((uint16_t)Type << 8) | Index), 0, Buf, Length);
}

static void
ParseConfig(USB_DEVICE *D, const uint8_t *B, uint32_t Length)
{
    D->ConfigValue = B[5];
    D->IsMass = 0;

    int InMass = 0;
    int HaveIn = 0;
    int HaveOut = 0;
    uint32_t Pos = 0;
    int LastEpIn = -1;

    while (Pos + 2 <= Length) {
        uint32_t Len = B[Pos];
        uint32_t Type = B[Pos + 1];

        if (Len < 2 || Pos + Len > Length) {
            break;
        }

        if (Type == 4 && Len >= 9) {
            InMass = 0;
            LastEpIn = -1;

            if (B[Pos + 5] == 0x09) {
                D->HubSeen = 1;
            }

            if (!D->IsMass && B[Pos + 3] == 0 && B[Pos + 5] == 0x08 && B[Pos + 6] == 0x06 && B[Pos + 7] == 0x50) {
                InMass = 1;
                D->Interface = B[Pos + 2];
                HaveIn = 0;
                HaveOut = 0;
            }
        } else if (Type == 5 && Len >= 7 && InMass) {
            uint8_t Addr = B[Pos + 2];
            uint8_t Attr = B[Pos + 3];
            uint16_t Mps = (uint16_t)(B[Pos + 4] | (B[Pos + 5] << 8));

            if ((Attr & 3) == 2) {
                if (Addr & 0x80) {
                    D->BulkInAddr = Addr;
                    D->BulkInMps = Mps & 0x7FF;
                    D->BulkInBurst = 0;
                    HaveIn = 1;
                    LastEpIn = 1;
                } else {
                    D->BulkOutAddr = Addr;
                    D->BulkOutMps = Mps & 0x7FF;
                    D->BulkOutBurst = 0;
                    HaveOut = 1;
                    LastEpIn = 0;
                }

                if (HaveIn && HaveOut) {
                    D->IsMass = 1;
                }
            } else {
                LastEpIn = -1;
            }
        } else if (Type == 0x30 && Len >= 3 && InMass) {
            if (LastEpIn == 1) {
                D->BulkInBurst = B[Pos + 2];
            } else if (LastEpIn == 0) {
                D->BulkOutBurst = B[Pos + 2];
            }
        }

        Pos += Len;
    }
}

static int
ConfigureEndpoints(XHCI *X, USB_DEVICE *D)
{
    int DciIn = DciOf(D->BulkInAddr);
    int DciOut = DciOf(D->BulkOutAddr);
    int MaxDci = DciIn > DciOut ? DciIn : DciOut;

    if (!RingInit(&D->Ring[DciIn]) || !RingInit(&D->Ring[DciOut])) {
        return 0;
    }

    Zero(D->InCtx, 4096);

    volatile uint32_t *Ctl = CtxAt(D->InCtx, X->CtxSize, 0);
    volatile uint32_t *Slot = CtxAt(D->InCtx, X->CtxSize, 1);
    volatile uint32_t *OutSlot = CtxAt(D->OutCtx, X->CtxSize, 1);

    Ctl[1] = 1u | (1u << DciIn) | (1u << DciOut);

    for (int i = 0; i < 8; i++) {
        Slot[i] = OutSlot[i];
    }

    Slot[0] = (Slot[0] & ~(0x1Fu << 27)) | ((uint32_t)MaxDci << 27);

    for (int Dir = 0; Dir < 2; Dir++) {
        int In = Dir == 0;
        int Dci = In ? DciIn : DciOut;
        uint32_t Mps = In ? D->BulkInMps : D->BulkOutMps;
        uint32_t Burst = In ? D->BulkInBurst : D->BulkOutBurst;
        XHCI_RING *R = &D->Ring[Dci];
        volatile uint32_t *Ep = CtxAt(D->InCtx, X->CtxSize, Dci + 1);

        Ep[0] = 0;
        Ep[1] = (3u << 1) | ((In ? 6u : 2u) << 3) | (Burst << 8) | (Mps << 16);
        Ep[2] = (uint32_t)R->Phys | 1u;
        Ep[3] = (uint32_t)(R->Phys >> 32);
        Ep[4] = Mps;
    }

    Barrier();

    int Code = Command(X, (uint32_t)PhysOf(D->InCtx), (uint32_t)(PhysOf(D->InCtx) >> 32),
        ((uint32_t)TRB_CONFIGURE_EP << 10) | ((uint32_t)D->Slot << 24), 0);

    if (Code != 1) {
        LogCode("configure endpoint failed", Code);
        return 0;
    }

    return 1;
}

static int
FindFreeDevice(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!Devices[i].Used) {
            return i;
        }
    }

    return -1;
}

void
XhciRelease(USB_DEVICE *D)
{
    if (!D->Used) {
        return;
    }

    XHCI *X = &Ctrls[D->Controller];

    if (D->Slot > 0) {
        Command(X, 0, 0, ((uint32_t)TRB_DISABLE_SLOT << 10) | ((uint32_t)D->Slot << 24), 0);
        X->Dcbaa[D->Slot] = 0;
    }

    for (int i = 0; i < 32; i++) {
        RingFree(&D->Ring[i]);
    }

    if (D->OutCtx) {
        FreePage(D->OutCtx);
        D->OutCtx = 0;
    }

    if (D->InCtx) {
        FreePage(D->InCtx);
        D->InCtx = 0;
    }

    uint32_t Off = 0x400 + 0x10 * (uint32_t)(D->Port - 1);
    Wr32(X->Op, Off, (Rd32(X->Op, Off) & PORTSC_KEEP) | PORTSC_CHANGE);

    D->Used = 0;
}

static void
EnumeratePort(XHCI *X, int Ci, int Port)
{
    int Index = FindFreeDevice();

    if (Index < 0) {
        KLog("XHCI: device table full\n");
        return;
    }

    if (!ResetPort(X, Port)) {
        KLog("XHCI: port ");
        KLogUInt((uint64_t)Port);
        KLog(" did not enable after reset\n");
        return;
    }

    uint32_t Off = 0x400 + 0x10 * (uint32_t)(Port - 1);
    uint32_t Speed = (Rd32(X->Op, Off) >> 10) & 0xF;

    if (Speed == 0 || Speed > 5) {
        Speed = Speed == 0 ? USB_SPEED_FULL : USB_SPEED_SUPER;
    }

    USB_DEVICE *D = &Devices[Index];
    Zero(D, sizeof(USB_DEVICE));
    D->Used = 1;
    D->Controller = Ci;
    D->Port = Port;
    D->Speed = (int)Speed;

    KLog("XHCI: port ");
    KLogUInt((uint64_t)Port);
    KLog(" connected, speed ");
    KLog(UsbSpeedName(D->Speed));
    KLog("\n");

    uint32_t SlotId = 0;
    int Code = Command(X, 0, 0, (uint32_t)TRB_ENABLE_SLOT << 10, &SlotId);

    if (Code != 1 || SlotId == 0 || (int)SlotId > X->MaxSlots) {
        LogCode("enable slot failed", Code);
        D->Used = 0;
        return;
    }

    D->Slot = (int)SlotId;
    D->OutCtx = (uint8_t *)DmaPage();
    D->InCtx = (uint8_t *)DmaPage();

    if (!D->OutCtx || !D->InCtx || !RingInit(&D->Ring[1])) {
        KLog("XHCI: no memory for device\n");
        goto Fail;
    }

    X->Dcbaa[D->Slot] = PhysOf(D->OutCtx);

    uint32_t Mps0 = Speed == USB_SPEED_LOW ? 8 : (Speed == USB_SPEED_FULL ? 8 : (Speed == USB_SPEED_HIGH ? 64 : 512));
    D->Ep0Mps = Mps0;

    volatile uint32_t *Ctl = CtxAt(D->InCtx, X->CtxSize, 0);
    volatile uint32_t *Slot = CtxAt(D->InCtx, X->CtxSize, 1);
    volatile uint32_t *Ep0 = CtxAt(D->InCtx, X->CtxSize, 2);

    Ctl[1] = 3;
    Slot[0] = (Speed << 20) | (1u << 27);
    Slot[1] = (uint32_t)Port << 16;
    Ep0[1] = (3u << 1) | (4u << 3) | (Mps0 << 16);
    Ep0[2] = (uint32_t)D->Ring[1].Phys | 1u;
    Ep0[3] = (uint32_t)(D->Ring[1].Phys >> 32);
    Ep0[4] = 8;

    Barrier();

    Code = Command(X, (uint32_t)PhysOf(D->InCtx), (uint32_t)(PhysOf(D->InCtx) >> 32),
        ((uint32_t)TRB_ADDRESS_DEVICE << 10) | ((uint32_t)D->Slot << 24), 0);

    if (Code != 1) {
        LogCode("address device failed", Code);
        goto Fail;
    }

    Sleep(10);

    uint8_t Desc[18];

    if (GetDescriptor(D, 1, 0, Desc, 8) != XHCI_OK) {
        KLog("XHCI: device descriptor read failed\n");
        goto Fail;
    }

    uint32_t NewMps = Desc[7];

    if (Speed >= USB_SPEED_SUPER) {
        NewMps = NewMps >= 9 ? (1u << NewMps) : Mps0;
    }

    if (NewMps != Mps0 && NewMps >= 8 && Speed != USB_SPEED_SUPER && Speed != USB_SPEED_SUPER_PLUS) {
        Zero(D->InCtx, 4096);
        volatile uint32_t *OutEp0 = CtxAt(D->OutCtx, X->CtxSize, 2);
        volatile uint32_t *InEp0 = CtxAt(D->InCtx, X->CtxSize, 2);

        CtxAt(D->InCtx, X->CtxSize, 0)[1] = 2;

        for (int i = 0; i < 8; i++) {
            InEp0[i] = OutEp0[i];
        }

        InEp0[1] = (InEp0[1] & 0xFFFFu) | (NewMps << 16);

        Barrier();

        Code = Command(X, (uint32_t)PhysOf(D->InCtx), (uint32_t)(PhysOf(D->InCtx) >> 32),
            ((uint32_t)TRB_EVALUATE_CTX << 10) | ((uint32_t)D->Slot << 24), 0);

        if (Code != 1) {
            LogCode("evaluate context failed", Code);
            goto Fail;
        }

        D->Ep0Mps = NewMps;
    }

    if (GetDescriptor(D, 1, 0, Desc, 18) != XHCI_OK) {
        KLog("XHCI: full device descriptor read failed\n");
        goto Fail;
    }

    D->DevClass = Desc[4];
    D->DevSubClass = Desc[5];
    D->DevProtocol = Desc[6];
    D->VendorId = (uint16_t)(Desc[8] | (Desc[9] << 8));
    D->ProductId = (uint16_t)(Desc[10] | (Desc[11] << 8));

    KLog("XHCI: device ");
    KLogHex(D->VendorId, 4);
    KLog(":");
    KLogHex(D->ProductId, 4);
    KLog(" class ");
    KLogHex(D->DevClass, 2);
    KLog("\n");

    uint8_t Cfg[9];

    if (GetDescriptor(D, 2, 0, Cfg, 9) != XHCI_OK) {
        KLog("XHCI: config descriptor read failed\n");
        goto Fail;
    }

    uint32_t Total = (uint32_t)(Cfg[2] | (Cfg[3] << 8));

    if (Total < 9 || Total > 2048) {
        KLog("XHCI: bad config descriptor length\n");
        goto Fail;
    }

    static uint8_t CfgFull[2048];

    if (GetDescriptor(D, 2, 0, CfgFull, (uint16_t)Total) != XHCI_OK) {
        KLog("XHCI: full config descriptor read failed\n");
        goto Fail;
    }

    ParseConfig(D, CfgFull, Total);

    if (D->DevClass == 9) {
        D->HubSeen = 1;
    }

    if (D->HubSeen) {
        KLog("XHCI: hubs are not supported, device ignored\n");
    }

    if (D->IsMass && !D->HubSeen) {
        KLog("XHCI: mass storage interface ");
        KLogUInt(D->Interface);
        KLog(", bulk in ");
        KLogHex(D->BulkInAddr, 2);
        KLog(" out ");
        KLogHex(D->BulkOutAddr, 2);
        KLog("\n");

        if (!ConfigureEndpoints(X, D)) {
            goto Fail;
        }

        if (XhciControl(D, 0x00, 9, D->ConfigValue, 0, 0, 0) != XHCI_OK) {
            KLog("XHCI: set configuration failed\n");
            goto Fail;
        }
    } else {
        D->IsMass = 0;
    }

    return;

Fail:
    XhciRelease(D);
}

int
XhciRescan(void)
{
    if (!Probed) {
        Probed = 1;

        for (int i = 0; i < MAX_CONTROLLERS; i++) {
            PCI_DEVICE_INFO Info;

            if (!PciFindByClass(0x0C, 0x03, 0x30, i, &Info)) {
                break;
            }

            if (ControllerInit(&Ctrls[CtrlCount], &Info)) {
                CtrlCount++;
            } else {
                KLog("XHCI: controller init failed\n");
            }
        }
    }

    if (CtrlCount == 0) {
        return -1;
    }

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        USB_DEVICE *D = &Devices[i];

        if (!D->Used || D->Gone) {
            continue;
        }

        XHCI *X = &Ctrls[D->Controller];
        uint32_t Sc = Rd32(X->Op, 0x400 + 0x10 * (uint32_t)(D->Port - 1));

        if (!(Sc & PORTSC_CCS) || (Sc & PORTSC_CSC)) {
            D->Gone = 1;
            KLog("XHCI: device on port ");
            KLogUInt((uint64_t)D->Port);
            KLog(" removed\n");
        }
    }

    int Added = 0;

    for (int c = 0; c < CtrlCount; c++) {
        XHCI *X = &Ctrls[c];

        for (int p = 1; p <= X->MaxPorts; p++) {
            uint32_t Sc = Rd32(X->Op, 0x400 + 0x10 * (uint32_t)(p - 1));

            if (!(Sc & PORTSC_CCS)) {
                continue;
            }

            int Known = 0;

            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                if (Devices[i].Used && Devices[i].Controller == c && Devices[i].Port == p) {
                    Known = 1;
                    break;
                }
            }

            if (Known) {
                continue;
            }

            int Before = 0;

            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                Before += Devices[i].Used;
            }

            EnumeratePort(X, c, p);

            int After = 0;

            for (int i = 0; i < USB_MAX_DEVICES; i++) {
                After += Devices[i].Used;
            }

            if (After > Before) {
                Added++;
            }
        }
    }

    return Added;
}

int
XhciControllerCount(void)
{
    return CtrlCount;
}

int
XhciDeviceCount(void)
{
    return USB_MAX_DEVICES;
}

USB_DEVICE *
XhciDevice(int Index)
{
    if (Index < 0 || Index >= USB_MAX_DEVICES || !Devices[Index].Used) {
        return 0;
    }

    return &Devices[Index];
}

const char *
UsbSpeedName(int Speed)
{
    switch (Speed) {
    case USB_SPEED_LOW: return "low";
    case USB_SPEED_FULL: return "full";
    case USB_SPEED_HIGH: return "high";
    case USB_SPEED_SUPER: return "super";
    case USB_SPEED_SUPER_PLUS: return "super+";
    default: return "unknown";
    }
}
