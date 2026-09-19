#include <stdint.h>
#include "boot_info.h"
#include "font8x16.h"
#include "logo.h"
#include "idt.h"
#include "keyboard.h"
#include "mouse.h"
#include "pmm.h"
#include "pci.h"
#include "ahci.h"
#include "fat32.h"
#include "blockdev.h"
#include "nvme.h"
#include "hda.h"
#include "hda.h"

static volatile uint32_t *FrameBuffer;
static uint64_t ScreenWidth;
static uint64_t ScreenHeight;
static uint64_t Stride;

static void
PutPixel(uint32_t x, uint32_t y, uint32_t color)
{
    FrameBuffer[y * Stride + x] = color;
}

static uint32_t
GetPixel(uint32_t x, uint32_t y)
{
    return FrameBuffer[y * Stride + x];
}

static void
FillScreen(uint32_t color)
{
    for (uint64_t y = 0; y < ScreenHeight; y++) {
        for (uint64_t x = 0; x < ScreenWidth; x++) {
            PutPixel(x, y, color);
        }
    }
}

static void
DrawChar(uint32_t x, uint32_t y, char ch, uint32_t color)
{
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR) {
        return;
    }

    const uint8_t *Glyph = Font8x16[ch - FONT_FIRST_CHAR];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t Bits = Glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (Bits & (1 << (7 - col))) {
                PutPixel(x + col, y + row, color);
            }
        }
    }
}

static void
DrawLogo(uint32_t OffsetX, uint32_t OffsetY)
{
    for (uint32_t y = 0; y < LOGO_HEIGHT; y++) {
        for (uint32_t x = 0; x < LOGO_WIDTH; x++) {
            uint32_t Pixel = BloomLogo[y * LOGO_WIDTH + x];
            uint8_t Alpha = (Pixel >> 24) & 0xFF;
            if (Alpha == 0) {
                continue;
            }
            PutPixel(OffsetX + x, OffsetY + y, Pixel & 0x00FFFFFF);
        }
    }
}

static void
FillRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            PutPixel(x + col, y + row, color);
        }
    }
}

#define CURSOR_WIDTH 8
#define CURSOR_HEIGHT 14

static const uint8_t CursorBitmap[CURSOR_HEIGHT] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF,
    0xF8, 0xD8, 0x8C, 0x0C, 0x06, 0x06
};

static uint32_t CursorSaved[CURSOR_WIDTH * CURSOR_HEIGHT];

static void
DrawCursor(uint32_t x, uint32_t y, uint32_t color)
{
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        uint8_t Bits = CursorBitmap[row];
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            if (Bits & (1 << (7 - col))) {
                PutPixel(x + col, y + row, color);
            }
        }
    }
}

static void
SaveUnderCursor(uint32_t x, uint32_t y)
{
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            CursorSaved[row * CURSOR_WIDTH + col] = GetPixel(x + col, y + row);
        }
    }
}

static void
RestoreUnderCursor(uint32_t x, uint32_t y)
{
    for (int row = 0; row < CURSOR_HEIGHT; row++) {
        for (int col = 0; col < CURSOR_WIDTH; col++) {
            PutPixel(x + col, y + row, CursorSaved[row * CURSOR_WIDTH + col]);
        }
    }
}

static void
DrawString(uint32_t x, uint32_t y, const char *s, uint32_t color)
{
    uint32_t CurX = x;
    for (int i = 0; s[i]; i++) {
        DrawChar(CurX, y, s[i], color);
        CurX += FONT_WIDTH;
    }
}

static void
DrawUInt64(uint32_t x, uint32_t y, uint64_t value, uint32_t color)
{
    char Buffer[21];
    int Pos = 20;
    Buffer[Pos] = 0;

    if (value == 0) {
        Buffer[--Pos] = '0';
    } else {
        while (value > 0) {
            Buffer[--Pos] = (char)('0' + (value % 10));
            value /= 10;
        }
    }

    DrawString(x, y, &Buffer[Pos], color);
}

