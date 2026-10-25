#include "hda.h"
#include "pmm.h"
#include "idt.h"
#include "klog.h"

#define REG_GCAP        0x00
#define REG_GCTL        0x08
#define REG_STATESTS    0x0E
#define REG_INTCTL      0x18
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48
#define REG_CORBRP      0x4A
#define REG_CORBCTL     0x4C
#define REG_CORBSTS     0x4D
#define REG_CORBSIZE    0x4E
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58
#define REG_RINTCNT     0x5A
#define REG_RIRBCTL     0x5C
#define REG_RIRBSTS     0x5D
#define REG_RIRBSIZE    0x5E
#define REG_SD_BASE     0x80
#define SD_SIZE         0x20

#define SDCTL_OFF   0x00
#define SDSTS_OFF   0x03
#define SDLPIB_OFF  0x04
#define SDCBL_OFF   0x08
#define SDFIFOS_OFF 0x10
#define SDLVI_OFF   0x0C
#define SDFMT_OFF   0x12
#define SDBDPL_OFF  0x18
#define SDBDPU_OFF  0x1C

#define VERB_GET_PARAMETER          0xF00
#define PARAM_VENDOR_ID             0x00
#define PARAM_NODE_COUNT            0x04
#define PARAM_FUNCTION_GROUP_TYPE   0x05
#define PARAM_AUDIO_WIDGET_CAPS     0x09
#define PARAM_PIN_CAPS              0x0C
#define PARAM_IN_AMP_CAPS           0x0D
#define PARAM_GPIO_COUNT            0x11
#define PARAM_CONN_LIST_LENGTH      0x0E
#define PARAM_OUT_AMP_CAPS          0x12

#define VERB_GET_CONFIG_DEFAULT     0xF1C
#define VERB_GET_STREAM_FORMAT      0xA
#define VERB_GET_AMP_GAIN_MUTE      0xB
#define VERB_GET_POWER_STATE        0xF05
#define VERB_GET_CHANNEL_STREAM_ID  0xF06
#define VERB_GET_PIN_WIDGET_CTL     0xF07

#define VERB_GET_CONN_LIST_ENTRY    0xF02
#define VERB_SET_CONN_SELECT        0x701
#define VERB_SET_AMP_GAIN_MUTE      0x3
#define VERB_SET_STREAM_FORMAT      0x2
#define VERB_SET_CHANNEL_STREAM_ID  0x706
#define VERB_SET_PIN_WIDGET_CTL     0x707
#define VERB_SET_POWER_STATE        0x705
#define VERB_SET_EAPD_BTL_ENABLE    0x70C

#define WIDGET_TYPE_AUDIO_OUTPUT    0x0
#define WIDGET_TYPE_PIN_COMPLEX     0x4

#define STREAM_TAG  1
#define BUFFER_FRAMES 1024

static volatile uint8_t *HdaBase;

static uint8_t  Read8(uint32_t Offset)  { return HdaBase[Offset]; }
static void     Write8(uint32_t Offset, uint8_t Value)  { HdaBase[Offset] = Value; }
static uint16_t Read16(uint32_t Offset) { return *(volatile uint16_t *)(HdaBase + Offset); }
static void     Write16(uint32_t Offset, uint16_t Value) { *(volatile uint16_t *)(HdaBase + Offset) = Value; }
static uint32_t Read32(uint32_t Offset) { return *(volatile uint32_t *)(HdaBase + Offset); }
static void     Write32(uint32_t Offset, uint32_t Value) { *(volatile uint32_t *)(HdaBase + Offset) = Value; }

static volatile uint32_t *Corb;
static volatile uint64_t *Rirb;
static uint16_t CorbEntries;
static uint16_t RirbEntries;
static uint16_t LastRirbWp;
static uint16_t CorbWritePos;
static uint16_t RirbReadPos;
static uint8_t  CodecAddr;
static uint32_t OutStreamBase;
static int StreamOk;

static void
Wait(uint32_t Loops)
{
    for (volatile uint32_t i = 0; i < Loops; i++) {
        __asm__ __volatile__("nop");
    }
}

