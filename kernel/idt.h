#pragma once

#include <stdint.h>

typedef void (*IRQ_HANDLER)(void);

extern volatile uint64_t TickCount;

void InitIdt(void);
void EnableInterrupts(void);
void DisableInterrupts(void);
void Sleep(uint64_t Milliseconds);
void RegisterIrqHandler(int Irq, IRQ_HANDLER Handler);
void UnmaskIrq(int Irq);
