#include <stdint.h>
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "pmm.h"
#include "blockdev.h"
#include "hda.h"
#include "io.h"

#define LINE_MAX 256
#define HISTORY_SIZE 16
#define MAX_ARGS 16

typedef void (*COMMAND_FN)(int Argc, char **Argv);

typedef struct {
    const char *Name;
    const char *Help;
    COMMAND_FN Fn;
} COMMAND;

HDA_STATUS GlobalHdaStatus;
int GlobalHdaFound;

static char Line[LINE_MAX];
static int LineLen;
static char History[HISTORY_SIZE][LINE_MAX];
static int HistoryNext;
static int HistoryCount;
static int HistoryPos;

static int
StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void
StrCopy(char *Dst, const char *Src)
{
    while (*Src) {
        *Dst++ = *Src++;
    }
    *Dst = 0;
}

static void
PrintKeyValue(const char *Key, uint64_t Value)
{
    ConsolePrint(Key);
    ConsolePrint(": ");
    ConsolePrintUInt(Value);
}

static uint32_t
PciRead(uint8_t Bus, uint8_t Dev, uint8_t Fn, uint8_t Off)
{
    OutL(0xCF8, 0x80000000u | ((uint32_t)Bus << 16) | ((uint32_t)Dev << 11) | ((uint32_t)Fn << 8) | (Off & 0xFC));
    return InL(0xCFC);
}

static const char *
PciClassName(uint8_t Base, uint8_t Sub, uint8_t Prog)
{
    switch (Base) {
    case 0x01:
        if (Sub == 0x08) return "NVMe";
        if (Sub == 0x06) return "SATA";
        if (Sub == 0x01) return "IDE";
        return "Storage";
    case 0x02:
        return "Network";
    case 0x03:
        return "Display";
    case 0x04:
        if (Sub == 0x03) return "Audio (HDA)";
        if (Sub == 0x01) return "Audio";
        return "Multimedia";
    case 0x05:
        return "Memory";
    case 0x06:
        if (Sub == 0x00) return "Host bridge";
        if (Sub == 0x01) return "ISA bridge";
        if (Sub == 0x04) return "PCI bridge";
        return "Bridge";
    case 0x07:
        return "Communication";
    case 0x08:
        return "System";
    case 0x09:
        return "Input";
    case 0x0C:
        if (Sub == 0x03) {
            if (Prog == 0x00) return "USB UHCI";
            if (Prog == 0x10) return "USB OHCI";
            if (Prog == 0x20) return "USB EHCI";
            if (Prog == 0x30) return "USB xHCI";
            return "USB";
        }
        if (Sub == 0x05) return "SMBus";
        return "Serial bus";
    case 0x0D:
        return "Wireless";
    default:
        return "Other";
    }
}

static void CmdHelp(int Argc, char **Argv);

static void
CmdClear(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;
    ConsoleClear();
}

static void
CmdEcho(int Argc, char **Argv)
{
    for (int i = 1; i < Argc; i++) {
        if (i > 1) {
            ConsolePutChar(' ');
        }
        ConsolePrint(Argv[i]);
    }
    ConsolePutChar('\n');
}

static void
CmdMem(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    uint64_t Free = GetFreePageCount();
    uint64_t Total = GetTotalPageCount();

    ConsolePrint("Pages free/total: ");
    ConsolePrintUInt(Free);
    ConsolePrint(" / ");
    ConsolePrintUInt(Total);
    ConsolePutChar('\n');
    ConsolePrint("Memory free/total: ");
    ConsolePrintUInt(Free / 256);
    ConsolePrint(" MB / ");
    ConsolePrintUInt(Total / 256);
    ConsolePrint(" MB\n");
}