static int
ResetController(void)
{
    Write32(REG_GCTL, Read32(REG_GCTL) & ~1u);

    for (int i = 0; i < 100000; i++) {
        if ((Read32(REG_GCTL) & 1u) == 0) {
            break;
        }
        Wait(100);
    }

    Write32(REG_GCTL, Read32(REG_GCTL) | 1u);

    for (int i = 0; i < 100000; i++) {
        if (Read32(REG_GCTL) & 1u) {
            return 1;
        }
        Wait(100);
    }

    return 0;
}

static int
SetupCorbRirb(void)
{
    Write8(REG_CORBCTL, 0);
    Write8(REG_RIRBCTL, 0);

    void *CorbPage = AllocPageBelow4G();
    void *RirbPage = AllocPageBelow4G();

    if (!CorbPage || !RirbPage) {
        return 0;
    }

    Corb = (volatile uint32_t *)CorbPage;
    Rirb = (volatile uint64_t *)RirbPage;

    for (int i = 0; i < 1024; i++) {
        Corb[i] = 0;
    }

    for (int i = 0; i < 512; i++) {
        Rirb[i] = 0;
    }

    __asm__ __volatile__("mfence" : : : "memory");

    uint8_t CorbSizeCap = (Read8(REG_CORBSIZE) >> 4) & 0x7;
    uint8_t CorbSizeBits;
    if (CorbSizeCap & 0x4) { CorbEntries = 256; CorbSizeBits = 2; }
    else if (CorbSizeCap & 0x2) { CorbEntries = 16; CorbSizeBits = 1; }
    else { CorbEntries = 2; CorbSizeBits = 0; }

    uint8_t RirbSizeCap = (Read8(REG_RIRBSIZE) >> 4) & 0x7;
    uint8_t RirbSizeBits;
    if (RirbSizeCap & 0x4) { RirbEntries = 256; RirbSizeBits = 2; }
    else if (RirbSizeCap & 0x2) { RirbEntries = 16; RirbSizeBits = 1; }
    else { RirbEntries = 2; RirbSizeBits = 0; }

    Write32(REG_CORBLBASE, (uint32_t)(uintptr_t)CorbPage);
    Write32(REG_CORBUBASE, 0);
    Write8(REG_CORBSIZE, CorbSizeBits);
    Write16(REG_CORBRP, 0x8000);
    for (int i = 0; i < 100000; i++) {
        if (Read16(REG_CORBRP) & 0x8000) {
            break;
        }
        Wait(100);
    }
    Write16(REG_CORBRP, 0);
    for (int i = 0; i < 100000; i++) {
        if ((Read16(REG_CORBRP) & 0x8000) == 0) {
            break;
        }
        Wait(100);
    }
    Write16(REG_CORBWP, 0);
    CorbWritePos = 0;

    Write32(REG_RIRBLBASE, (uint32_t)(uintptr_t)RirbPage);
    Write32(REG_RIRBUBASE, 0);
    Write8(REG_RIRBSIZE, RirbSizeBits);
    Write16(REG_RIRBWP, 0x8000);
    RirbReadPos = 0;
    Write16(REG_RINTCNT, 0xC0);
    Write8(REG_RIRBSTS, Read8(REG_RIRBSTS));

    Write8(REG_CORBCTL, 0x02);
    Write8(REG_RIRBCTL, 0x03);

    return 1;
}

static uint32_t
SendVerb(uint8_t Nid, uint32_t Verb, uint32_t Payload)
{
    uint32_t Command = ((uint32_t)CodecAddr << 28) | ((uint32_t)Nid << 20) | (Verb << 8) | (Payload & 0xFF);

    if (Verb <= 0xF) {
        Command = ((uint32_t)CodecAddr << 28) | ((uint32_t)Nid << 20) | (Verb << 16) | (Payload & 0xFFFF);
    }

    uint16_t NextPos = (uint16_t)((CorbWritePos + 1) % CorbEntries);
    Corb[NextPos] = Command;
    CorbWritePos = NextPos;
    Write16(REG_CORBWP, CorbWritePos);

    for (int i = 0; i < 1000000; i++) {
        uint16_t Wp = Read16(REG_RIRBWP) & 0xFF;

        while (RirbReadPos != Wp) {
            RirbReadPos = (uint16_t)((RirbReadPos + 1) % RirbEntries);
            uint64_t Entry = Rirb[RirbReadPos];
            LastRirbWp = RirbReadPos;
            Write8(REG_RIRBSTS, Read8(REG_RIRBSTS));

            if (Entry & (1ULL << 36)) {
                continue;
            }

            return (uint32_t)(Entry & 0xFFFFFFFF);
        }

        Wait(50);
    }

    return 0;
}

