#include "nvme.h"
#include "pmm.h"

#define REG_CAP     0x00
#define REG_CC      0x14
#define REG_CSTS    0x1C
#define REG_AQA     0x24
#define REG_ASQ     0x28
#define REG_ACQ     0x30
#define REG_DOORBELL_BASE 0x1000

#define QUEUE_DEPTH 64

#pragma pack(push, 1)

typedef struct {
    uint32_t Cdw0;
    uint32_t Nsid;
    uint64_t Reserved;
    uint64_t Mptr;
    uint64_t Prp1;
    uint64_t Prp2;
    uint32_t Cdw10;
    uint32_t Cdw11;
    uint32_t Cdw12;
    uint32_t Cdw13;
    uint32_t Cdw14;
    uint32_t Cdw15;
} NVME_COMMAND;

typedef struct {
    uint32_t Cdw0;
    uint32_t Reserved;
    uint16_t SqHead;
    uint16_t SqId;
    uint16_t CommandId;
    uint16_t StatusPhase;
} NVME_COMPLETION;

#pragma pack(pop)

static volatile uint8_t *NvmeBase;
static uint32_t DoorbellStride;
static uint16_t NextCommandId = 1;

static NVME_COMMAND *AdminSq;
static NVME_COMPLETION *AdminCq;
static uint16_t AdminSqTail;
static uint16_t AdminCqHead;
static uint8_t AdminPhase;

static NVME_COMMAND *IoSq;
static NVME_COMPLETION *IoCq;
static uint16_t IoSqTail;
static uint16_t IoCqHead;
static uint8_t IoPhase;

static uint32_t
Read32(uint32_t Offset)
{
    return *(volatile uint32_t *)(NvmeBase + Offset);
}

static void
Write32(uint32_t Offset, uint32_t Value)
{
    *(volatile uint32_t *)(NvmeBase + Offset) = Value;
}

static uint64_t
Read64(uint32_t Offset)
{
    return *(volatile uint64_t *)(NvmeBase + Offset);
}

static void
Write64(uint32_t Offset, uint64_t Value)
{
    *(volatile uint64_t *)(NvmeBase + Offset) = Value;
}

static void
Wait(uint32_t Loops)
{
    for (volatile uint32_t i = 0; i < Loops; i++) {
        __asm__ __volatile__("nop");
    }
}

static void
ZeroBytes(void *Ptr, uint64_t Size)
{
    uint8_t *Bytes = (uint8_t *)Ptr;
    for (uint64_t i = 0; i < Size; i++) {
        Bytes[i] = 0;
    }
}

static void
RingDoorbell(uint16_t QueueId, int IsCompletion, uint16_t Value)
{
    uint32_t Offset = REG_DOORBELL_BASE + ((2 * QueueId + (IsCompletion ? 1 : 0)) * DoorbellStride);
    Write32(Offset, Value);
}

static uint16_t
SubmitAdminCommand(NVME_COMMAND *Cmd)
{
    Cmd->Cdw0 = (Cmd->Cdw0 & 0xFFFF) | ((uint32_t)NextCommandId << 16);
    NextCommandId++;

    AdminSq[AdminSqTail] = *Cmd;
    AdminSqTail = (uint16_t)((AdminSqTail + 1) % QUEUE_DEPTH);
    RingDoorbell(0, 0, AdminSqTail);

    for (int i = 0; i < 1000000; i++) {
        volatile NVME_COMPLETION *Cpl = &AdminCq[AdminCqHead];
        uint8_t Phase = (uint8_t)(Cpl->StatusPhase & 1);

        if (Phase == AdminPhase) {
            uint16_t Status = (uint16_t)(Cpl->StatusPhase >> 1);
            AdminCqHead = (uint16_t)((AdminCqHead + 1) % QUEUE_DEPTH);
            if (AdminCqHead == 0) {
                AdminPhase ^= 1;
            }
            RingDoorbell(0, 1, AdminCqHead);
            return Status;
        }

        Wait(50);
    }

    return 0xFFFF;
}

