#pragma once

#include <stdint.h>

typedef struct {
    int Found;
    int CodecFound;
    uint32_t CorbEntriesUsed;
    uint32_t RirbEntriesUsed;
    uint32_t RootFgCount;
    uint32_t FgStartNid;
    int AfgFound;
    uint32_t LastFgTypeRaw;
    uint32_t WpAfterFirst;
    uint32_t WpBeforeSecond;
    uint32_t CorbWpReadback;
    uint32_t WidgetCount;
    int DacFound;
    int PinFound;
    int PathLinked;
    int StreamStarted;
} HDA_STATUS;

HDA_STATUS HdaInit(uint32_t Bar0);
void HdaPlayTestTone(void);
