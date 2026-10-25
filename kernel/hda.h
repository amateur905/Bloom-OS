#pragma once

#include <stdint.h>

typedef struct {
    int Found;
    int CodecFound;
    uint32_t CorbEntriesUsed;
    uint32_t RirbEntriesUsed;
    uint32_t RootFgCount;
    uint32_t FgStartNid;
    uint32_t RepeatSameCallRaw;
    int AfgFound;
    uint32_t LastFgTypeRaw;
    uint32_t WpAfterFirst;
    uint32_t WpBeforeSecond;
    uint32_t CorbWpReadback;
    uint32_t CorbRpAfterFirst;
    uint32_t CorbSts;
    uint32_t WidgetCount;
    int DacFound;
    int PinFound;
    int PathLinked;
    int StreamStarted;
} HDA_STATUS;

HDA_STATUS HdaInit(uint32_t Bar0);
void HdaPlayTestTone(void);

#define HDA_STREAM_PAGE_BYTES 4096
#define HDA_STREAM_PAGE_FRAMES (HDA_STREAM_PAGE_BYTES / 4)
#define HDA_STREAM_ERR_UNAVAILABLE -1
#define HDA_STREAM_ERR_RATE -2
#define HDA_STREAM_ERR_MEMORY -3

int HdaStreamOpen(uint32_t SampleRate);
void *HdaStreamNextPage(void);
void HdaStreamCommit(void);
void HdaStreamStart(void);
uint32_t HdaStreamFilled(void);
uint32_t HdaStreamPlayed(void);
void HdaStreamClose(void);
