#include <stdint.h>
#include "font8x16.h"
#include "gfx.h"

volatile uint32_t *FrameBuffer;
uint64_t ScreenWidth;
uint64_t ScreenHeight;
uint64_t Stride;

void
GfxInit(BOOT_INFO *Info)
{
    FrameBuffer = (volatile uint32_t *)Info->FrameBufferBase;
    ScreenWidth = Info->Width;
    ScreenHeight = Info->Height;
    Stride = Info->PixelsPerScanLine;
}

void
PutPixel(uint32_t x, uint32_t y, uint32_t color)
{
    FrameBuffer[y * Stride + x] = color;
}

void
FillScreen(uint32_t color)
{
    for (uint64_t y = 0; y < ScreenHeight; y++) {
        volatile uint32_t *Line = FrameBuffer + y * Stride;
        for (uint64_t x = 0; x < ScreenWidth; x++) {
            Line[x] = color;
        }
    }
}

void
FillRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t row = 0; row < h; row++) {
        volatile uint32_t *Line = FrameBuffer + (uint64_t)(y + row) * Stride + x;
        for (uint32_t col = 0; col < w; col++) {
            Line[col] = color;
        }
    }
}

void
DrawChar(uint32_t x, uint32_t y, char ch, uint32_t color)
{
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR) {
        return;
    }

    const uint8_t *Glyph = Font8x16[ch - FONT_FIRST_CHAR];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t Bits = Glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (Bits & (1 << (7 - col))) {
                PutPixel(x + col, y + row, color);
            }
        }
    }
}

void
DrawCell(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *Glyph = 0;

    if (ch >= FONT_FIRST_CHAR && ch <= FONT_LAST_CHAR) {
        Glyph = Font8x16[ch - FONT_FIRST_CHAR];
    }

    for (int row = 0; row < FONT_HEIGHT; row++) {
        volatile uint32_t *Line = FrameBuffer + (uint64_t)(y + row) * Stride + x;
        uint8_t Bits = Glyph ? Glyph[row] : 0;
        for (int col = 0; col < FONT_WIDTH; col++) {
            Line[col] = (Bits & (0x80 >> col)) ? fg : bg;
        }
    }
}

void
DrawString(uint32_t x, uint32_t y, const char *s, uint32_t color)
{
    uint32_t CurX = x;
    for (int i = 0; s[i]; i++) {
        DrawChar(CurX, y, s[i], color);
        CurX += FONT_WIDTH;
    }
}

void
DrawUInt64(uint32_t x, uint32_t y, uint64_t value, uint32_t color)
{
    char Buffer[21];
    int Pos = 20;
    Buffer[Pos] = 0;

    if (value == 0) {
        Buffer[--Pos] = '0';
    } else {
        while (value > 0) {
            Buffer[--Pos] = (char)('0' + (value % 10));
            value /= 10;
        }
    }

    DrawString(x, y, &Buffer[Pos], color);
}
