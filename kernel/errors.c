#include <stdint.h>
#include "errors.h"
#include "klog.h"

typedef struct {
    int Code;
    int Warning;
    const char *Text;
} ERROR_ENTRY;

static const ERROR_ENTRY Table[] = {
    {E_NO_VOLUMES,     0, "no readable volumes found"},
    {E_FOLDER_READ,    0, "cannot read folder"},
    {E_FOLDER_LARGE,   1, "folder is large, showing the first 1024 entries"},
    {E_NAME_LONG,      0, "name is too long to open"},
    {E_PATH_LONG,      0, "path is too long"},
    {E_FILE_OPEN,      0, "cannot open file"},
    {E_FILE_READ,      0, "read error while loading file"},
    {E_FILE_TRUNCATED, 1, "file is large, showing only the first 256 KB"},
    {E_FILE_BINARY,    1, "binary file, opened in hex view"},
    {E_FILE_EMPTY,     1, "file is empty"},
    {E_VIEW_LINES,     1, "too many lines, view is truncated"},
    {E_VIEW_RANGE,     0, "internal viewer range error"},
    {E_AUDIO_RATE,     0, "sample rate is not supported by the HDA driver"},
    {E_AUDIO_FORMAT,   0, "unsupported WAV format (need PCM 8/16/24/32-bit, 1-8 channels)"},
    {E_AUDIO_DEVICE,   0, "HDA audio stream is not available"},
    {E_AUDIO_MEMORY,   0, "not enough low memory for audio buffers"},
    {E_AUDIO_READ,     0, "read error during playback"},
    {E_AUDIO_NOTWAV,   0, "not a RIFF/WAVE file"},
    {E_INTERNAL_SEL,   0, "internal selection error, auto-corrected"},
    {E_SCREEN_SMALL,   0, "screen is too small, need at least 800x480"},
    {E_TEXT_NOTFOUND,  1, "text not found"},
    {E_INFO_FAILED,    0, "cannot read file details"},
};

static int
Length(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void
Append(char *Dst, const char *Src, int Max)
{
    int n = Length(Dst);
    int i = 0;

    while (Src[i] && n + i < Max - 1) {
        Dst[n + i] = Src[i];
        i++;
    }

    Dst[n + i] = 0;
}

static void
UIntText(int Value, char *Out)
{
    char Tmp[12];
    int Pos = 11;
    Tmp[Pos] = 0;

    if (Value == 0) {
        Tmp[--Pos] = '0';
    }

    while (Value > 0) {
        Tmp[--Pos] = (char)('0' + Value % 10);
        Value /= 10;
    }

    int i = 0;
    while (Tmp[Pos]) {
        Out[i++] = Tmp[Pos++];
    }
    Out[i] = 0;
}

const char *
ErrorText(int Code)
{
    for (unsigned i = 0; i < sizeof(Table) / sizeof(Table[0]); i++) {
        if (Table[i].Code == Code) {
            return Table[i].Text;
        }
    }

    return "unknown error";
}

int
ErrorIsWarning(int Code)
{
    for (unsigned i = 0; i < sizeof(Table) / sizeof(Table[0]); i++) {
        if (Table[i].Code == Code) {
            return Table[i].Warning;
        }
    }

    return 0;
}

void
ErrorFormat(int Code, const char *Detail, char *Out, int Max)
{
    char Num[12];

    UIntText(Code, Num);
    Out[0] = 0;
    Append(Out, "E", Max);
    Append(Out, Num, Max);
    Append(Out, " ", Max);
    Append(Out, ErrorText(Code), Max);

    if (Detail && Detail[0]) {
        Append(Out, ": ", Max);
        Append(Out, Detail, Max);
    }
}

void
ErrorLog(const char *Source, int Code, const char *Detail)
{
    KLog(Source);
    KLog(": E");
    KLogUInt((uint64_t)Code);
    KLog(" ");
    KLog(ErrorText(Code));

    if (Detail && Detail[0]) {
        KLog(" [");
        KLog(Detail);
        KLog("]");
    }

    KLog("\n");
}