static uint32_t
GetParam(uint8_t Nid, uint32_t Param)
{
    return SendVerb(Nid, VERB_GET_PARAMETER, Param);
}

#define MAX_DACS 8
#define MAX_PINS 16

static uint32_t AfgInAmpCaps;
static uint32_t AfgOutAmpCaps;
static uint8_t Dacs[MAX_DACS];
static int DacCount;
static uint8_t Pins[MAX_PINS];
static uint32_t PinCfgs[MAX_PINS];
static uint32_t PinCapsList[MAX_PINS];
static int PinCount;
static uint8_t UsedDacs[MAX_DACS];
static int UsedDacCount;
static uint8_t EnabledPins[MAX_PINS];
static int EnabledPinCount;

static void
FlushRange(void *Addr, uint32_t Bytes)
{
    uint8_t *Ptr = (uint8_t *)Addr;

    for (uint32_t i = 0; i < Bytes; i += 64) {
        __asm__ __volatile__("clflush (%0)" : : "r"(Ptr + i) : "memory");
    }

    __asm__ __volatile__("mfence" : : : "memory");
}

static void
UnmuteWidget(uint8_t Nid, int InputIndex)
{
    uint32_t Caps = GetParam(Nid, PARAM_AUDIO_WIDGET_CAPS);

    SendVerb(Nid, VERB_SET_POWER_STATE, 0);

    if (Caps & (1u << 2)) {
        uint32_t OutCaps = (Caps & (1u << 3)) ? GetParam(Nid, PARAM_OUT_AMP_CAPS) : AfgOutAmpCaps;
        SendVerb(Nid, VERB_SET_AMP_GAIN_MUTE, 0xB000 | (OutCaps & 0x7F));
    }

    if ((Caps & (1u << 1)) && InputIndex >= 0) {
        uint32_t InCaps = (Caps & (1u << 3)) ? GetParam(Nid, PARAM_IN_AMP_CAPS) : AfgInAmpCaps;
        SendVerb(Nid, VERB_SET_AMP_GAIN_MUTE, 0x7000 | (((uint32_t)InputIndex & 0xF) << 8) | (InCaps & 0x7F));
    }
}

static int
ConnectPathToDac(uint8_t Nid, uint8_t TargetDac, int Depth)
{
    if (Depth <= 0) {
        return 0;
    }

    uint32_t ConnParam = GetParam(Nid, PARAM_CONN_LIST_LENGTH);
    uint8_t Count = (uint8_t)(ConnParam & 0x7F);
    uint8_t LongForm = (uint8_t)((ConnParam >> 7) & 1);

    if (Count == 0 || LongForm) {
        return 0;
    }

    for (uint8_t i = 0; i < Count; i++) {
        uint32_t Resp = SendVerb(Nid, VERB_GET_CONN_LIST_ENTRY, (uint32_t)(i & ~3u));
        uint8_t Entry = (uint8_t)((Resp >> ((i & 3) * 8)) & 0x7F);

        int Match = (Entry == TargetDac);
        if (!Match) {
            Match = ConnectPathToDac(Entry, TargetDac, Depth - 1);
        }

        if (Match) {
            SendVerb(Nid, VERB_SET_CONN_SELECT, i);
            UnmuteWidget(Nid, i);
            return 1;
        }
    }

    return 0;
}

static void
LogConnList(uint8_t Nid)
{
    uint32_t ConnParam = GetParam(Nid, PARAM_CONN_LIST_LENGTH);
    uint8_t Count = (uint8_t)(ConnParam & 0x7F);

    if (Count == 0 || (ConnParam & 0x80)) {
        return;
    }

    KLog(" conn");

    for (uint8_t i = 0; i < Count && i < 8; i++) {
        uint32_t Resp = SendVerb(Nid, VERB_GET_CONN_LIST_ENTRY, (uint32_t)(i & ~3u));
        KLog(i == 0 ? " " : ",");
        KLogHex((Resp >> ((i & 3) * 8)) & 0xFF, 2);
    }
}

