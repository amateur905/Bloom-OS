#pragma once

#include <stdint.h>
#include "gfx.h"

#define COL_BG        0x00201040
#define COL_BG_ALT    0x00241248
#define COL_PANEL     0x00180C34
#define COL_HEADER    0x00342070
#define COL_SELECT    0x00583CA0
#define COL_TEXT      0x00E0D0FF
#define COL_DIM       0x008878A8
#define COL_ACCENT    0x0080FFC0
#define COL_ERROR     0x00FF7070
#define COL_WARN      0x00F0C060
#define COL_FOLDER    0x00F0C060
#define COL_FOLDER_DK 0x00B88830
#define COL_FILE      0x00A8A0D8
#define COL_BUTTON    0x00483088
#define COL_TRACK     0x00100820
#define COL_MATCH     0x00806018

extern uint32_t UiW;
extern uint32_t UiH;
extern int UiMouseX;
extern int UiMouseY;

void UiInit(void);
void UiText(uint32_t X, uint32_t Y, const char *Str, uint32_t Color, int MaxChars);
void UiCursorHide(void);
void UiCursorShow(void);
int UiPollMouse(uint8_t *Pressed);
int UiInside(int X, int Y, uint32_t RX, uint32_t RY, uint32_t RW, uint32_t RH);

int UiStrLen(const char *s);
void UiStrCopy(char *Dst, const char *Src, int Max);
void UiStrAppend(char *Dst, const char *Src, int Max);
int UiStrEqual(const char *a, const char *b);
char UiLower(char c);
int UiCompareNames(const char *a, const char *b);
void UiUIntToText(uint64_t Value, char *Out);
void UiFormatSize(uint32_t Size, char *Out);
void UiFormatTime(uint32_t Seconds, char *Out);
