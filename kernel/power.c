#include <stdint.h>
#include "power.h"
#include "io.h"
#include "klog.h"
#include "console.h"
#include "idt.h"

#define SLP_EN 0x2000
#define MAX_TABLE_SIZE 0x1000000

static uint16_t Pm1aCnt;
static uint16_t Pm1bCnt;
static uint16_t SlpTypA;
static uint16_t SlpTypB;
static int S5Found;
static uint16_t SmiCmd;
static uint16_t Pm1aEvt;
static uint16_t Pm1bEvt;
static uint8_t Pm1EvtLen;
static uint16_t Gpe0Blk;
static uint8_t Gpe0Len;
static uint16_t Gpe1Blk;
static uint8_t Gpe1Len;
static uint8_t AcpiEnableValue;
static int ResetSupported;
static uint8_t ResetSpace;
static uint64_t ResetAddress;
static uint8_t ResetValue;
static int Ready;

static void
OutW(uint16_t Port, uint16_t Value)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(Value), "Nd"(Port));
}

static uint16_t
InW(uint16_t Port)
{
    uint16_t Value;
    __asm__ __volatile__("inw %1, %0" : "=a"(Value) : "Nd"(Port));
    return Value;
}

static uint32_t
ReadU32(const uint8_t *P)
{
    return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

static uint64_t
ReadU64(const uint8_t *P)
{
    return (uint64_t)ReadU32(P) | ((uint64_t)ReadU32(P + 4) << 32);
}

static int
SigEq(const uint8_t *P, const char *Sig)
{
    for (int i = 0; i < 4; i++) {
        if (P[i] != (uint8_t)Sig[i]) {
            return 0;
        }
    }
    return 1;
}

static int
ChecksumOk(const uint8_t *P, uint32_t Length)
{
    uint8_t Sum = 0;
    for (uint32_t i = 0; i < Length; i++) {
        Sum = (uint8_t)(Sum + P[i]);
    }
    return Sum == 0;
}

static const uint8_t *
FindTable(const uint8_t *Root, int EntrySize, const char *Sig)
{
    uint32_t Length = ReadU32(Root + 4);

    if (Length < 36 || Length > MAX_TABLE_SIZE) {
        return 0;
    }

    uint32_t Count = (Length - 36) / (uint32_t)EntrySize;

    for (uint32_t i = 0; i < Count; i++) {
        const uint8_t *Entry = Root + 36 + i * (uint32_t)EntrySize;
        uint64_t Address = (EntrySize == 8) ? ReadU64(Entry) : ReadU32(Entry);

        if (Address == 0) {
            continue;
        }

        const uint8_t *Table = (const uint8_t *)(uintptr_t)Address;

        if (SigEq(Table, Sig)) {
            return Table;
        }
    }

    return 0;
}

static int
ReadPackageByte(const uint8_t **P, uint16_t *Out)
{
    if (**P == 0x0A) {
        (*P)++;
    } else if (**P != 0x00 && **P != 0x01) {
        return 0;
    }

    *Out = **P;
    (*P)++;
    return 1;
}

static int
ParseS5(const uint8_t *Table)
{
    uint32_t Length = ReadU32(Table + 4);

    if (Length < 44 || Length > MAX_TABLE_SIZE) {
        return 0;
    }

    for (uint32_t i = 36; i + 12 < Length; i++) {
        if (Table[i] != '_' || Table[i + 1] != 'S' || Table[i + 2] != '5' || Table[i + 3] != '_') {
            continue;
        }

        const uint8_t *P = Table + i + 4;

        if (*P != 0x12) {
            continue;
        }

        P++;
        P += ((*P >> 6) & 3) + 1;
        P++;

        uint16_t A;
        uint16_t B;

        if (!ReadPackageByte(&P, &A) || !ReadPackageByte(&P, &B)) {
            continue;
        }

        SlpTypA = (uint16_t)(A << 10);
        SlpTypB = (uint16_t)(B << 10);
        return 1;
    }

    return 0;
}

void
PowerInit(uint64_t Rsdp)
{
    if (Rsdp == 0 || Rsdp >= 0x0000010000000000ULL) {
        KLog("ACPI: no rsdp\n");
        return;
    }

    const uint8_t *R = (const uint8_t *)(uintptr_t)Rsdp;

    if (R[0] != 'R' || R[1] != 'S' || R[2] != 'D' || R[3] != ' ' || R[4] != 'P' || R[5] != 'T' || R[6] != 'R' || R[7] != ' ') {
        KLog("ACPI: bad rsdp signature\n");
        return;
    }

    if (!ChecksumOk(R, 20)) {
        KLog("ACPI: bad rsdp checksum\n");
        return;
    }

    uint8_t Revision = R[15];
    const uint8_t *Root = 0;
    int EntrySize = 4;

    if (Revision >= 2 && ReadU64(R + 24) != 0) {
        Root = (const uint8_t *)(uintptr_t)ReadU64(R + 24);
        EntrySize = 8;
    } else {
        Root = (const uint8_t *)(uintptr_t)ReadU32(R + 16);
    }

    KLog("ACPI: rsdp ");
    KLogHex(Rsdp, 1);
    KLog(" rev ");
    KLogUInt(Revision);
    KLog(EntrySize == 8 ? " xsdt\n" : " rsdt\n");

    if (Root == 0) {
        KLog("ACPI: no root table\n");
        return;
    }

    const uint8_t *Fadt = FindTable(Root, EntrySize, "FACP");

    if (Fadt == 0 || ReadU32(Fadt + 4) < 116) {
        KLog("ACPI: no fadt\n");
        return;
    }

    uint32_t FadtLength = ReadU32(Fadt + 4);

    SmiCmd = (uint16_t)ReadU32(Fadt + 48);
    AcpiEnableValue = Fadt[52];
    Pm1aEvt = (uint16_t)ReadU32(Fadt + 56);
    Pm1bEvt = (uint16_t)ReadU32(Fadt + 60);
    Pm1aCnt = (uint16_t)ReadU32(Fadt + 64);
    Pm1bCnt = (uint16_t)ReadU32(Fadt + 68);

    Pm1EvtLen = Fadt[88];
    Gpe0Blk = (uint16_t)ReadU32(Fadt + 80);
    Gpe1Blk = (uint16_t)ReadU32(Fadt + 84);
    Gpe0Len = Fadt[92];
    Gpe1Len = Fadt[93];

    uint32_t Flags = ReadU32(Fadt + 112);

    if (FadtLength >= 129 && (Flags & (1u << 10))) {
        ResetSpace = Fadt[116];
        ResetAddress = ReadU64(Fadt + 120);
        ResetValue = Fadt[128];
        ResetSupported = (ResetSpace == 0 || ResetSpace == 1) && ResetAddress != 0;
    }

    uint64_t DsdtAddress = ReadU32(Fadt + 40);

    if (FadtLength >= 148 && ReadU64(Fadt + 140) != 0) {
        DsdtAddress = ReadU64(Fadt + 140);
    }

    if (DsdtAddress != 0) {
        const uint8_t *Dsdt = (const uint8_t *)(uintptr_t)DsdtAddress;

        if (SigEq(Dsdt, "DSDT")) {
            S5Found = ParseS5(Dsdt);
        }
    }

    if (!S5Found) {
        uint32_t Length = ReadU32(Root + 4);
        uint32_t Count = (Length >= 36 && Length <= MAX_TABLE_SIZE) ? (Length - 36) / (uint32_t)EntrySize : 0;

        for (uint32_t i = 0; i < Count && !S5Found; i++) {
            const uint8_t *Entry = Root + 36 + i * (uint32_t)EntrySize;
            uint64_t Address = (EntrySize == 8) ? ReadU64(Entry) : ReadU32(Entry);

            if (Address != 0 && SigEq((const uint8_t *)(uintptr_t)Address, "SSDT")) {
                S5Found = ParseS5((const uint8_t *)(uintptr_t)Address);
            }
        }
    }

    KLog("ACPI: pm1a ");
    KLogHex(Pm1aCnt, 1);
    KLog(" pm1b ");
    KLogHex(Pm1bCnt, 1);
    KLog(" s5 ");
    if (S5Found) {
        KLogHex(SlpTypA >> 10, 1);
        KLog(",");
        KLogHex(SlpTypB >> 10, 1);
    } else {
        KLog("none");
    }
    KLog(" smi ");
    KLogHex(SmiCmd, 1);
    KLog("\n");

    KLog("ACPI: evt ");
    KLogHex(Pm1aEvt, 1);
    KLog(" len ");
    KLogUInt(Pm1EvtLen);
    KLog(" gpe0 ");
    KLogHex(Gpe0Blk, 1);
    KLog(" len ");
    KLogUInt(Gpe0Len);
    KLog(" gpe1 ");
    KLogHex(Gpe1Blk, 1);
    KLog(" len ");
    KLogUInt(Gpe1Len);
    KLog("\n");

    KLog("ACPI: reset ");
    if (ResetSupported) {
        KLogUInt(ResetSpace);
        KLog(" ");
        KLogHex(ResetAddress, 1);
        KLog(" value ");
        KLogHex(ResetValue, 1);
    } else {
        KLog("none");
    }
    KLog("\n");

    Ready = 1;
}

static void
LongDelay(void)
{
    for (volatile uint64_t i = 0; i < 300000000ULL; i++) {
    }
}

static void
ClearWakeSources(void)
{
    uint16_t Half = (uint16_t)(Pm1EvtLen / 2);

    if (Pm1aEvt != 0 && Half >= 2) {
        OutW(Pm1aEvt, 0xFFFF);
        OutW((uint16_t)(Pm1aEvt + Half), 0);
    }

    if (Pm1bEvt != 0 && Half >= 2) {
        OutW(Pm1bEvt, 0xFFFF);
        OutW((uint16_t)(Pm1bEvt + Half), 0);
    }

    if (Gpe0Blk != 0) {
        uint16_t GpeHalf = (uint16_t)(Gpe0Len / 2);
        for (uint16_t i = 0; i < GpeHalf; i++) {
            OutB((uint16_t)(Gpe0Blk + i), 0xFF);
            OutB((uint16_t)(Gpe0Blk + GpeHalf + i), 0);
        }
    }

    if (Gpe1Blk != 0) {
        uint16_t GpeHalf = (uint16_t)(Gpe1Len / 2);
        for (uint16_t i = 0; i < GpeHalf; i++) {
            OutB((uint16_t)(Gpe1Blk + i), 0xFF);
            OutB((uint16_t)(Gpe1Blk + GpeHalf + i), 0);
        }
    }
}

static void
PrintHex(const char *Label, uint64_t Value)
{
    ConsolePrint(Label);
    ConsolePrint("0x");
    ConsolePrintHex(Value, 1);
}

int
PowerShutdown(void)
{
    if (!Ready || !S5Found || Pm1aCnt == 0) {
        return 0;
    }

    PrintHex("pm1a cnt ", Pm1aCnt);
    PrintHex("  pm1b cnt ", Pm1bCnt);
    PrintHex("  s5 typ ", SlpTypA >> 10);
    PrintHex(",", SlpTypB >> 10);
    ConsolePutChar('\n');
    PrintHex("pm1a evt ", Pm1aEvt);
    PrintHex("  evt len ", Pm1EvtLen);
    PrintHex("  gpe0 ", Gpe0Blk);
    PrintHex(" len ", Gpe0Len);
    PrintHex("  pm1 cnt now ", InW(Pm1aCnt));
    PrintHex("  pm1 sts now ", Pm1aEvt != 0 ? InW(Pm1aEvt) : 0);
    ConsolePutChar('\n');
    ConsoleFlush();

    Sleep(4000);

    __asm__ __volatile__("cli");

    if (SmiCmd != 0 && AcpiEnableValue != 0 && !(InW(Pm1aCnt) & 1)) {
        OutB(SmiCmd, AcpiEnableValue);

        for (int i = 0; i < 300 && !(InW(Pm1aCnt) & 1); i++) {
            for (volatile int j = 0; j < 1000000; j++) {
            }
        }
    }

    ClearWakeSources();

    __asm__ __volatile__("wbinvd" : : : "memory");

    uint16_t ControlA = (uint16_t)(InW(Pm1aCnt) & ~(0x1C00 | SLP_EN));
    OutW(Pm1aCnt, (uint16_t)(ControlA | SlpTypA));

    if (Pm1bCnt != 0) {
        uint16_t ControlB = (uint16_t)(InW(Pm1bCnt) & ~(0x1C00 | SLP_EN));
        OutW(Pm1bCnt, (uint16_t)(ControlB | SlpTypB));
    }

    OutW(Pm1aCnt, (uint16_t)(ControlA | SlpTypA | SLP_EN));

    if (Pm1bCnt != 0) {
        uint16_t ControlB = (uint16_t)(InW(Pm1bCnt) & ~(0x1C00 | SLP_EN));
        OutW(Pm1bCnt, (uint16_t)(ControlB | SlpTypB | SLP_EN));
    }

    LongDelay();

    __asm__ __volatile__("sti");
    return 0;
}

void
PowerReboot(void)
{
    __asm__ __volatile__("cli");

    if (Ready && ResetSupported) {
        if (ResetSpace == 1) {
            OutB((uint16_t)ResetAddress, ResetValue);
        } else {
            *(volatile uint8_t *)(uintptr_t)ResetAddress = ResetValue;
        }

        LongDelay();
    }

    OutB(0xCF9, 0x02);
    OutB(0xCF9, 0x06);
    LongDelay();

    for (int i = 0; i < 100000 && (InB(0x64) & 0x02); i++) {
    }
    OutB(0x64, 0xFE);
    LongDelay();

    struct {
        uint16_t Limit;
        uint64_t Base;
    } __attribute__((packed)) NullIdt = {0, 0};

    __asm__ __volatile__("lidt %0; int3" : : "m"(NullIdt));

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
