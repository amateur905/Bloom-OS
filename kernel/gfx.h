#pragma once

#include <stdint.h>
#include "boot_info.h"

#ifndef FONT_WIDTH
#define FONT_WIDTH 8
#endif

#ifndef FONT_HEIGHT
#define FONT_HEIGHT 16
#endif

extern volatile uint32_t *FrameBuffer;
extern uint64_t ScreenWidth;
extern uint64_t ScreenHeight;
extern uint64_t Stride;

void GfxInit(BOOT_INFO *Info);
void PutPixel(uint32_t x, uint32_t y, uint32_t color);
void FillScreen(uint32_t color);
void FillRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void DrawChar(uint32_t x, uint32_t y, char ch, uint32_t color);
void DrawCell(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg);
void DrawString(uint32_t x, uint32_t y, const char *s, uint32_t color);
void DrawUInt64(uint32_t x, uint32_t y, uint64_t value, uint32_t color);