static uint8_t
WalkWidgets(uint8_t AfgNid)
{
    uint32_t NodeCount = GetParam(AfgNid, PARAM_NODE_COUNT);
    uint8_t StartNid = (uint8_t)((NodeCount >> 16) & 0xFF);
    uint8_t Count = (uint8_t)(NodeCount & 0xFF);

    DacCount = 0;
    PinCount = 0;

    for (uint8_t i = 0; i < Count; i++) {
        uint8_t Nid = (uint8_t)(StartNid + i);
        uint32_t Caps = GetParam(Nid, PARAM_AUDIO_WIDGET_CAPS);
        uint8_t Type = (uint8_t)((Caps >> 20) & 0xF);

        KLog("HDA w ");
        KLogHex(Nid, 2);
        KLog(" t");
        KLogUInt(Type);
        KLog(" c");
        KLogHex(Caps, 1);

        if (Type == WIDGET_TYPE_AUDIO_OUTPUT && !(Caps & (1u << 9)) && DacCount < MAX_DACS) {
            Dacs[DacCount++] = Nid;
        }

        if (Type == WIDGET_TYPE_PIN_COMPLEX) {
            uint32_t PinCaps = GetParam(Nid, PARAM_PIN_CAPS);
            uint32_t Cfg = SendVerb(Nid, VERB_GET_CONFIG_DEFAULT, 0);

            KLog(" cfg ");
            KLogHex(Cfg, 8);
            KLog(" pc ");
            KLogHex(PinCaps, 1);

            if (PinCount < MAX_PINS) {
                Pins[PinCount] = Nid;
                PinCfgs[PinCount] = Cfg;
                PinCapsList[PinCount] = PinCaps;
                PinCount++;
            }
        }

        if (Caps & (1u << 8)) {
            LogConnList(Nid);
        }

        KLog("\n");
    }

    return Count;
}

static int
PinIsOutputTarget(uint32_t PinCaps, uint32_t Cfg)
{
    if (!(PinCaps & (1u << 4))) {
        return 0;
    }

    if (((Cfg >> 30) & 3) == 1) {
        return 0;
    }

    return ((Cfg >> 20) & 0xF) <= 2;
}

static void
EnablePin(uint8_t Nid, uint32_t PinCaps)
{
    SendVerb(Nid, VERB_SET_POWER_STATE, 0);
    SendVerb(Nid, VERB_SET_PIN_WIDGET_CTL, 0xC0);

    if (PinCaps & (1u << 16)) {
        SendVerb(Nid, VERB_SET_EAPD_BTL_ENABLE, 0x02);
    }
}

static void
MarkDacUsed(uint8_t Dac)
{
    for (int i = 0; i < UsedDacCount; i++) {
        if (UsedDacs[i] == Dac) {
            return;
        }
    }

    if (UsedDacCount < MAX_DACS) {
        UsedDacs[UsedDacCount++] = Dac;
    }
}

static int
RouteOutputPins(int RequireTarget, int *CandidatePins)
{
    int Enabled = 0;

    for (int p = 0; p < PinCount; p++) {
        int Wanted = RequireTarget ? PinIsOutputTarget(PinCapsList[p], PinCfgs[p]) : ((PinCapsList[p] & (1u << 4)) != 0);

        if (!Wanted) {
            continue;
        }

        (*CandidatePins)++;

        for (int d = 0; d < DacCount; d++) {
            if (ConnectPathToDac(Pins[p], Dacs[d], 4)) {
                EnablePin(Pins[p], PinCapsList[p]);
                MarkDacUsed(Dacs[d]);

                if (EnabledPinCount < MAX_PINS) {
                    EnabledPins[EnabledPinCount++] = Pins[p];
                }

                KLog("HDA: pin ");
                KLogHex(Pins[p], 2);
                KLog(" -> dac ");
                KLogHex(Dacs[d], 2);
                KLog("\n");

                Enabled++;
                break;
            }
        }
    }

    return Enabled;
}

