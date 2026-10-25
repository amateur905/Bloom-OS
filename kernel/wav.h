#pragma once

#include <stdint.h>
#include "fs.h"

#define WAV_ERR_READ     -1
#define WAV_ERR_NOTWAV   -2
#define WAV_ERR_FORMAT   -3
#define WAV_ERR_NODATA   -4

typedef struct {
    uint16_t Channels;
    uint16_t BitsPerSample;
    uint16_t BlockAlign;
    uint32_t SampleRate;
    uint32_t DataOffset;
    uint32_t DataBytes;
} WAV_INFO;

int WavParse(const FS_NODE *File, WAV_INFO *Info);
void WavConvert(const WAV_INFO *Info, const uint8_t *Src, uint32_t Frames, int16_t *Dst);
const char *WavErrorText(int Code);