static int
StrLen(const char *s)
{
    int len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

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

static void
PlayPhrase(const uint32_t *Freqs, const uint64_t *Durations, int Count)
{
    uint32_t Divisor = 1193180 / Freqs[0];

    OutB(0x43, 0xB6);
    OutB(0x42, (uint8_t)(Divisor & 0xFF));
    OutB(0x42, (uint8_t)((Divisor >> 8) & 0xFF));

    uint8_t Tmp = InB(0x61);
    OutB(0x61, Tmp | 0x03);

    for (int i = 0; i < Count; i++) {
        int Steps = 6;
        uint32_t StartFreq = Freqs[i];
        uint32_t EndFreq = (i + 1 < Count) ? Freqs[i + 1] : Freqs[i];
        uint64_t StepDuration = Durations[i] / Steps;

        for (int s = 0; s < Steps; s++) {
            uint32_t CurrentFreq = StartFreq + (EndFreq - StartFreq) * s / Steps;
            Divisor = 1193180 / CurrentFreq;
            OutB(0x42, (uint8_t)(Divisor & 0xFF));
            OutB(0x42, (uint8_t)((Divisor >> 8) & 0xFF));
            Sleep(StepDuration);
        }
    }

    Tmp = InB(0x61);
    OutB(0x61, Tmp & 0xFC);
}

static const uint32_t Group1Freqs[] = {220, 262, 330};
static const uint64_t Group1Durations[] = {350, 350, 450};

static const uint32_t Group2Freqs[] = {294, 349, 440};
static const uint64_t Group2Durations[] = {300, 300, 450};

static const uint32_t Group3Freqs[] = {392, 330};
static const uint64_t Group3Durations[] = {300, 300};

static const uint32_t Group4Freqs[] = {523};
static const uint64_t Group4Durations[] = {500};

static const uint32_t Group5Freqs[] = {440, 659, 880};
static const uint64_t Group5Durations[] = {250, 250, 900};

static void
PlayStartupChime(void)
{
    PlayPhrase(Group1Freqs, Group1Durations, 3);
    Sleep(150);

    PlayPhrase(Group2Freqs, Group2Durations, 3);
    Sleep(150);

    PlayPhrase(Group3Freqs, Group3Durations, 2);
    Sleep(200);

    PlayPhrase(Group4Freqs, Group4Durations, 1);
    Sleep(250);

    PlayPhrase(Group5Freqs, Group5Durations, 3);
}

void
kmain(BOOT_INFO *Info)
{
    FrameBuffer = (volatile uint32_t *)Info->FrameBufferBase;
    ScreenWidth = Info->Width;
    ScreenHeight = Info->Height;
    Stride = Info->PixelsPerScanLine;

    FillScreen(0x00FF0000);

    InitIdt();
    FillScreen(0x00FF8000);
    InitKeyboard();
    FillScreen(0x00FFFF00);
    InitMouse();
    FillScreen(0x0000FF00);
    EnableInterrupts();
    InitPmm(Info);

    FillScreen(0x000000FF);

    uint32_t BackgroundColor = 0x00201040;
    uint32_t TextColor = 0x00E0D0FF;

    FillScreen(BackgroundColor);

    const char *Message = "Welcome to Bloom-OS";
    int Len = StrLen(Message);
    uint32_t TextWidth = Len * FONT_WIDTH;
    uint32_t TextX = (ScreenWidth - TextWidth) / 2;
    uint32_t TextY = (ScreenHeight - FONT_HEIGHT) / 2;

    uint32_t Gap = 30;
    uint32_t LogoX = (ScreenWidth - LOGO_WIDTH) / 2;
    uint32_t LogoY = TextY - Gap - LOGO_HEIGHT;

    DrawLogo(LogoX, LogoY);

    DrawString(20, 10, "Free pages: ", TextColor);
    DrawUInt64(20 + 12 * FONT_WIDTH, 10, GetFreePageCount(), TextColor);
    DrawString(20 + 20 * FONT_WIDTH, 10, "/", TextColor);
    DrawUInt64(20 + 21 * FONT_WIDTH, 10, GetTotalPageCount(), TextColor);

    AHCI_LOCATION Ahci = FindAhciController();

    if (Ahci.Found) {
        DrawString(20, 30, "AHCI found, ABAR: ", TextColor);
        DrawUInt64(20 + 18 * FONT_WIDTH, 30, Ahci.Abar, TextColor);

        if (AhciInit(Ahci.Abar)) {
            RegisterBlockDevice(AhciReadSectors, "AHCI");
        } else {
            DrawString(20, 50, "AHCI port init failed", TextColor);
        }
    } else {
        DrawString(20, 30, "AHCI not found", TextColor);
    }

    if (!BlockDeviceReady()) {
        NVME_LOCATION Nvme = FindNvmeController();

        if (Nvme.Found) {
            DrawString(20, 110, "NVMe found", TextColor);

            if (NvmeInit(Nvme.Bar0)) {
                RegisterBlockDevice(NvmeReadSectors, "NVMe");
                DrawString(20, 130, "NVMe init OK", TextColor);
            } else {
                DrawString(20, 130, "NVMe init failed", TextColor);
            }
        } else {
            DrawString(20, 110, "NVMe not found", TextColor);
        }
    }

    if (BlockDeviceReady()) {
        uint8_t *SectorBuffer = (uint8_t *)AllocPage();

        if (BlockReadSectors(0, 1, SectorBuffer)) {
            if (SectorBuffer[510] == 0x55 && SectorBuffer[511] == 0xAA) {
                DrawString(20, 50, "Disk read OK, boot sig valid", TextColor);

                if (FatInit()) {
                    FAT_FILE File = FatFindFile("KERNEL  BIN");

                    if (File.Found) {
                        DrawString(20, 70, "FAT32: KERNEL.BIN size ", TextColor);
                        DrawUInt64(20 + 23 * FONT_WIDTH, 70, File.Size, TextColor);
                    } else {
                        DrawString(20, 70, "FAT32: KERNEL.BIN not found", TextColor);
                    }
                } else {
                    DrawString(20, 70, "FAT32 init failed", TextColor);
                }
            } else {
                DrawString(20, 50, "Disk read OK, bad boot sig", TextColor);
            }
        } else {
            DrawString(20, 50, "Disk read failed", TextColor);
        }
    }

    HDA_LOCATION Hda = FindHdaController();

    if (Hda.Found) {
        DrawString(20, 90, "HDA found, BAR0: ", TextColor);
        DrawUInt64(20 + 17 * FONT_WIDTH, 90, Hda.Bar0, TextColor);

        HDA_STATUS HdaStatus = HdaInit(Hda.Bar0);

        DrawString(20, 150, "HDA codec: ", TextColor);
        DrawUInt64(20 + 11 * FONT_WIDTH, 150, HdaStatus.CodecFound, TextColor);
        DrawString(20 + 13 * FONT_WIDTH, 150, "corb/rirb: ", TextColor);
        DrawUInt64(20 + 24 * FONT_WIDTH, 150, HdaStatus.CorbEntriesUsed, TextColor);
        DrawString(20 + 28 * FONT_WIDTH, 150, "/", TextColor);
        DrawUInt64(20 + 29 * FONT_WIDTH, 150, HdaStatus.RirbEntriesUsed, TextColor);

        DrawString(20, 170, "fgcount: ", TextColor);
        DrawUInt64(20 + 9 * FONT_WIDTH, 170, HdaStatus.RootFgCount, TextColor);
        DrawString(20 + 11 * FONT_WIDTH, 170, "fgstart: ", TextColor);
        DrawUInt64(20 + 20 * FONT_WIDTH, 170, HdaStatus.FgStartNid, TextColor);
        DrawString(20 + 22 * FONT_WIDTH, 170, "afg: ", TextColor);
        DrawUInt64(20 + 27 * FONT_WIDTH, 170, HdaStatus.AfgFound, TextColor);
        DrawString(20 + 29 * FONT_WIDTH, 170, "fgtype: ", TextColor);
        DrawUInt64(20 + 37 * FONT_WIDTH, 170, HdaStatus.LastFgTypeRaw, TextColor);

        DrawString(20, 190, "wp1: ", TextColor);
        DrawUInt64(20 + 5 * FONT_WIDTH, 190, HdaStatus.WpAfterFirst, TextColor);
        DrawString(20 + 8 * FONT_WIDTH, 190, "wp2before: ", TextColor);
        DrawUInt64(20 + 19 * FONT_WIDTH, 190, HdaStatus.WpBeforeSecond, TextColor);
        DrawString(20 + 22 * FONT_WIDTH, 190, "corbwp2: ", TextColor);
        DrawUInt64(20 + 31 * FONT_WIDTH, 190, HdaStatus.CorbWpReadback, TextColor);
        DrawString(20 + 34 * FONT_WIDTH, 190, "repeat: ", TextColor);
        DrawUInt64(20 + 42 * FONT_WIDTH, 190, HdaStatus.RepeatSameCallRaw, TextColor);

        DrawString(20, 210, "HDA widgets: ", TextColor);
        DrawUInt64(20 + 13 * FONT_WIDTH, 210, HdaStatus.WidgetCount, TextColor);
        DrawString(20 + 16 * FONT_WIDTH, 210, "dac: ", TextColor);
        DrawUInt64(20 + 21 * FONT_WIDTH, 210, HdaStatus.DacFound, TextColor);
        DrawString(20 + 23 * FONT_WIDTH, 210, "pin: ", TextColor);
        DrawUInt64(20 + 28 * FONT_WIDTH, 210, HdaStatus.PinFound, TextColor);

        DrawString(20, 230, "HDA path: ", TextColor);
        DrawUInt64(20 + 10 * FONT_WIDTH, 230, HdaStatus.PathLinked, TextColor);
        DrawString(20 + 12 * FONT_WIDTH, 230, "stream: ", TextColor);
        DrawUInt64(20 + 20 * FONT_WIDTH, 230, HdaStatus.StreamStarted, TextColor);

        if (HdaStatus.StreamStarted) {
            HdaPlayTestTone();
        }
    } else {
        DrawString(20, 90, "HDA not found", TextColor);
    }

    PlayStartupChime();

    for (int i = 0; i < Len; i++) {
        DrawChar(TextX + i * FONT_WIDTH, TextY, Message[i], TextColor);
        Sleep(40);
    }

    uint32_t Margin = 20;
    uint32_t LineSpacing = 4;
    uint32_t CursorX = Margin;
    uint32_t CursorY = TextY + FONT_HEIGHT + 40;

    #define MAX_LINES 256
    static uint32_t LineEndX[MAX_LINES];
    int CurrentLine = 0;

    uint32_t MouseCursorX = ScreenWidth / 2;
    uint32_t MouseCursorY = ScreenHeight / 2;
    uint32_t CursorColor = 0x00FFFFFF;

    SaveUnderCursor(MouseCursorX, MouseCursorY);
    DrawCursor(MouseCursorX, MouseCursorY, CursorColor);

    for (;;) {
        int GotEvent = 0;
        char c = KeyboardGetChar();

        if (c != 0) {
            GotEvent = 1;

            if (c == '\b') {
                if (CursorX > Margin) {
                    CursorX -= FONT_WIDTH;
                    FillRect(CursorX, CursorY, FONT_WIDTH, FONT_HEIGHT, BackgroundColor);
                } else if (CurrentLine > 0) {
                    CurrentLine--;
                    CursorY -= FONT_HEIGHT + LineSpacing;
                    CursorX = LineEndX[CurrentLine];
                }
            } else if (c == '\n') {
                if (CurrentLine < MAX_LINES - 1) {
                    LineEndX[CurrentLine] = CursorX;
                    CurrentLine++;
                }
                CursorX = Margin;
                CursorY += FONT_HEIGHT + LineSpacing;
            } else {
                DrawChar(CursorX, CursorY, c, TextColor);
                CursorX += FONT_WIDTH;

                if (CursorX + FONT_WIDTH > ScreenWidth - Margin) {
                    if (CurrentLine < MAX_LINES - 1) {
                        LineEndX[CurrentLine] = CursorX;
                        CurrentLine++;
                    }
                    CursorX = Margin;
                    CursorY += FONT_HEIGHT + LineSpacing;
                }
            }
        }

        int Dx, Dy;
        uint8_t Buttons;

        if (MouseGetDelta(&Dx, &Dy, &Buttons)) {
            GotEvent = 1;

            RestoreUnderCursor(MouseCursorX, MouseCursorY);

            int NewX = (int)MouseCursorX + Dx;
            int NewY = (int)MouseCursorY - Dy;

            if (NewX < 0) {
                NewX = 0;
            }
            if (NewX > (int)ScreenWidth - CURSOR_WIDTH) {
                NewX = ScreenWidth - CURSOR_WIDTH;
            }
            if (NewY < 0) {
                NewY = 0;
            }
            if (NewY > (int)ScreenHeight - CURSOR_HEIGHT) {
                NewY = ScreenHeight - CURSOR_HEIGHT;
            }

            MouseCursorX = NewX;
            MouseCursorY = NewY;

            SaveUnderCursor(MouseCursorX, MouseCursorY);
            DrawCursor(MouseCursorX, MouseCursorY, CursorColor);
        }

        if (!GotEvent) {
            __asm__ __volatile__("hlt");
        }
    }
}