static void
ConfigureUsedDacs(void)
{
    for (int i = 0; i < UsedDacCount; i++) {
        uint8_t Dac = UsedDacs[i];

        SendVerb(Dac, VERB_SET_POWER_STATE, 0);
        SendVerb(Dac, VERB_SET_CHANNEL_STREAM_ID, (STREAM_TAG << 4) | 0);
        SendVerb(Dac, VERB_SET_STREAM_FORMAT, 0x0011);
        UnmuteWidget(Dac, -1);
    }
}

static void
LogVerify(void)
{
    for (int i = 0; i < UsedDacCount; i++) {
        uint8_t Dac = UsedDacs[i];

        KLog("HDA: chk dac ");
        KLogHex(Dac, 2);
        KLog(" fmt ");
        KLogHex(SendVerb(Dac, VERB_GET_STREAM_FORMAT, 0), 4);
        KLog(" strm ");
        KLogHex(SendVerb(Dac, VERB_GET_CHANNEL_STREAM_ID, 0), 2);
        KLog(" amp ");
        KLogHex(SendVerb(Dac, VERB_GET_AMP_GAIN_MUTE, 0xA000), 2);
        KLog(" pwr ");
        KLogHex(SendVerb(Dac, VERB_GET_POWER_STATE, 0), 2);
        KLog("\n");
    }

    for (int i = 0; i < EnabledPinCount; i++) {
        uint8_t Pin = EnabledPins[i];

        KLog("HDA: chk pin ");
        KLogHex(Pin, 2);
        KLog(" ctl ");
        KLogHex(SendVerb(Pin, VERB_GET_PIN_WIDGET_CTL, 0), 2);
        KLog(" amp ");
        KLogHex(SendVerb(Pin, VERB_GET_AMP_GAIN_MUTE, 0xA000), 2);
        KLog(" pwr ");
        KLogHex(SendVerb(Pin, VERB_GET_POWER_STATE, 0), 2);
        KLog("\n");
    }
}

static void *SampleBuffer;
static void *BdlPage;

static void
FillSquareWave(void)
{
    int16_t *Samples = (int16_t *)SampleBuffer;
    const int PeriodFrames = 128;
    const int16_t Amplitude = 12000;

    for (int i = 0; i < BUFFER_FRAMES; i++) {
        int16_t Value = ((i % PeriodFrames) < (PeriodFrames / 2)) ? Amplitude : -Amplitude;
        Samples[i * 2] = Value;
        Samples[i * 2 + 1] = Value;
    }
}

static int
SetupStream(void)
{
    SampleBuffer = AllocPageBelow4G();
    BdlPage = AllocPageBelow4G();

    if (!SampleBuffer || !BdlPage) {
        return 0;
    }

    FillSquareWave();
    FlushRange(SampleBuffer, BUFFER_FRAMES * 2 * sizeof(int16_t));

    uint32_t *Bdl = (uint32_t *)BdlPage;
    uint32_t BufferBytes = BUFFER_FRAMES * 2 * sizeof(int16_t);

    Bdl[0] = (uint32_t)(uintptr_t)SampleBuffer;
    Bdl[1] = 0;
    Bdl[2] = BufferBytes;
    Bdl[3] = 1;
    FlushRange(BdlPage, 64);

    uint32_t SdBase = OutStreamBase;

    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) | 0x01);
    for (int i = 0; i < 100000; i++) {
        if (Read8(SdBase + SDCTL_OFF) & 0x01) {
            break;
        }
        Wait(50);
    }
    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) & ~0x01);
    for (int i = 0; i < 100000; i++) {
        if (!(Read8(SdBase + SDCTL_OFF) & 0x01)) {
            break;
        }
        Wait(50);
    }

    Write32(SdBase + SDBDPL_OFF, (uint32_t)(uintptr_t)BdlPage);
    Write32(SdBase + SDBDPU_OFF, 0);
    Write16(SdBase + SDLVI_OFF, 0);
    Write32(SdBase + SDCBL_OFF, BufferBytes);
    Write16(SdBase + SDFMT_OFF, 0x0011);

    Write8(SdBase + SDCTL_OFF + 2, (Read8(SdBase + SDCTL_OFF + 2) & 0x0F) | ((STREAM_TAG & 0xF) << 4));

    return 1;
}