static void
CmdLspci(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    for (int Bus = 0; Bus < 256; Bus++) {
        for (int Dev = 0; Dev < 32; Dev++) {
            uint32_t First = PciRead((uint8_t)Bus, (uint8_t)Dev, 0, 0);

            if ((First & 0xFFFF) == 0xFFFF) {
                continue;
            }

            uint8_t Header = (uint8_t)((PciRead((uint8_t)Bus, (uint8_t)Dev, 0, 0x0C) >> 16) & 0xFF);
            int FnCount = (Header & 0x80) ? 8 : 1;

            for (int Fn = 0; Fn < FnCount; Fn++) {
                uint32_t Id = PciRead((uint8_t)Bus, (uint8_t)Dev, (uint8_t)Fn, 0);

                if ((Id & 0xFFFF) == 0xFFFF) {
                    continue;
                }

                uint32_t Class = PciRead((uint8_t)Bus, (uint8_t)Dev, (uint8_t)Fn, 0x08);

                ConsolePrintHex((uint64_t)Bus, 2);
                ConsolePutChar(':');
                ConsolePrintHex((uint64_t)Dev, 2);
                ConsolePutChar('.');
                ConsolePrintHex((uint64_t)Fn, 1);
                ConsolePrint("  ");
                ConsolePrintHex(Id & 0xFFFF, 4);
                ConsolePutChar(':');
                ConsolePrintHex(Id >> 16, 4);
                ConsolePrint("  ");
                ConsolePrint(PciClassName((uint8_t)(Class >> 24), (uint8_t)(Class >> 16), (uint8_t)(Class >> 8)));
                ConsolePutChar('\n');
            }
        }
    }
}

