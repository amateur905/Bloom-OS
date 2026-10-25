#include "player.h"
#include "wav.h"
#include "fs.h"
#include "hda.h"
#include "console.h"
#include "keyboard.h"
#include "idt.h"

static FS_NODE PlayNode;
static WAV_INFO Info;
static PLAYER_INFO Public;
static uint8_t SourceBuffer[HDA_STREAM_PAGE_FRAMES * 32];

static int LastError;
static int Finished;
static int Active;
static int Started;
static int ReadFailed;
static uint32_t TotalFrames;
static uint32_t FramesDone;
static uint32_t DataPages;

int
PlayerStart(const char *Path, const char **Error)
{
    const char *Dummy;

    if (Error == 0) {
        Error = &Dummy;
    }

    PlayerStop();
    LastError = PLAYER_ERR_NONE;
    Finished = 0;

    int Status = FsResolve(Path, &PlayNode);

    if (Status != FS_OK) {
        *Error = FsErrorText(Status);
        LastError = PLAYER_ERR_FS;
        return 0;
    }

    if (PlayNode.IsDir) {
        *Error = "is a directory";
        LastError = PLAYER_ERR_DIR;
        return 0;
    }

    Status = WavParse(&PlayNode, &Info);

    if (Status != 1) {
        *Error = WavErrorText(Status);
        LastError = Status == WAV_ERR_NOTWAV ? PLAYER_ERR_NOTWAV :
                    Status == WAV_ERR_NODATA ? PLAYER_ERR_NODATA :
                    Status == WAV_ERR_READ ? PLAYER_ERR_READ : PLAYER_ERR_FORMAT;
        return 0;
    }

    Status = HdaStreamOpen(Info.SampleRate);

    if (Status == HDA_STREAM_ERR_RATE) {
        *Error = "sample rate is not supported by the HDA driver";
        LastError = PLAYER_ERR_RATE;
        return 0;
    }

    if (Status == HDA_STREAM_ERR_MEMORY) {
        *Error = "not enough low memory for audio buffers";
        LastError = PLAYER_ERR_MEMORY;
        return 0;
    }

    if (Status != 1) {
        *Error = "HDA stream is not available";
        LastError = PLAYER_ERR_DEVICE;
        return 0;
    }

    TotalFrames = Info.DataBytes / Info.BlockAlign;
    FramesDone = 0;
    DataPages = 0;
    Started = 0;
    ReadFailed = 0;
    Active = 1;

    Public.SampleRate = Info.SampleRate;
    Public.BitsPerSample = Info.BitsPerSample;
    Public.Channels = Info.Channels;
    Public.TotalSeconds = TotalFrames / Info.SampleRate;

    PlayerPoll();
    return 1;
}

void
PlayerPoll(void)
{
    if (!Active) {
        return;
    }

    void *Page;

    while (FramesDone < TotalFrames && !ReadFailed && (Page = HdaStreamNextPage()) != 0) {
        uint32_t Frames = TotalFrames - FramesDone;

        if (Frames > HDA_STREAM_PAGE_FRAMES) {
            Frames = HDA_STREAM_PAGE_FRAMES;
        }

        uint32_t Got = FsRead(&PlayNode, Info.DataOffset + FramesDone * Info.BlockAlign,
                              SourceBuffer, Frames * Info.BlockAlign);

        if (Got < Info.BlockAlign) {
            ReadFailed = 1;
            break;
        }

        Frames = Got / Info.BlockAlign;

        int16_t *Out = (int16_t *)Page;
        WavConvert(&Info, SourceBuffer, Frames, Out);

        for (uint32_t i = Frames * 2; i < HDA_STREAM_PAGE_FRAMES * 2; i++) {
            Out[i] = 0;
        }

        HdaStreamCommit();
        FramesDone += Frames;
        DataPages++;

        if (!Started && HdaStreamFilled() >= 16) {
            HdaStreamStart();
            Started = 1;
        }
    }

    int InputDone = FramesDone >= TotalFrames || ReadFailed;

    if (!Started && DataPages > 0 && InputDone) {
        HdaStreamStart();
        Started = 1;
    }

    if (DataPages == 0 && InputDone) {
        Finished = 1;
        PlayerStop();
        return;
    }

    if (Started && InputDone && HdaStreamPlayed() >= DataPages) {
        Finished = 1;
        PlayerStop();
    }
}

void
PlayerStop(void)
{
    if (!Active) {
        return;
    }

    HdaStreamClose();
    Active = 0;
    Started = 0;
}

int
PlayerTakeFinished(void)
{
    int Value = Finished;
    Finished = 0;
    return Value;
}

int
PlayerLastError(void)
{
    return LastError;
}

int
PlayerActive(void)
{
    return Active;
}

int
PlayerReadFailed(void)
{
    return ReadFailed;
}

uint32_t
PlayerElapsedSeconds(void)
{
    if (!Active || !Started) {
        return 0;
    }

    uint64_t Frames = (uint64_t)HdaStreamPlayed() * HDA_STREAM_PAGE_FRAMES;
    uint32_t Seconds = (uint32_t)(Frames / Info.SampleRate);

    return Seconds > Public.TotalSeconds ? Public.TotalSeconds : Seconds;
}

const PLAYER_INFO *
PlayerGetInfo(void)
{
    return &Public;
}

static void
PrintError(const char *Text)
{
    ConsoleSetAttr(CONSOLE_ATTR_ERROR);
    ConsolePrint(Text);
    ConsolePutChar('\n');
    ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
}

static void
PrintTime(uint32_t Seconds)
{
    ConsolePrintUInt(Seconds / 60);
    ConsolePutChar(':');
    if ((Seconds % 60) < 10) {
        ConsolePutChar('0');
    }
    ConsolePrintUInt(Seconds % 60);
}

static int
AbortRequested(void)
{
    int Abort = 0;

    while (KeyboardGetKey() != 0) {
        Abort = 1;
    }

    return Abort;
}

int
PlayWavFile(const char *Path)
{
    const char *Error = "";

    if (!PlayerStart(Path, &Error)) {
        PrintError(Error);
        return 0;
    }

    const PLAYER_INFO *P = PlayerGetInfo();

    ConsolePrintUInt(P->SampleRate);
    ConsolePrint(" Hz, ");
    ConsolePrintUInt(P->BitsPerSample);
    ConsolePrint("-bit, ");
    ConsolePrintUInt(P->Channels);
    ConsolePrint(P->Channels == 1 ? " channel, " : " channels, ");
    PrintTime(P->TotalSeconds);
    ConsolePrint("\n");
    ConsolePrint("Playing, press any key to stop\n");
    ConsoleFlush();

    while (KeyboardGetKey() != 0) {
    }

    uint32_t LastShown = 0xFFFFFFFF;
    int Aborted = 0;

    while (PlayerActive()) {
        PlayerPoll();

        if (!PlayerActive()) {
            break;
        }

        if (AbortRequested()) {
            Aborted = 1;
            PlayerStop();
            break;
        }

        uint32_t Seconds = PlayerElapsedSeconds();

        if (Seconds != LastShown) {
            LastShown = Seconds;
            ConsolePutChar('\r');
            PrintTime(Seconds);
            ConsolePutChar('/');
            PrintTime(P->TotalSeconds);
            ConsoleFlush();
        }

        Sleep(2);
    }

    ConsolePutChar('\n');

    if (PlayerReadFailed()) {
        PrintError("read error during playback");
        return 0;
    }

    ConsolePrint(Aborted ? "Stopped\n" : "Done\n");
    return 1;
}
