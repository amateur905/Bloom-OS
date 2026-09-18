#pragma once

#include <stdint.h>

typedef struct {
    int Found;
    int CodecFound;
    uint32_t RootFgCount;
    int AfgFound;
    uint32_t LastFgTypeRaw;
    uint32_t WidgetCount;
    int DacFound;
    int PinFound;
    int PathLinked;
    int StreamStarted;
} HDA_STATUS;

HDA_STATUS HdaInit(uint32_t Bar0);
void HdaPlayTestTone(void);