static void
CmdDisks(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    if (BlockDeviceReady()) {
        ConsolePrint("Active block device: ");
        ConsolePrint(BlockDeviceName());
        ConsolePutChar('\n');
    } else {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("No block device\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
    }
}

static void
CmdHda(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    if (!GlobalHdaFound) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("HDA controller not found\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    PrintKeyValue("codec", (uint64_t)GlobalHdaStatus.CodecFound);
    ConsolePrint("  ");
    PrintKeyValue("afg", (uint64_t)GlobalHdaStatus.AfgFound);
    ConsolePutChar('\n');
    PrintKeyValue("widgets", GlobalHdaStatus.WidgetCount);
    ConsolePrint("  ");
    PrintKeyValue("dac", (uint64_t)GlobalHdaStatus.DacFound);
    ConsolePrint("  ");
    PrintKeyValue("pin", (uint64_t)GlobalHdaStatus.PinFound);
    ConsolePutChar('\n');
    PrintKeyValue("path", (uint64_t)GlobalHdaStatus.PathLinked);
    ConsolePrint("  ");
    PrintKeyValue("stream", (uint64_t)GlobalHdaStatus.StreamStarted);
    ConsolePutChar('\n');
}

static void
CmdBeep(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    if (!GlobalHdaFound || !GlobalHdaStatus.StreamStarted) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("HDA stream is not available\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    ConsolePrint("Playing test tone...\n");
    ConsoleFlush();
    HdaPlayTestTone();
}

static void
CmdReboot(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    ConsolePrint("Rebooting...\n");
    ConsoleFlush();

    OutB(0x64, 0xFE);

    for (volatile int i = 0; i < 100000000; i++) {
    }

    struct {
        uint16_t Limit;
        uint64_t Base;
    } __attribute__((packed)) NullIdt = {0, 0};

    __asm__ __volatile__("lidt %0; int3" : : "m"(NullIdt));
}

static const COMMAND Commands[] = {
    {"help",   "list commands",             CmdHelp},
    {"clear",  "clear the screen",          CmdClear},
    {"echo",   "print arguments",           CmdEcho},
    {"mem",    "show memory usage",         CmdMem},
    {"lspci",  "list PCI devices",          CmdLspci},
    {"disks",  "show active block device",  CmdDisks},
    {"hda",    "show audio controller state", CmdHda},
    {"beep",   "play test tone via HDA",    CmdBeep},
    {"reboot", "restart the machine",       CmdReboot},
};

#define COMMAND_COUNT ((int)(sizeof(Commands) / sizeof(Commands[0])))

static void
CmdHelp(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    for (int i = 0; i < COMMAND_COUNT; i++) {
        ConsoleSetAttr(CONSOLE_ATTR_ACCENT);
        ConsolePrint(Commands[i].Name);
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);

        int Pad = 10;
        for (const char *p = Commands[i].Name; *p; p++) {
            Pad--;
        }
        while (Pad-- > 0) {
            ConsolePutChar(' ');
        }

        ConsolePrint(Commands[i].Help);
        ConsolePutChar('\n');
    }
}

static void
Execute(void)
{
    static char Work[LINE_MAX];
    char *Argv[MAX_ARGS];
    int Argc = 0;

    StrCopy(Work, Line);

    char *p = Work;
    while (*p && Argc < MAX_ARGS) {
        while (*p == ' ') {
            *p++ = 0;
        }
        if (!*p) {
            break;
        }
        Argv[Argc++] = p;
        while (*p && *p != ' ') {
            p++;
        }
    }

    if (Argc == 0) {
        return;
    }

    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (StrEq(Argv[0], Commands[i].Name)) {
            Commands[i].Fn(Argc, Argv);
            return;
        }
    }

    ConsoleSetAttr(CONSOLE_ATTR_ERROR);
    ConsolePrint("Unknown command: ");
    ConsolePrint(Argv[0]);
    ConsolePutChar('\n');
    ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
}

static void
PrintPrompt(void)
{
    ConsoleSetAttr(CONSOLE_ATTR_ACCENT);
    ConsolePrint("bloom> ");
    ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
}

static void
HistoryAdd(void)
{
    if (LineLen == 0) {
        return;
    }

    if (HistoryCount > 0) {
        int Last = (HistoryNext + HISTORY_SIZE - 1) % HISTORY_SIZE;
        if (StrEq(History[Last], Line)) {
            return;
        }
    }

    StrCopy(History[HistoryNext], Line);
    HistoryNext = (HistoryNext + 1) % HISTORY_SIZE;

    if (HistoryCount < HISTORY_SIZE) {
        HistoryCount++;
    }
}

static void
ReplaceInput(const char *Text)
{
    while (LineLen > 0) {
        ConsolePutChar('\b');
        LineLen--;
    }

    while (*Text && LineLen < LINE_MAX - 1) {
        Line[LineLen++] = *Text;
        ConsolePutChar(*Text);
        Text++;
    }

    Line[LineLen] = 0;
}

static void
HistoryStep(int Direction)
{
    if (Direction > 0) {
        if (HistoryPos < HistoryCount) {
            HistoryPos++;
        } else {
            return;
        }
    } else {
        if (HistoryPos > 0) {
            HistoryPos--;
        } else {
            return;
        }
    }

    if (HistoryPos == 0) {
        ReplaceInput("");
        return;
    }

    int Index = (HistoryNext + HISTORY_SIZE - HistoryPos) % HISTORY_SIZE;
    ReplaceInput(History[Index]);
}

void
ShellRun(void)
{
    ConsoleSetAttr(CONSOLE_ATTR_ACCENT);
    ConsolePrint("Bloom-OS console\n");
    ConsoleSetAttr(CONSOLE_ATTR_DIM);
    ConsolePrint("Type 'help' for a list of commands.\n\n");
    ConsoleSetAttr(CONSOLE_ATTR_NORMAL);

    PrintPrompt();
    ConsoleFlush();

    for (;;) {
        int Key = KeyboardGetKey();

        if (Key == 0) {
            __asm__ __volatile__("hlt");
            continue;
        }

        if (Key == '\n') {
            ConsolePutChar('\n');
            Line[LineLen] = 0;
            HistoryAdd();
            Execute();
            LineLen = 0;
            Line[0] = 0;
            HistoryPos = 0;
            PrintPrompt();
        } else if (Key == '\b') {
            if (LineLen > 0) {
                LineLen--;
                ConsolePutChar('\b');
            }
        } else if (Key == KEY_UP) {
            HistoryStep(1);
        } else if (Key == KEY_DOWN) {
            HistoryStep(-1);
        } else if (Key >= 32 && Key <= 126) {
            if (LineLen < LINE_MAX - 1) {
                Line[LineLen++] = (char)Key;
                ConsolePutChar((char)Key);
            }
        }

        ConsoleFlush();
    }
}
