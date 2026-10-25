#include <stdint.h>
#include "boot_info.h"
#include "logo.h"
#include "gfx.h"
#include "console.h"
#include "shell.h"
#include "idt.h"
#include "keyboard.h"
#include "mouse.h"
#include "pmm.h"
#include "pci.h"
#include "ahci.h"
#include "fs.h"
#include "part.h"
#include "blockdev.h"
#include "nvme.h"
#include "hda.h"
#include "klog.h"
#include "power.h"

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

static void
LogHdaStatus(const HDA_STATUS *S)
{
    KLog("HDA: codec=");
    KLogUInt((uint64_t)S->CodecFound);
    KLog(" corb=");
    KLogUInt(S->CorbEntriesUsed);
    KLog(" rirb=");
    KLogUInt(S->RirbEntriesUsed);
    KLog("\n");
    KLog("HDA: fgcount=");
    KLogUInt(S->RootFgCount);
    KLog(" fgstart=");
    KLogUInt(S->FgStartNid);
    KLog(" afg=");
    KLogUInt((uint64_t)S->AfgFound);
    KLog(" fgtype=");
    KLogHex(S->LastFgTypeRaw, 1);
    KLog("\n");
    KLog("HDA: wp1=");
    KLogUInt(S->WpAfterFirst);
    KLog(" wp2=");
    KLogUInt(S->WpBeforeSecond);
    KLog(" corbwp=");
    KLogUInt(S->CorbWpReadback);
    KLog(" corbrp=");
    KLogUInt(S->CorbRpAfterFirst);
    KLog(" corbsts=");
    KLogUInt(S->CorbSts);
    KLog(" repeat=");
    KLogHex(S->RepeatSameCallRaw, 1);
    KLog("\n");
    KLog("HDA: widgets=");
    KLogUInt(S->WidgetCount);
    KLog(" dac=");
    KLogUInt((uint64_t)S->DacFound);
    KLog(" pin=");
    KLogUInt((uint64_t)S->PinFound);
    KLog(" path=");
    KLogUInt((uint64_t)S->PathLinked);
    KLog(" stream=");
    KLogUInt((uint64_t)S->StreamStarted);
    KLog("\n");
}

