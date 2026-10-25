#include <stdint.h>
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "player.h"
#include "explorer.h"
#include "pmm.h"
#include "blockdev.h"
#include "hda.h"
#include "klog.h"
#include "power.h"
#include "fs.h"
#include "part.h"
#include "io.h"
#include "xhci.h"
#include "usbmsc.h"

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

    if (BlockDeviceCount() == 0) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("No block devices\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    for (int i = 0; i < BlockDeviceCount(); i++) {
        ConsolePrint(BlockDeviceName(i));
        ConsolePutChar('\n');
    }
}

static void
PrintPadded(const char *Text, int Width)
{
    int Length = 0;

    while (Text[Length]) {
        ConsolePutChar(Text[Length]);
        Length++;
    }

    while (Length < Width) {
        ConsolePutChar(' ');
        Length++;
    }
}

static int
AppendUInt(char *Out, int Pos, uint64_t Value)
{
    char Digits[21];
    int N = 0;

    do {
        Digits[N++] = (char)('0' + (Value % 10));
        Value /= 10;
    } while (Value > 0);

    while (N > 0) {
        Out[Pos++] = Digits[--N];
    }

    return Pos;
}

static int
AppendText(char *Out, int Pos, const char *Text)
{
    while (*Text) {
        Out[Pos++] = *Text++;
    }
    return Pos;
}

static void
FormatSize(char *Out, uint64_t Sectors)
{
    uint64_t Bytes = Sectors * 512;
    int Pos = 0;

    if (Sectors == 0) {
        Out[0] = '-';
        Out[1] = 0;
        return;
    }

    if (Bytes >= (1ULL << 30)) {
        uint64_t Mb = Bytes >> 20;
        uint64_t Tenths = Mb * 10 / 1024;
        Pos = AppendUInt(Out, Pos, Tenths / 10);
        Out[Pos++] = '.';
        Pos = AppendUInt(Out, Pos, Tenths % 10);
        Pos = AppendText(Out, Pos, " GB");
    } else if (Bytes >= (1ULL << 20)) {
        Pos = AppendUInt(Out, Pos, Bytes >> 20);
        Pos = AppendText(Out, Pos, " MB");
    } else {
        Pos = AppendUInt(Out, Pos, Bytes >> 10);
        Pos = AppendText(Out, Pos, " KB");
    }

    Out[Pos] = 0;
}

static int
IsMounted(const char *PartName)
{
    for (int v = 0; v < FsVolumeCount(); v++) {
        if (StrEq(FsVolumeName(v), PartName)) {
            return 1;
        }
    }
    return 0;
}

static void
CmdLsblk(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    if (BlockDeviceCount() == 0) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("No block devices\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    ConsoleSetAttr(CONSOLE_ATTR_DIM);
    PrintPadded("NAME", 12);
    PrintPadded("SIZE", 11);
    PrintPadded("TABLE", 7);
    PrintPadded("TYPE", 22);
    PrintPadded("FS", 10);
    ConsolePrint("STATE\n");
    ConsoleSetAttr(CONSOLE_ATTR_NORMAL);

    for (int d = 0; d < BlockDeviceCount(); d++) {
        ConsoleSetAttr(CONSOLE_ATTR_ACCENT);
        ConsolePrint(BlockDeviceName(d));
        ConsolePutChar('\n');
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);

        for (int i = 0; i < PartCount(); i++) {
            const PART_INFO *P = PartGet(i);

            if (P->Device != d) {
                continue;
            }

            char Size[24];
            FormatSize(Size, P->SectorCount);

            ConsolePrint("  ");
            PrintPadded(P->Name, 10);
            PrintPadded(Size, 11);
            PrintPadded(P->Scheme == PART_SCHEME_GPT ? "GPT" : (P->Scheme == PART_SCHEME_MBR ? "MBR" : "none"), 7);
            PrintPadded(PartTypeName(P), 22);
            PrintPadded(PartFsName(P->Fs), 10);
            ConsolePrint(IsMounted(P->Name) ? "mounted" : "-");
            ConsolePutChar('\n');
        }
    }
}

