#pragma once

#include <stdint.h>

#define CONSOLE_ATTR_NORMAL 0
#define CONSOLE_ATTR_ACCENT 1
#define CONSOLE_ATTR_ERROR  2
#define CONSOLE_ATTR_DIM    3

void ConsoleInit(void);
void ConsoleClear(void);
void ConsoleSetAttr(uint8_t Attr);
void ConsolePutChar(char c);
void ConsolePrint(const char *s);
void ConsolePrintUInt(uint64_t Value);
void ConsolePrintHex(uint64_t Value, int MinDigits);
void ConsoleFlush(void);