void
kmain(BOOT_INFO *Info)
{
    GfxInit(Info);

    FillScreen(0x00FF0000);

    InitIdt();
    FillScreen(0x00FF8000);
    InitKeyboard();
    FillScreen(0x00FFFF00);
    InitMouse();
    FillScreen(0x0000FF00);
    EnableInterrupts();
    InitPmm(Info);

    KLog("Bloom-OS boot log\n");
    KLog("PMM: pages free ");
    KLogUInt(GetFreePageCount());
    KLog(" / total ");
    KLogUInt(GetTotalPageCount());
    KLog("\n");

    PowerInit(Info->AcpiRsdp);

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

        KLogPci("AHCI", Ahci.Bus, Ahci.Device, Ahci.Function, Ahci.VendorId, Ahci.DeviceId);
        KLog("AHCI: abar ");
        KLogHex(Ahci.Abar, 8);
        KLog(", pci command was ");
        KLogHex(PciEnableDevice(Ahci.Bus, Ahci.Device, Ahci.Function), 4);
        KLog("\n");

        if (AhciInit(Ahci.Abar)) {
            BlockRegister("sata0", AhciReadSectors);
            KLog("AHCI: init ok\n");
        } else {
            DrawString(20, 50, "AHCI port init failed", TextColor);
            KLog("AHCI: init failed\n");
        }
    } else {
        DrawString(20, 30, "AHCI not found", TextColor);
        KLog("AHCI: not found\n");
    }

    {
        NVME_LOCATION Nvme = FindNvmeController();

        if (Nvme.Found) {
            DrawString(20, 110, "NVMe found", TextColor);

            KLogPci("NVMe", Nvme.Bus, Nvme.Device, Nvme.Function, Nvme.VendorId, Nvme.DeviceId);
            KLog("NVMe: bar0 ");
            KLogHex(Nvme.Bar0, 8);
            KLog(", pci command was ");
            KLogHex(PciEnableDevice(Nvme.Bus, Nvme.Device, Nvme.Function), 4);
            KLog("\n");

            if (NvmeInit(Nvme.Bar0)) {
                BlockRegister("nvme0", NvmeReadSectors);
                DrawString(20, 130, "NVMe init OK", TextColor);
                KLog("NVMe: init ok\n");
            } else {
                DrawString(20, 130, "NVMe init failed", TextColor);
                KLog("NVMe: init failed, stage ");
                KLogUInt((uint64_t)NvmeDbgStage);
                KLog(", status ");
                KLogHex(NvmeDbgStatus, 1);
                KLog(", cap ");
                KLogHex(NvmeDbgCap, 1);
                KLog(", csts ");
                KLogHex(NvmeDbgCsts, 1);
                KLog("\n");
            }
        } else {
            DrawString(20, 110, "NVMe not found", TextColor);
            KLog("NVMe: not found\n");
        }
    }

    if (BlockDeviceCount() == 0) {
        KLog("Disk: no block device\n");
    }

    int Mounted = FsInit();

    DrawString(20, 50, "Block devices: ", TextColor);
    DrawUInt64(20 + 15 * FONT_WIDTH, 50, (uint64_t)BlockDeviceCount(), TextColor);
    DrawString(20, 70, "FAT32 volumes mounted: ", TextColor);
    DrawUInt64(20 + 23 * FONT_WIDTH, 70, (uint64_t)Mounted, TextColor);

    HDA_LOCATION Hda = FindHdaController();

    if (Hda.Found) {
        DrawString(20, 90, "HDA found, BAR0: ", TextColor);
        DrawUInt64(20 + 17 * FONT_WIDTH, 90, Hda.Bar0, TextColor);

        KLogPci("HDA", Hda.Bus, Hda.Device, Hda.Function, Hda.VendorId, Hda.DeviceId);
        KLog("HDA: bar0 ");
        KLogHex(Hda.Bar0, 8);
        KLog(", pci command was ");
        KLogHex(PciEnableDevice(Hda.Bus, Hda.Device, Hda.Function), 4);
        KLog("\n");

        if (Hda.VendorId == 0x8086) {
            uint32_t Tcsel = PciConfigRead32(Hda.Bus, Hda.Device, Hda.Function, 0x44);
            uint32_t Devc = PciConfigRead32(Hda.Bus, Hda.Device, Hda.Function, 0x78);

            KLog("HDA: pci 0x44 was ");
            KLogHex(Tcsel, 8);
            KLog(", 0x78 was ");
            KLogHex(Devc, 8);
            KLog("\n");

            PciConfigWrite32(Hda.Bus, Hda.Device, Hda.Function, 0x44, Tcsel & ~0x07u);
            PciConfigWrite32(Hda.Bus, Hda.Device, Hda.Function, 0x78, Devc & 0x0000F7FFu);
        }

        HDA_STATUS HdaStatus = HdaInit(Hda.Bar0);
        LogHdaStatus(&HdaStatus);
        GlobalHdaStatus = HdaStatus;
        GlobalHdaFound = 1;

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
        DrawString(20 + 8 * FONT_WIDTH, 190, "wp2: ", TextColor);
        DrawUInt64(20 + 13 * FONT_WIDTH, 190, HdaStatus.WpBeforeSecond, TextColor);
        DrawString(20 + 16 * FONT_WIDTH, 190, "corbrp: ", TextColor);
        DrawUInt64(20 + 24 * FONT_WIDTH, 190, HdaStatus.CorbRpAfterFirst, TextColor);
        DrawString(20 + 27 * FONT_WIDTH, 190, "corbsts: ", TextColor);
        DrawUInt64(20 + 36 * FONT_WIDTH, 190, HdaStatus.CorbSts, TextColor);
        DrawString(20 + 38 * FONT_WIDTH, 190, "repeat: ", TextColor);
        DrawUInt64(20 + 46 * FONT_WIDTH, 190, HdaStatus.RepeatSameCallRaw, TextColor);

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
        KLog("HDA: not found\n");
    }

    PlayStartupChime();

    for (int i = 0; i < Len; i++) {
        DrawChar(TextX + i * FONT_WIDTH, TextY, Message[i], TextColor);
        Sleep(40);
    }

    Sleep(3000);
    ConsoleInit();
    ShellRun();
}