static void
PrintFsError(int Code)
{
    ConsoleSetAttr(CONSOLE_ATTR_ERROR);
    ConsolePrint(FsErrorText(Code));
    ConsolePutChar('\n');
    ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
}

static void
PrintEntryLine(const FAT_ENTRY *E)
{
    char Flags[5];
    Flags[0] = (E->Attr & FAT_ATTR_READ_ONLY) ? 'r' : '-';
    Flags[1] = (E->Attr & FAT_ATTR_HIDDEN) ? 'h' : '-';
    Flags[2] = (E->Attr & FAT_ATTR_SYSTEM) ? 's' : '-';
    Flags[3] = ' ';
    Flags[4] = 0;
    ConsolePrint(Flags);

    if (E->Attr & FAT_ATTR_DIRECTORY) {
        ConsoleSetAttr(CONSOLE_ATTR_ACCENT);
        PrintPadded("<DIR>", 11);
        ConsolePrint(E->Name);
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
    } else {
        char Text[16];
        int Len = AppendUInt(Text, 0, E->Size);
        Text[Len] = 0;
        for (int i = Len; i < 10; i++) {
            ConsolePutChar(' ');
        }
        ConsolePrint(Text);
        ConsolePutChar(' ');
        ConsolePrint(E->Name);
    }

    ConsolePutChar('\n');
}

static int
LsCallback(const FAT_ENTRY *Entry, void *Context)
{
    (*(int *)Context)++;
    PrintEntryLine(Entry);
    return 0;
}

static void
CmdLs(int Argc, char **Argv)
{
    static FS_NODE Node;

    if (FsVolumeCount() == 0) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("No FAT32 volumes mounted (see lsblk)\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    int Status = FsResolve(Argc > 1 ? Argv[1] : ".", &Node);

    if (Status != FS_OK) {
        PrintFsError(Status);
        return;
    }

    if (!Node.IsDir) {
        PrintEntryLine(&Node.Entry);
        return;
    }

    int Count = 0;
    Status = FsList(&Node, LsCallback, &Count);

    if (Status != FS_OK) {
        PrintFsError(Status);
        return;
    }

    ConsoleSetAttr(CONSOLE_ATTR_DIM);
    ConsolePrintUInt((uint64_t)Count);
    ConsolePrint(Count == 1 ? " entry\n" : " entries\n");
    ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
}

static void
CmdPwd(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    if (!FsHasCwd()) {
        PrintFsError(FS_ERR_NOVOLUME);
        return;
    }

    ConsolePrint(FsCwdVolume());
    ConsolePutChar(':');
    ConsolePrint(FsCwdPath());
    ConsolePutChar('\n');
}

static void
CmdCd(int Argc, char **Argv)
{
    if (Argc < 2) {
        CmdPwd(Argc, Argv);
        return;
    }

    int Status = FsChdir(Argv[1]);

    if (Status != FS_OK) {
        PrintFsError(Status);
    }
}

static int
ParseNumber(const char *Text, uint32_t *Out)
{
    uint32_t Value = 0;
    int Base = 10;

    if (Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X')) {
        Base = 16;
        Text += 2;
    }

    if (*Text == 0) {
        return 0;
    }

    while (*Text) {
        char c = *Text++;
        uint32_t Digit;

        if (c >= '0' && c <= '9') {
            Digit = (uint32_t)(c - '0');
        } else if (Base == 16 && c >= 'a' && c <= 'f') {
            Digit = (uint32_t)(c - 'a' + 10);
        } else if (Base == 16 && c >= 'A' && c <= 'F') {
            Digit = (uint32_t)(c - 'A' + 10);
        } else {
            return 0;
        }

        Value = Value * (uint32_t)Base + Digit;
    }

    *Out = Value;
    return 1;
}

static void
CmdHexdump(int Argc, char **Argv)
{
    static FS_NODE Node;
    static uint8_t Bytes[512];

    if (Argc < 2) {
        ConsolePrint("usage: hexdump <file> [offset] [length]\n");
        return;
    }

    uint32_t Offset = 0;
    uint32_t Length = 128;

    if (Argc > 2 && !ParseNumber(Argv[2], &Offset)) {
        ConsolePrint("bad offset\n");
        return;
    }

    if (Argc > 3 && !ParseNumber(Argv[3], &Length)) {
        ConsolePrint("bad length\n");
        return;
    }

    if (Length > sizeof(Bytes)) {
        Length = sizeof(Bytes);
    }

    int Status = FsResolve(Argv[1], &Node);

    if (Status != FS_OK) {
        PrintFsError(Status);
        return;
    }

    if (Node.IsDir) {
        ConsolePrint("is a directory\n");
        return;
    }

    uint32_t Got = FsRead(&Node, Offset, Bytes, Length);

    for (uint32_t i = 0; i < Got; i += 16) {
        ConsolePrintHex(Offset + i, 8);
        ConsolePrint("  ");

        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < Got) {
                ConsolePrintHex(Bytes[i + j], 2);
                ConsolePutChar(' ');
            } else {
                ConsolePrint("   ");
            }
        }

        ConsolePutChar(' ');

        for (uint32_t j = 0; j < 16 && i + j < Got; j++) {
            uint8_t c = Bytes[i + j];
            ConsolePutChar((c >= 32 && c <= 126) ? (char)c : '.');
        }

        ConsolePutChar('\n');
    }

    if (Got == 0) {
        ConsolePrint("nothing to read at this offset\n");
    }
}

