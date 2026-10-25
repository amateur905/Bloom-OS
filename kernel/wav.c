#include "wav.h"

static uint16_t
Le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int
TagEq(const uint8_t *p, const char *Tag)
{
    return p[0] == (uint8_t)Tag[0] && p[1] == (uint8_t)Tag[1] && p[2] == (uint8_t)Tag[2] && p[3] == (uint8_t)Tag[3];
}

int
WavParse(const FS_NODE *File, WAV_INFO *Info)
{
    uint8_t Head[12];
    uint8_t Chunk[8];
    uint8_t Fmt[40];
    uint32_t FileSize = File->Entry.Size;

    if (FileSize < 12 || FsRead(File, 0, Head, 12) != 12) {
        return WAV_ERR_NOTWAV;
    }

    if (!TagEq(Head, "RIFF") || !TagEq(Head + 8, "WAVE")) {
        return WAV_ERR_NOTWAV;
    }

    int HaveFmt = 0;
    uint32_t Pos = 12;

    Info->DataBytes = 0;
    Info->DataOffset = 0;

    while (Pos + 8 <= FileSize) {
        if (FsRead(File, Pos, Chunk, 8) != 8) {
            return WAV_ERR_READ;
        }

        uint32_t Size = Le32(Chunk + 4);
        uint32_t Body = Pos + 8;

        if (TagEq(Chunk, "fmt ")) {
            uint32_t Want = Size < sizeof(Fmt) ? Size : (uint32_t)sizeof(Fmt);

            if (Want < 16 || FsRead(File, Body, Fmt, Want) != Want) {
                return WAV_ERR_FORMAT;
            }

            uint16_t Tag = Le16(Fmt);

            if (Tag == 0xFFFE && Want >= 26) {
                Tag = Le16(Fmt + 24);
            }

            if (Tag != 1) {
                return WAV_ERR_FORMAT;
            }

            Info->Channels = Le16(Fmt + 2);
            Info->SampleRate = Le32(Fmt + 4);
            Info->BlockAlign = Le16(Fmt + 12);
            Info->BitsPerSample = Le16(Fmt + 14);

            if (Info->Channels == 0 || Info->Channels > 8) {
                return WAV_ERR_FORMAT;
            }

            if (Info->BitsPerSample != 8 && Info->BitsPerSample != 16 &&
                Info->BitsPerSample != 24 && Info->BitsPerSample != 32) {
                return WAV_ERR_FORMAT;
            }

            if (Info->BlockAlign != Info->Channels * (Info->BitsPerSample / 8)) {
                return WAV_ERR_FORMAT;
            }

            HaveFmt = 1;
        } else if (TagEq(Chunk, "data")) {
            if (!HaveFmt) {
                return WAV_ERR_FORMAT;
            }

            uint32_t Avail = FileSize - Body;

            if (Size > Avail) {
                Size = Avail;
            }

            Size -= Size % Info->BlockAlign;

            if (Size == 0) {
                return WAV_ERR_NODATA;
            }

            Info->DataOffset = Body;
            Info->DataBytes = Size;
            return 1;
        }

        uint32_t Next = Body + Size + (Size & 1);

        if (Next <= Pos) {
            break;
        }
        Pos = Next;
    }

    return HaveFmt ? WAV_ERR_NODATA : WAV_ERR_FORMAT;
}

static int16_t
DecodeSample(const uint8_t *p, uint16_t Bits)
{
    switch (Bits) {
    case 8:  return (int16_t)(((int)p[0] - 128) << 8);
    case 16: return (int16_t)Le16(p);
    case 24: return (int16_t)Le16(p + 1);
    default: return (int16_t)Le16(p + 2);
    }
}

void
WavConvert(const WAV_INFO *Info, const uint8_t *Src, uint32_t Frames, int16_t *Dst)
{
    uint16_t Bytes = Info->BitsPerSample / 8;

    for (uint32_t i = 0; i < Frames; i++) {
        const uint8_t *Frame = Src + (uint32_t)i * Info->BlockAlign;
        int16_t Left = DecodeSample(Frame, Info->BitsPerSample);
        int16_t Right = Info->Channels > 1 ? DecodeSample(Frame + Bytes, Info->BitsPerSample) : Left;

        Dst[i * 2] = Left;
        Dst[i * 2 + 1] = Right;
    }
}

const char *
WavErrorText(int Code)
{
    switch (Code) {
    case WAV_ERR_READ:   return "read error";
    case WAV_ERR_NOTWAV: return "not a RIFF/WAVE file";
    case WAV_ERR_FORMAT: return "unsupported WAV format (need PCM 8/16/24/32-bit, 1-8 channels)";
    case WAV_ERR_NODATA: return "no audio data";
    default:             return "unknown error";
    }
}
