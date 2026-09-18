#include <stdint.h>
#include "idt.h"

typedef struct __attribute__((packed)) {
    uint16_t OffsetLow;
    uint16_t Selector;
    uint8_t  Ist;
    uint8_t  TypeAttr;
    uint16_t OffsetMid;
    uint32_t OffsetHigh;
    uint32_t Zero;
} IDT_ENTRY;

typedef struct __attribute__((packed)) {
    uint16_t Limit;
    uint64_t Base;
} IDT_POINTER;

typedef struct {
    uint64_t Ip;
    uint64_t Cs;
    uint64_t Flags;
    uint64_t Sp;
    uint64_t Ss;
} INTERRUPT_FRAME;

static IDT_ENTRY Idt[256];
static IDT_POINTER IdtPtr;
static IRQ_HANDLER IrqHandlers[16];

volatile uint64_t TickCount = 0;

static void
OutB(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t
InB(uint16_t port)
{
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint16_t
ReadCs(void)
{
    uint16_t Cs;
    __asm__ __volatile__("mov %%cs, %0" : "=r"(Cs));
    return Cs;
}

static void
SetGate(int Num, void *Handler, uint16_t Selector, uint8_t TypeAttr)
{
    uint64_t Addr = (uint64_t)Handler;

    Idt[Num].OffsetLow = Addr & 0xFFFF;
    Idt[Num].Selector = Selector;
    Idt[Num].Ist = 0;
    Idt[Num].TypeAttr = TypeAttr;
    Idt[Num].OffsetMid = (Addr >> 16) & 0xFFFF;
    Idt[Num].OffsetHigh = (Addr >> 32) & 0xFFFFFFFF;
    Idt[Num].Zero = 0;
}

static void
DisableLocalApic(void)
{
    uint32_t Low, High;

    __asm__ __volatile__("rdmsr" : "=a"(Low), "=d"(High) : "c"(0x1B));
    Low &= ~(1u << 11);
    __asm__ __volatile__("wrmsr" : : "c"(0x1B), "a"(Low), "d"(High));
}

static void
ForcePicMode(void)
{
    OutB(0x22, 0x70);
    OutB(0x23, 0x01);
}

static void
RemapPic(void)
{
    DisableLocalApic();
    ForcePicMode();

    OutB(0x20, 0x11);
    OutB(0xA0, 0x11);
    OutB(0x21, 0x20);
    OutB(0xA1, 0x28);
    OutB(0x21, 0x04);
    OutB(0xA1, 0x02);
    OutB(0x21, 0x01);
    OutB(0xA1, 0x01);
    OutB(0x21, 0xFF);
    OutB(0xA1, 0xFF);
}

static void
InitPit(uint32_t Frequency)
{
    uint32_t Divisor = 1193182 / Frequency;

    OutB(0x43, 0x36);
    OutB(0x40, (uint8_t)(Divisor & 0xFF));
    OutB(0x40, (uint8_t)((Divisor >> 8) & 0xFF));
}

void
IrqDispatchC(int Irq)
{
    if (Irq == 0) {
        TickCount++;
    }

    if (IrqHandlers[Irq]) {
        IrqHandlers[Irq]();
    }

    if (Irq >= 8) {
        OutB(0xA0, 0x20);
    }
    OutB(0x20, 0x20);
}

extern void Irq0Stub(void);
extern void Irq1Stub(void);
extern void Irq2Stub(void);
extern void Irq3Stub(void);
extern void Irq4Stub(void);
extern void Irq5Stub(void);
extern void Irq6Stub(void);
extern void Irq7Stub(void);
extern void Irq8Stub(void);
extern void Irq9Stub(void);
extern void Irq10Stub(void);
extern void Irq11Stub(void);
extern void Irq12Stub(void);
extern void Irq13Stub(void);
extern void Irq14Stub(void);
extern void Irq15Stub(void);

void
InitIdt(void)
{
    uint16_t Cs = ReadCs();

    for (int i = 0; i < 256; i++) {
        SetGate(i, 0, 0, 0);
    }

    SetGate(32, (void *)Irq0Stub, Cs, 0x8E);
    SetGate(33, (void *)Irq1Stub, Cs, 0x8E);
    SetGate(34, (void *)Irq2Stub, Cs, 0x8E);
    SetGate(35, (void *)Irq3Stub, Cs, 0x8E);
    SetGate(36, (void *)Irq4Stub, Cs, 0x8E);
    SetGate(37, (void *)Irq5Stub, Cs, 0x8E);
    SetGate(38, (void *)Irq6Stub, Cs, 0x8E);
    SetGate(39, (void *)Irq7Stub, Cs, 0x8E);
    SetGate(40, (void *)Irq8Stub, Cs, 0x8E);
    SetGate(41, (void *)Irq9Stub, Cs, 0x8E);
    SetGate(42, (void *)Irq10Stub, Cs, 0x8E);
    SetGate(43, (void *)Irq11Stub, Cs, 0x8E);
    SetGate(44, (void *)Irq12Stub, Cs, 0x8E);
    SetGate(45, (void *)Irq13Stub, Cs, 0x8E);
    SetGate(46, (void *)Irq14Stub, Cs, 0x8E);
    SetGate(47, (void *)Irq15Stub, Cs, 0x8E);

    RemapPic();
    InitPit(1000);
    UnmaskIrq(0);

    IdtPtr.Limit = sizeof(Idt) - 1;
    IdtPtr.Base = (uint64_t)&Idt;
    __asm__ __volatile__("lidt %0" : : "m"(IdtPtr));
}

void
EnableInterrupts(void)
{
    __asm__ __volatile__("sti");
}

void
DisableInterrupts(void)
{
    __asm__ __volatile__("cli");
}

void
Sleep(uint64_t Milliseconds)
{
    uint64_t Target = TickCount + Milliseconds;

    while (TickCount < Target) {
        __asm__ __volatile__("hlt");
    }
}

void
RegisterIrqHandler(int Irq, IRQ_HANDLER Handler)
{
    IrqHandlers[Irq] = Handler;
}

void
UnmaskIrq(int Irq)
{
    if (Irq < 8) {
        uint8_t Mask = InB(0x21);
        Mask &= ~(1 << Irq);
        OutB(0x21, Mask);
    } else {
        uint8_t Mask = InB(0xA1);
        Mask &= ~(1 << (Irq - 8));
        OutB(0xA1, Mask);

        Mask = InB(0x21);
        Mask &= ~(1 << 2);
        OutB(0x21, Mask);
    }
}