static void
CmdCat(int Argc, char **Argv)
{
    static FS_NODE Node;
    static uint8_t Chunk[1024];

    if (Argc < 2) {
        ConsolePrint("usage: cat <file>\n");
        return;
    }

    int Status = FsResolve(Argv[1], &Node);

    if (Status != FS_OK) {
        PrintFsError(Status);
        return;
    }

    if (Node.IsDir) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("is a directory\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    uint32_t Offset = 0;
    uint32_t Limit = 65536;

    while (Offset < Limit) {
        uint32_t Got = FsRead(&Node, Offset, Chunk, sizeof(Chunk));

        if (Got == 0) {
            break;
        }

        for (uint32_t i = 0; i < Got; i++) {
            uint8_t c = Chunk[i];

            if (c == '\n' || c == '\t') {
                ConsolePutChar((char)c);
            } else if (c >= 32 && c <= 126) {
                ConsolePutChar((char)c);
            } else if (c != '\r') {
                ConsolePutChar('.');
            }
        }

        Offset += Got;
    }

    if (Offset < Node.Entry.Size) {
        ConsoleSetAttr(CONSOLE_ATTR_DIM);
        ConsolePrint("\n[truncated at 64 KB]");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
    }

    ConsolePutChar('\n');
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
    PrintKeyValue("corb", GlobalHdaStatus.CorbEntriesUsed);
    ConsolePrint("  ");
    PrintKeyValue("rirb", GlobalHdaStatus.RirbEntriesUsed);
    ConsolePutChar('\n');
    PrintKeyValue("fgcount", GlobalHdaStatus.RootFgCount);
    ConsolePrint("  ");
    PrintKeyValue("fgstart", GlobalHdaStatus.FgStartNid);
    ConsolePrint("  ");
    PrintKeyValue("afg", (uint64_t)GlobalHdaStatus.AfgFound);
    ConsolePrint("  fgtype: 0x");
    ConsolePrintHex(GlobalHdaStatus.LastFgTypeRaw, 1);
    ConsolePutChar('\n');
    PrintKeyValue("wp1", GlobalHdaStatus.WpAfterFirst);
    ConsolePrint("  ");
    PrintKeyValue("wp2", GlobalHdaStatus.WpBeforeSecond);
    ConsolePrint("  ");
    PrintKeyValue("corbwp", GlobalHdaStatus.CorbWpReadback);
    ConsolePrint("  ");
    PrintKeyValue("corbrp", GlobalHdaStatus.CorbRpAfterFirst);
    ConsolePrint("  ");
    PrintKeyValue("corbsts", GlobalHdaStatus.CorbSts);
    ConsolePrint("  repeat: 0x");
    ConsolePrintHex(GlobalHdaStatus.RepeatSameCallRaw, 1);
    ConsolePutChar('\n');
    PrintKeyValue("widgets", GlobalHdaStatus.WidgetCount);
    ConsolePrint("  ");
    PrintKeyValue("dac", (uint64_t)GlobalHdaStatus.DacFound);
    ConsolePrint("  ");
    PrintKeyValue("pin", (uint64_t)GlobalHdaStatus.PinFound);
    ConsolePrint("  ");
    PrintKeyValue("path", (uint64_t)GlobalHdaStatus.PathLinked);
    ConsolePrint("  ");
    PrintKeyValue("stream", (uint64_t)GlobalHdaStatus.StreamStarted);
    ConsolePutChar('\n');
}

static char
LowerChar(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int
LineHasPrefix(const char *Line, const char *Prefix)
{
    while (*Prefix) {
        if (*Line == 0 || *Line == '\n' || LowerChar(*Line) != LowerChar(*Prefix)) {
            return 0;
        }
        Line++;
        Prefix++;
    }
    return 1;
}

static void
CmdDmesg(int Argc, char **Argv)
{
    const char *Text = KLogText();

    if (Argc < 2) {
        ConsolePrint(Text);
        return;
    }

    while (*Text) {
        int Match = LineHasPrefix(Text, Argv[1]);

        while (*Text && *Text != '\n') {
            if (Match) {
                ConsolePutChar(*Text);
            }
            Text++;
        }

        if (*Text == '\n') {
            if (Match) {
                ConsolePutChar('\n');
            }
            Text++;
        }
    }
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
CmdPlay(int Argc, char **Argv)
{
    if (Argc < 2) {
        ConsolePrint("usage: play <file.wav>\n");
        return;
    }

    if (!GlobalHdaFound || !GlobalHdaStatus.StreamStarted) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("HDA stream is not available\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    PlayWavFile(Argv[1]);
}

static void
CmdExplorer(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    ExplorerRun();
}

static void
CmdReboot(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    ConsolePrint("Rebooting...\n");
    ConsoleFlush();
    PowerReboot();
}

static void
CmdShutdown(int Argc, char **Argv)
{
    (void)Argc;
    (void)Argv;

    ConsolePrint("Shutting down...\n");
    ConsoleFlush();

    if (!PowerShutdown()) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("Shutdown failed: ACPI power off is not available\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
    }
}

static int
AppendHex4(char *Out, int Pos, uint16_t Value)
{
    for (int Shift = 12; Shift >= 0; Shift -= 4) {
        uint8_t Nib = (uint8_t)((Value >> Shift) & 0xF);
        Out[Pos++] = (char)(Nib < 10 ? '0' + Nib : 'a' + Nib - 10);
    }
    return Pos;
}

static void
CmdUsb(int Argc, char **Argv)
{
    if (Argc > 1 && StrEq(Argv[1], "list")) {
        int Shown = 0;

        for (int i = 0; i < XhciDeviceCount(); i++) {
            USB_DEVICE *D = XhciDevice(i);

            if (!D) {
                continue;
            }

            char Text[96];
            int Pos = 0;

            Pos = AppendText(Text, Pos, "port ");
            Pos = AppendUInt(Text, Pos, (uint64_t)D->Port);
            Pos = AppendText(Text, Pos, "  ");
            Pos = AppendText(Text, Pos, UsbSpeedName(D->Speed));
            Pos = AppendText(Text, Pos, "  ");
            Pos = AppendHex4(Text, Pos, D->VendorId);
            Pos = AppendText(Text, Pos, ":");
            Pos = AppendHex4(Text, Pos, D->ProductId);
            Pos = AppendText(Text, Pos, "  ");

            if (D->HubSeen) {
                Pos = AppendText(Text, Pos, "hub (unsupported)");
            } else if (D->IsMass) {
                char Names[48];
                UsbMscBlocks(D, Names, 48);
                Pos = AppendText(Text, Pos, "storage ");
                Pos = AppendText(Text, Pos, Names);
            } else {
                Pos = AppendText(Text, Pos, "other");
            }

            Text[Pos] = 0;
            ConsolePrint(Text);
            ConsolePutChar('\n');
            Shown++;
        }

        if (!Shown) {
            ConsolePrint("No USB devices enumerated. Run 'usb' to scan.\n");
        }

        return;
    }

    int New[32];
    int Removed = 0;

    ConsolePrint("Scanning USB...\n");

    int Count = UsbScan(New, 32, &Removed);

    if (Count < 0) {
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint("No working xHCI controller. See: dmesg XHCI\n");
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    for (int i = 0; i < Count; i++) {
        int Index = New[i];
        int Volumes = FsMountDevice(Index);
        char Size[24];
        char Text[80];
        int Pos = 0;

        FormatSize(Size, BlockSize512(Index));

        ConsoleSetAttr(CONSOLE_ATTR_ACCENT);
        PrintPadded(BlockDeviceName(Index), 8);
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        PrintPadded(Size, 11);

        Pos = AppendText(Text, Pos, "sector ");
        Pos = AppendUInt(Text, Pos, BlockSectorSize(Index));
        Pos = AppendText(Text, Pos, ", ");
        Pos = AppendUInt(Text, Pos, (uint64_t)Volumes);
        Pos = AppendText(Text, Pos, " volume(s) mounted\n");
        Text[Pos] = 0;
        ConsolePrint(Text);
    }

    if (Removed > 0) {
        char Text[48];
        int Pos = AppendUInt(Text, 0, (uint64_t)Removed);
        Pos = AppendText(Text, Pos, " device(s) removed\n");
        Text[Pos] = 0;
        ConsolePrint(Text);
    }

    if (Count == 0 && Removed == 0) {
        ConsolePrint("No new USB storage devices. See: dmesg USB\n");
    }
}

static const COMMAND Commands[] = {
    {"help",   "list commands",             CmdHelp},
    {"clear",  "clear the screen",          CmdClear},
    {"echo",   "print arguments",           CmdEcho},
    {"mem",    "show memory usage",         CmdMem},
    {"lspci",  "list PCI devices",          CmdLspci},
    {"disks",  "list block devices",        CmdDisks},
    {"lsblk",  "list disks and partitions", CmdLsblk},
    {"usb",    "scan USB storage [list]",   CmdUsb},
    {"ls",     "list directory",            CmdLs},
    {"cd",     "change directory",          CmdCd},
    {"pwd",    "print current directory",   CmdPwd},
    {"cat",    "print a text file",         CmdCat},
    {"hexdump","hex view of a file",        CmdHexdump},
    {"hda",    "show audio controller state", CmdHda},
    {"dmesg",  "show boot log [prefix]",    CmdDmesg},
    {"beep",   "play test tone via HDA",    CmdBeep},
    {"play",   "play a WAV file",           CmdPlay},
    {"explorer","visual file explorer",     CmdExplorer},
    {"reboot", "restart the machine",       CmdReboot},
    {"shutdown", "power off the machine",   CmdShutdown},
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
        if (*p == '"') {
            p++;
            Argv[Argc++] = p;
            while (*p && *p != '"') {
                p++;
            }
            if (*p) {
                *p++ = 0;
            }
            continue;
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
    ConsolePrint("bloom");
    if (FsHasCwd()) {
        ConsolePutChar(' ');
        ConsolePrint(FsCwdVolume());
        ConsolePutChar(':');
        ConsolePrint(FsCwdPath());
    }
    ConsolePrint("> ");
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
