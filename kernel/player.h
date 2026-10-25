#pragma once

#include <stdint.h>

typedef struct {
    uint32_t SampleRate;
    uint16_t BitsPerSample;
    uint16_t Channels;
    uint32_t TotalSeconds;
} PLAYER_INFO;

#define PLAYER_ERR_NONE   0
#define PLAYER_ERR_FS     1
#define PLAYER_ERR_DIR    2
#define PLAYER_ERR_NOTWAV 3
#define PLAYER_ERR_FORMAT 4
#define PLAYER_ERR_NODATA 5
#define PLAYER_ERR_RATE   6
#define PLAYER_ERR_MEMORY 7
#define PLAYER_ERR_DEVICE 8
#define PLAYER_ERR_READ   9

int PlayerLastError(void);
int PlayerTakeFinished(void);
int PlayerStart(const char *Path, const char **Error);
void PlayerPoll(void);
void PlayerStop(void);
int PlayerActive(void);
int PlayerReadFailed(void);
uint32_t PlayerElapsedSeconds(void);
const PLAYER_INFO *PlayerGetInfo(void);

int PlayWavFile(const char *Path);