#define PLAY_PAGES      32
#define PLAY_PAGE_BYTES HDA_STREAM_PAGE_BYTES

static void *PlayPages[PLAY_PAGES];
static void *PlayBdl;
static int PlayAllocated;
static int PlayActive;
static int PlayStarted;
static uint32_t PlayFilled;
static uint32_t PlayHwTotal;
static uint32_t PlayLastHw;

static void
ResetStreamEngine(void)
{
    uint32_t SdBase = OutStreamBase;

    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) & ~0x02);
    for (int i = 0; i < 100000; i++) {
        if (!(Read8(SdBase + SDCTL_OFF) & 0x02)) {
            break;
        }
        Wait(50);
    }

    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) | 0x01);
    for (int i = 0; i < 100000; i++) {
        if (Read8(SdBase + SDCTL_OFF) & 0x01) {
            break;
        }
        Wait(50);
    }
    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) & ~0x01);
    for (int i = 0; i < 100000; i++) {
        if (!(Read8(SdBase + SDCTL_OFF) & 0x01)) {
            break;
        }
        Wait(50);
    }
}

static void
ProgramStream(void *Bdl, uint32_t TotalBytes, uint16_t LastIndex, uint16_t Format)
{
    uint32_t SdBase = OutStreamBase;

    ResetStreamEngine();

    Write32(SdBase + SDBDPL_OFF, (uint32_t)(uintptr_t)Bdl);
    Write32(SdBase + SDBDPU_OFF, 0);
    Write16(SdBase + SDLVI_OFF, LastIndex);
    Write32(SdBase + SDCBL_OFF, TotalBytes);
    Write16(SdBase + SDFMT_OFF, Format);
    Write8(SdBase + SDCTL_OFF + 2, (Read8(SdBase + SDCTL_OFF + 2) & 0x0F) | ((STREAM_TAG & 0xF) << 4));

    for (int i = 0; i < UsedDacCount; i++) {
        SendVerb(UsedDacs[i], VERB_SET_STREAM_FORMAT, Format);
    }
}

static uint16_t
FormatForRate(uint32_t Rate)
{
    switch (Rate) {
    case 8000:   return 0x0511;
    case 11025:  return 0x4311;
    case 12000:  return 0x0311;
    case 16000:  return 0x0211;
    case 22050:  return 0x4111;
    case 24000:  return 0x0111;
    case 32000:  return 0x0A11;
    case 44100:  return 0x4011;
    case 48000:  return 0x0011;
    case 88200:  return 0x4811;
    case 96000:  return 0x0811;
    case 176400: return 0x5811;
    case 192000: return 0x1811;
    default:     return 0;
    }
}

static void
UpdatePlayPosition(void)
{
    if (!PlayStarted) {
        return;
    }

    uint32_t Hw = Read32(OutStreamBase + SDLPIB_OFF) / PLAY_PAGE_BYTES;

    if (Hw >= PLAY_PAGES) {
        Hw = PLAY_PAGES - 1;
    }

    PlayHwTotal += (Hw + PLAY_PAGES - PlayLastHw) % PLAY_PAGES;
    PlayLastHw = Hw;
}

