#include <stdint.h>
#include "console.h"
#include "gfx.h"

#define MAX_COLS 256
#define MAX_ROWS 128
#define MARGIN 8
#define BACKGROUND 0x00201040

static const uint32_t Palette[4] = {
    0x00E0D0FF,
    0x0080FFC0,
    0x00FF7070,
    0x008878A8
};

static uint8_t Chars[MAX_ROWS][MAX_COLS];
static uint8_t Attrs[MAX_ROWS][MAX_COLS];
static uint8_t Dirty[MAX_ROWS];

static uint32_t Cols;
static uint32_t Rows;
static uint32_t CurCol;
static uint32_t CurRow;
static uint32_t PrevRow;
static uint8_t CurAttr;

static void
ClearRow(uint32_t Row)
{
    for (uint32_t c = 0; c < MAX_COLS; c++) {
        Chars[Row][c] = ' ';
        Attrs[Row][c] = CONSOLE_ATTR_NORMAL;
    }
    Dirty[Row] = 1;
}

static void
ScrollUp(void)
{
    for (uint32_t r = 1; r < Rows; r++) {
        for (uint32_t c = 0; c < Cols; c++) {
            Chars[r - 1][c] = Chars[r][c];
            Attrs[r - 1][c] = Attrs[r][c];
        }
    }

    ClearRow(Rows - 1);

    for (uint32_t r = 0; r < Rows; r++) {
        Dirty[r] = 1;
    }
}

static void
NewLine(void)
{
    CurCol = 0;

    if (CurRow + 1 < Rows) {
        CurRow++;
    } else {
        ScrollUp();
    }
}

static void
Backspace(void)
{
    if (CurCol > 0) {
        CurCol--;
    } else if (CurRow > 0) {
        Dirty[CurRow] = 1;
        CurRow--;
        CurCol = Cols - 1;
    } else {
        return;
    }

    Chars[CurRow][CurCol] = ' ';
    Attrs[CurRow][CurCol] = CONSOLE_ATTR_NORMAL;
    Dirty[CurRow] = 1;
}

static void
DrawRow(uint32_t Row)
{
    uint32_t Y = MARGIN + Row * FONT_HEIGHT;

    for (uint32_t c = 0; c < Cols; c++) {
        uint32_t Fg = Palette[Attrs[Row][c] & 3];
        uint32_t Bg = BACKGROUND;

        if (Row == CurRow && c == CurCol) {
            Bg = Palette[CONSOLE_ATTR_NORMAL];
            Fg = BACKGROUND;
        }

        DrawCell(MARGIN + c * FONT_WIDTH, Y, (char)Chars[Row][c], Fg, Bg);
    }
}

void
ConsoleClear(void)
{
    for (uint32_t r = 0; r < MAX_ROWS; r++) {
        ClearRow(r);
    }

    CurCol = 0;
    CurRow = 0;
    PrevRow = 0;

    FillScreen(BACKGROUND);
    ConsoleFlush();
}

void
ConsoleInit(void)
{
    Cols = (uint32_t)((ScreenWidth - 2 * MARGIN) / FONT_WIDTH);
    Rows = (uint32_t)((ScreenHeight - 2 * MARGIN) / FONT_HEIGHT);

    if (Cols > MAX_COLS) {
        Cols = MAX_COLS;
    }
    if (Rows > MAX_ROWS) {
        Rows = MAX_ROWS;
    }

    CurAttr = CONSOLE_ATTR_NORMAL;
    ConsoleClear();
}

void
ConsoleSetAttr(uint8_t Attr)
{
    CurAttr = Attr & 3;
}

void
ConsolePutChar(char c)
{
    if (c == '\n') {
        NewLine();
        return;
    }

    if (c == '\r') {
        CurCol = 0;
        return;
    }

    if (c == '\b') {
        Backspace();
        return;
    }

    if (c == '\t') {
        uint32_t Next = (CurCol + 4) & ~3u;
        while (CurCol < Next) {
            ConsolePutChar(' ');
        }
        return;
    }

    if (c < 32 || c > 126) {
        return;
    }

    Chars[CurRow][CurCol] = (uint8_t)c;
    Attrs[CurRow][CurCol] = CurAttr;
    Dirty[CurRow] = 1;
    CurCol++;

    if (CurCol >= Cols) {
        NewLine();
    }
}

void
ConsolePrint(const char *s)
{
    while (*s) {
        ConsolePutChar(*s++);
    }
}

void
ConsolePrintUInt(uint64_t Value)
{
    char Buffer[21];
    int Pos = 20;
    Buffer[Pos] = 0;

    if (Value == 0) {
        Buffer[--Pos] = '0';
    } else {
        while (Value > 0) {
            Buffer[--Pos] = (char)('0' + (Value % 10));
            Value /= 10;
        }
    }

    ConsolePrint(&Buffer[Pos]);
}

void
ConsolePrintHex(uint64_t Value, int MinDigits)
{
    char Buffer[17];
    int Pos = 16;
    Buffer[Pos] = 0;

    do {
        uint8_t Nibble = (uint8_t)(Value & 0xF);
        Buffer[--Pos] = (char)(Nibble < 10 ? '0' + Nibble : 'a' + Nibble - 10);
        Value >>= 4;
    } while ((Value > 0 || (16 - Pos) < MinDigits) && Pos > 0);

    ConsolePrint(&Buffer[Pos]);
}

void
ConsoleFlush(void)
{
    for (uint32_t r = 0; r < Rows; r++) {
        if (Dirty[r] || r == CurRow || r == PrevRow) {
            DrawRow(r);
            Dirty[r] = 0;
        }
    }

    PrevRow = CurRow;
}
