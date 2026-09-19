#include "hda.h"
#include "pmm.h"
#include "idt.h"

#define REG_GCAP        0x00
#define REG_GCTL        0x08
#define REG_STATESTS    0x0E
#define REG_INTCTL      0x18
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48
#define REG_CORBRP      0x4A
#define REG_CORBCTL     0x4C
#define REG_CORBSIZE    0x4E
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58
#define REG_RINTCNT     0x5A
#define REG_RIRBCTL     0x5C
#define REG_RIRBSIZE    0x5E
#define REG_SD0_BASE    0x80
#define SD_SIZE         0x20

#define SDCTL_OFF   0x00
#define SDSTS_OFF   0x03
#define SDLPIB_OFF  0x04
#define SDCBL_OFF   0x08
#define SDLVI_OFF   0x0C
#define SDFMT_OFF   0x14
#define SDBDPL_OFF  0x18
#define SDBDPU_OFF  0x1C

#define VERB_GET_PARAMETER          0xF00
#define PARAM_NODE_COUNT            0x04
#define PARAM_FUNCTION_GROUP_TYPE   0x05
#define PARAM_AUDIO_WIDGET_CAPS     0x09
#define PARAM_PIN_CAPS              0x0C
#define PARAM_CONN_LIST_LENGTH      0x0E

#define VERB_GET_CONN_LIST_ENTRY    0xF02
#define VERB_SET_CONN_SELECT        0x701
#define VERB_SET_AMP_GAIN_MUTE      0x300
#define VERB_SET_STREAM_FORMAT      0x200
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
    Wait(1000);
    Write16(REG_CORBRP, 0);
    Write16(REG_CORBWP, 0);
    CorbWritePos = 0;

    Write32(REG_RIRBLBASE, (uint32_t)(uintptr_t)RirbPage);
    Write32(REG_RIRBUBASE, 0);
    Write8(REG_RIRBSIZE, RirbSizeBits);
    Write16(REG_RIRBWP, 0x8000);
    RirbReadPos = 0xFFFF;
    Write16(REG_RINTCNT, 1);

    Write8(REG_CORBCTL, 0x02);
    Write8(REG_RIRBCTL, 0x02);

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

    uint32_t Response = 0;
    for (int i = 0; i < 1000000; i++) {
        uint16_t Wp = Read16(REG_RIRBWP) & 0xFF;
        if (Wp != RirbReadPos) {
            uint16_t Pos = (uint16_t)(Wp % RirbEntries);
            uint64_t Entry = Rirb[Pos];
            RirbReadPos = Wp;
            LastRirbWp = Wp;
            Response = (uint32_t)(Entry & 0xFFFFFFFF);
            return Response;
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

static uint8_t FoundDac = 0xFF;
static uint8_t FoundPin = 0xFF;

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
        uint32_t Resp = SendVerb(Nid, VERB_GET_CONN_LIST_ENTRY, i);
        uint8_t Entry = (uint8_t)((Resp >> ((i % 4) * 8)) & 0x7F);

        int Match = (Entry == TargetDac);
        if (!Match) {
            Match = ConnectPathToDac(Entry, TargetDac, Depth - 1);
        }

        if (Match) {
            SendVerb(Nid, VERB_SET_CONN_SELECT, i);
            SendVerb(Nid, VERB_SET_AMP_GAIN_MUTE, 0xB000 | (i << 8));
            SendVerb(Nid, VERB_SET_AMP_GAIN_MUTE, 0x7000);
            return 1;
        }
    }

    return 0;
}

static uint8_t
WalkWidgets(uint8_t AfgNid)
{
    uint32_t NodeCount = GetParam(AfgNid, PARAM_NODE_COUNT);
    uint8_t StartNid = (uint8_t)((NodeCount >> 16) & 0xFF);
    uint8_t Count = (uint8_t)(NodeCount & 0xFF);

    for (uint8_t i = 0; i < Count; i++) {
        uint8_t Nid = (uint8_t)(StartNid + i);
        uint32_t Caps = GetParam(Nid, PARAM_AUDIO_WIDGET_CAPS);
        uint8_t Type = (uint8_t)((Caps >> 20) & 0xF);

        if (Type == WIDGET_TYPE_AUDIO_OUTPUT && FoundDac == 0xFF) {
            FoundDac = Nid;
        }

        if (Type == WIDGET_TYPE_PIN_COMPLEX && FoundPin == 0xFF) {
            uint32_t PinCaps = GetParam(Nid, PARAM_PIN_CAPS);
            if (PinCaps & (1u << 4)) {
                FoundPin = Nid;
            }
        }
    }

    return Count;
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

    uint32_t *Bdl = (uint32_t *)BdlPage;
    uint32_t BufferBytes = BUFFER_FRAMES * 2 * sizeof(int16_t);

    Bdl[0] = (uint32_t)(uintptr_t)SampleBuffer;
    Bdl[1] = 0;
    Bdl[2] = BufferBytes;
    Bdl[3] = 1;

    uint32_t SdBase = REG_SD0_BASE;

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

    SendVerb(FoundDac, VERB_SET_CHANNEL_STREAM_ID, (STREAM_TAG << 4) | 0);
    SendVerb(FoundDac, VERB_SET_STREAM_FORMAT, 0x0011);
    SendVerb(FoundDac, VERB_SET_AMP_GAIN_MUTE, 0xB000);
    SendVerb(FoundDac, VERB_SET_POWER_STATE, 0);

    SendVerb(FoundPin, VERB_SET_PIN_WIDGET_CTL, 0x40);
    SendVerb(FoundPin, VERB_SET_POWER_STATE, 0);
    SendVerb(FoundPin, VERB_SET_EAPD_BTL_ENABLE, 0x02);

    return 1;
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
    Status.AfgFound = 0;
    Status.LastFgTypeRaw = 0;
    Status.WpAfterFirst = 0;
    Status.WpBeforeSecond = 0;
    Status.CorbWpReadback = 0;
    Status.WidgetCount = 0;
    Status.DacFound = 0;
    Status.PinFound = 0;
    Status.PathLinked = 0;
    Status.StreamStarted = 0;

    HdaBase = (volatile uint8_t *)(uintptr_t)Bar0;

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
    Status.WpAfterFirst = LastRirbWp;
    Status.WpBeforeSecond = Read16(REG_RIRBWP) & 0xFF;

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

    SendVerb(AfgNid, VERB_SET_POWER_STATE, 0);

    Status.WidgetCount = WalkWidgets(AfgNid);

    if (FoundDac == 0xFF) {
        return Status;
    }
    Status.DacFound = 1;

    if (FoundPin == 0xFF) {
        return Status;
    }
    Status.PinFound = 1;

    if (!ConnectPathToDac(FoundPin, FoundDac, 4)) {
        return Status;
    }
    Status.PathLinked = 1;

    if (!SetupStream()) {
        return Status;
    }

    Status.StreamStarted = 1;
    Status.Found = 1;

    return Status;
}

void
HdaPlayTestTone(void)
{
    uint32_t SdBase = REG_SD0_BASE;
    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) | 0x02);
    Sleep(1500);
    Write8(SdBase + SDCTL_OFF, Read8(SdBase + SDCTL_OFF) & ~0x02);
}