int
HdaStreamOpen(uint32_t SampleRate)
{
    if (!StreamOk || PlayActive) {
        return HDA_STREAM_ERR_UNAVAILABLE;
    }

    uint16_t Format = FormatForRate(SampleRate);

    if (Format == 0) {
        return HDA_STREAM_ERR_RATE;
    }

    if (!PlayAllocated) {
        PlayBdl = AllocPageBelow4G();
        int Got = 0;

        if (PlayBdl) {
            for (; Got < PLAY_PAGES; Got++) {
                PlayPages[Got] = AllocPageBelow4G();
                if (!PlayPages[Got]) {
                    break;
                }
            }
        }

        if (!PlayBdl || Got < PLAY_PAGES) {
            for (int i = 0; i < Got; i++) {
                FreePage(PlayPages[i]);
            }
            if (PlayBdl) {
                FreePage(PlayBdl);
                PlayBdl = 0;
            }
            return HDA_STREAM_ERR_MEMORY;
        }

        PlayAllocated = 1;
    }

    uint32_t *Bdl = (uint32_t *)PlayBdl;

    for (int i = 0; i < PLAY_PAGES; i++) {
        uint8_t *Page = (uint8_t *)PlayPages[i];

        for (uint32_t b = 0; b < PLAY_PAGE_BYTES; b++) {
            Page[b] = 0;
        }
        FlushRange(Page, PLAY_PAGE_BYTES);

        Bdl[i * 4 + 0] = (uint32_t)(uintptr_t)Page;
        Bdl[i * 4 + 1] = 0;
        Bdl[i * 4 + 2] = PLAY_PAGE_BYTES;
        Bdl[i * 4 + 3] = 0;
    }
    FlushRange(PlayBdl, PLAY_PAGES * 16);

    ProgramStream(PlayBdl, PLAY_PAGES * PLAY_PAGE_BYTES, PLAY_PAGES - 1, Format);

    PlayFilled = 0;
    PlayHwTotal = 0;
    PlayLastHw = 0;
    PlayStarted = 0;
    PlayActive = 1;
    return 1;
}

void *
HdaStreamNextPage(void)
{
    if (!PlayActive) {
        return 0;
    }

    UpdatePlayPosition();

    if (PlayFilled >= PlayHwTotal + PLAY_PAGES) {
        return 0;
    }

    return PlayPages[PlayFilled % PLAY_PAGES];
}

void
HdaStreamCommit(void)
{
    if (!PlayActive) {
        return;
    }

    FlushRange(PlayPages[PlayFilled % PLAY_PAGES], PLAY_PAGE_BYTES);
    PlayFilled++;
}

void
HdaStreamStart(void)
{
    if (!PlayActive) {
        return;
    }

    Write8(OutStreamBase + SDSTS_OFF, 0x1C);
    Write8(OutStreamBase + SDCTL_OFF, Read8(OutStreamBase + SDCTL_OFF) | 0x02);

    for (int i = 0; i < 50; i++) {
        if (Read32(OutStreamBase + SDLPIB_OFF) < PLAY_PAGE_BYTES) {
            break;
        }
        Sleep(1);
    }

    PlayHwTotal = 0;
    PlayLastHw = 0;
    PlayStarted = 1;
}

uint32_t
HdaStreamFilled(void)
{
    return PlayFilled;
}

uint32_t
HdaStreamPlayed(void)
{
    if (!PlayActive) {
        return 0;
    }

    UpdatePlayPosition();
    return PlayHwTotal;
}

void
HdaStreamClose(void)
{
    if (!PlayActive) {
        return;
    }

    PlayActive = 0;
    PlayStarted = 0;
    ProgramStream(BdlPage, BUFFER_FRAMES * 2 * sizeof(int16_t), 0, 0x0011);
}