static uint16_t
SubmitIoCommand(NVME_COMMAND *Cmd)
{
    Cmd->Cdw0 = (Cmd->Cdw0 & 0xFFFF) | ((uint32_t)NextCommandId << 16);
    NextCommandId++;

    IoSq[IoSqTail] = *Cmd;
    IoSqTail = (uint16_t)((IoSqTail + 1) % QUEUE_DEPTH);
    RingDoorbell(1, 0, IoSqTail);

    for (int i = 0; i < 1000000; i++) {
        volatile NVME_COMPLETION *Cpl = &IoCq[IoCqHead];
        uint8_t Phase = (uint8_t)(Cpl->StatusPhase & 1);

        if (Phase == IoPhase) {
            uint16_t Status = (uint16_t)(Cpl->StatusPhase >> 1);
            IoCqHead = (uint16_t)((IoCqHead + 1) % QUEUE_DEPTH);
            if (IoCqHead == 0) {
                IoPhase ^= 1;
            }
            RingDoorbell(1, 1, IoCqHead);
            return Status;
        }

        Wait(50);
    }

    return 0xFFFF;
}

int
NvmeInit(uint64_t Bar0)
{
    NvmeBase = (volatile uint8_t *)(uintptr_t)Bar0;

    uint64_t Cap = Read64(REG_CAP);
    DoorbellStride = 4u << ((Cap >> 32) & 0xF);

    Write32(REG_CC, Read32(REG_CC) & ~1u);
    for (int i = 0; i < 100000; i++) {
        if ((Read32(REG_CSTS) & 1u) == 0) {
            break;
        }
        Wait(100);
    }

    void *AsqPage = AllocPage();
    void *AcqPage = AllocPage();

    if (!AsqPage || !AcqPage) {
        return 0;
    }

    ZeroBytes(AsqPage, 4096);
    ZeroBytes(AcqPage, 4096);

    AdminSq = (NVME_COMMAND *)AsqPage;
    AdminCq = (NVME_COMPLETION *)AcqPage;
    AdminSqTail = 0;
    AdminCqHead = 0;
    AdminPhase = 1;

    Write32(REG_AQA, ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1));
    Write64(REG_ASQ, (uint64_t)(uintptr_t)AsqPage);
    Write64(REG_ACQ, (uint64_t)(uintptr_t)AcqPage);

    uint32_t Cc = 1u | (6u << 16) | (4u << 20);
    Write32(REG_CC, Cc);

    int Ready = 0;
    for (int i = 0; i < 100000; i++) {
        if (Read32(REG_CSTS) & 1u) {
            Ready = 1;
            break;
        }
        Wait(100);
    }

    if (!Ready) {
        return 0;
    }

    void *IoSqPage = AllocPage();
    void *IoCqPage = AllocPage();

    if (!IoSqPage || !IoCqPage) {
        return 0;
    }

    ZeroBytes(IoSqPage, 4096);
    ZeroBytes(IoCqPage, 4096);

    IoSq = (NVME_COMMAND *)IoSqPage;
    IoCq = (NVME_COMPLETION *)IoCqPage;
    IoSqTail = 0;
    IoCqHead = 0;
    IoPhase = 1;

    NVME_COMMAND CreateCq;
    ZeroBytes(&CreateCq, sizeof(CreateCq));
    CreateCq.Cdw0 = 0x05;
    CreateCq.Prp1 = (uint64_t)(uintptr_t)IoCqPage;
    CreateCq.Cdw10 = ((QUEUE_DEPTH - 1) << 16) | 1;
    CreateCq.Cdw11 = 1;

    if (SubmitAdminCommand(&CreateCq) != 0) {
        return 0;
    }

    NVME_COMMAND CreateSq;
    ZeroBytes(&CreateSq, sizeof(CreateSq));
    CreateSq.Cdw0 = 0x01;
    CreateSq.Prp1 = (uint64_t)(uintptr_t)IoSqPage;
    CreateSq.Cdw10 = ((QUEUE_DEPTH - 1) << 16) | 1;
    CreateSq.Cdw11 = (1 << 16) | 1;

    if (SubmitAdminCommand(&CreateSq) != 0) {
        return 0;
    }

    return 1;
}

int
NvmeReadSectors(uint64_t Lba, uint32_t Count, void *Buffer)
{
    if (!IoSq) {
        return 0;
    }

    NVME_COMMAND Cmd;
    ZeroBytes(&Cmd, sizeof(Cmd));

    Cmd.Cdw0 = 0x02;
    Cmd.Nsid = 1;
    Cmd.Prp1 = (uint64_t)(uintptr_t)Buffer;
    Cmd.Cdw10 = (uint32_t)(Lba & 0xFFFFFFFFu);
    Cmd.Cdw11 = (uint32_t)(Lba >> 32);
    Cmd.Cdw12 = (Count - 1) & 0xFFFF;

    uint16_t Status = SubmitIoCommand(&Cmd);
    return Status == 0;
}