HDA_STATUS
HdaInit(uint32_t Bar0)
{
    HDA_STATUS Status;
    Status.Found = 1;
    Status.CodecFound = 0;
    Status.CorbEntriesUsed = 0;
    Status.RirbEntriesUsed = 0;
    Status.RootFgCount = 0;
    Status.FgStartNid = 0;
    Status.RepeatSameCallRaw = 0xFFFFFFFF;
    Status.AfgFound = 0;
    Status.LastFgTypeRaw = 0;
    Status.WpAfterFirst = 0;
    Status.WpBeforeSecond = 0;
    Status.CorbWpReadback = 0;
    Status.CorbRpAfterFirst = 0;
    Status.CorbSts = 0;
    Status.WidgetCount = 0;
    Status.DacFound = 0;
    Status.PinFound = 0;
    Status.PathLinked = 0;
    Status.StreamStarted = 0;

    HdaBase = (volatile uint8_t *)(uintptr_t)Bar0;
    OutStreamBase = REG_SD_BASE + (uint32_t)((Read16(REG_GCAP) >> 8) & 0xF) * SD_SIZE;

    if (!ResetController()) {
        return Status;
    }

    Write32(REG_INTCTL, 0);

    if (!SetupCorbRirb()) {
        return Status;
    }

    Status.CorbEntriesUsed = CorbEntries;
    Status.RirbEntriesUsed = RirbEntries;

    Wait(20000);
    uint16_t States = Read16(REG_STATESTS);

    if (States == 0) {
        return Status;
    }

    for (uint8_t i = 0; i < 15; i++) {
        if (States & (1u << i)) {
            CodecAddr = i;
            break;
        }
    }

    Status.CodecFound = 1;

    uint32_t RootNodeCount = GetParam(0, PARAM_NODE_COUNT);
    uint8_t FgStart = (uint8_t)((RootNodeCount >> 16) & 0xFF);
    uint8_t FgCount = (uint8_t)(RootNodeCount & 0xFF);
    Status.RootFgCount = FgCount;
    Status.FgStartNid = FgStart;
    Status.RepeatSameCallRaw = GetParam(0, PARAM_NODE_COUNT);
    Status.WpAfterFirst = LastRirbWp;
    Status.WpBeforeSecond = Read16(REG_RIRBWP) & 0xFF;
    Status.CorbRpAfterFirst = Read16(REG_CORBRP) & 0xFF;
    Status.CorbSts = Read8(REG_CORBSTS);

    uint8_t AfgNid = 0xFF;
    for (uint8_t i = 0; i < FgCount; i++) {
        uint8_t Nid = (uint8_t)(FgStart + i);
        uint32_t FgType = GetParam(Nid, PARAM_FUNCTION_GROUP_TYPE);
        Status.LastFgTypeRaw = FgType;
        Status.CorbWpReadback = Read16(REG_CORBWP);
        if ((FgType & 0xFF) == 0x01) {
            AfgNid = Nid;
            break;
        }
    }

    if (AfgNid == 0xFF) {
        return Status;
    }
    Status.AfgFound = 1;

    KLog("HDA: vendor ");
    KLogHex(GetParam(0, PARAM_VENDOR_ID), 8);
    KLog(", out stream base ");
    KLogHex(OutStreamBase, 2);
    KLog("\n");

    SendVerb(AfgNid, VERB_SET_POWER_STATE, 0);
    Sleep(20);

    AfgInAmpCaps = GetParam(AfgNid, PARAM_IN_AMP_CAPS);
    AfgOutAmpCaps = GetParam(AfgNid, PARAM_OUT_AMP_CAPS);

    Status.WidgetCount = WalkWidgets(AfgNid);

    if (DacCount == 0) {
        return Status;
    }
    Status.DacFound = 1;

    int Candidates = 0;
    int Enabled = RouteOutputPins(1, &Candidates);

    if (Enabled == 0) {
        Candidates = 0;
        Enabled = RouteOutputPins(0, &Candidates);
    }

    if (Candidates == 0) {
        return Status;
    }
    Status.PinFound = 1;

    if (Enabled == 0) {
        return Status;
    }
    Status.PathLinked = 1;

    ConfigureUsedDacs();

    KLog("HDA: afg gpio ");
    KLogHex(GetParam(AfgNid, PARAM_GPIO_COUNT), 1);
    KLog(", out amp caps ");
    KLogHex(AfgOutAmpCaps, 1);
    KLog("\n");

    LogVerify();

    if (!SetupStream()) {
        return Status;
    }

    Status.StreamStarted = 1;
    Status.Found = 1;
    StreamOk = 1;

    return Status;
}

void
HdaPlayTestTone(void)
{
    uint32_t SdBase = OutStreamBase;
    uint32_t Samples[8];

    Write8(SdBase + SDSTS_OFF, 0x1C);
    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) | 0x02);

    for (int i = 0; i < 8; i++) {
        Sleep(190);
        Samples[i] = Read32(SdBase + SDLPIB_OFF);
    }

    uint8_t Sts = Read8(SdBase + SDSTS_OFF);
    uint8_t Ctl = Read8(SdBase + SDCTL_OFF);
    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) & ~0x02);

    KLog("HDA: beep lpib");
    for (int i = 0; i < 8; i++) {
        KLog(" ");
        KLogUInt(Samples[i]);
    }
    KLog("\n");
    KLog("HDA: beep sdsts ");
    KLogHex(Sts, 2);
    KLog(" sdctl ");
    KLogHex(Ctl, 2);
    KLog("\n");
}
