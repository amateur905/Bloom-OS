#include <stdint.h>
#include "ui.h"
#include "mouse.h"

#define CURSOR_W 12
#define CURSOR_H 19

uint32_t UiW;
uint32_t UiH;
int UiMouseX;
int UiMouseY;

static int CursorShown;
static int CursorDrawX;
static int CursorDrawY;
static uint32_t CursorSaved[CURSOR_W * CURSOR_H];

static const char *const Arrow[CURSOR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
};

int
UiStrLen(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void
UiStrCopy(char *Dst, const char *Src, int Max)
{
    int i = 0;
    while (Src[i] && i < Max - 1) {
        Dst[i] = Src[i];
        i++;
    }
    Dst[i] = 0;
}

void
UiStrAppend(char *Dst, const char *Src, int Max)
{
    int n = UiStrLen(Dst);
    int i = 0;
    while (Src[i] && n + i < Max - 1) {
        Dst[n + i] = Src[i];
        i++;
    }
    Dst[n + i] = 0;
}

int
UiStrEqual(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

char
UiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int
UiCompareNames(const char *a, const char *b)
{
    while (*a && UiLower(*a) == UiLower(*b)) {
        a++;
        b++;
    }
    return (int)(uint8_t)UiLower(*a) - (int)(uint8_t)UiLower(*b);
}

void
UiUIntToText(uint64_t Value, char *Out)
{
    char Tmp[21];
    int Pos = 20;
    Tmp[Pos] = 0;

    if (Value == 0) {
        Tmp[--Pos] = '0';
    }

    while (Value > 0) {
        Tmp[--Pos] = (char)('0' + (Value % 10));
        Value /= 10;
    }

    UiStrCopy(Out, &Tmp[Pos], 24);
}

void
UiFormatSize(uint32_t Size, char *Out)
{
    char Num[24];
    Out[0] = 0;

    if (Size < 1024) {
        UiUIntToText(Size, Num);
        UiStrAppend(Out, Num, 24);
        UiStrAppend(Out, " B", 24);
        return;
    }

    if (Size < 1024 * 1024) {
        UiUIntToText(Size / 1024, Num);
        UiStrAppend(Out, Num, 24);
        UiStrAppend(Out, " KB", 24);
        return;
    }

    uint64_t Unit = Size < 1024u * 1024u * 1024u ? 1024ull * 1024ull : 1024ull * 1024ull * 1024ull;
    uint64_t Tenths = ((uint64_t)Size * 10) / Unit;

    UiUIntToText(Tenths / 10, Num);
    UiStrAppend(Out, Num, 24);
    UiStrAppend(Out, ".", 24);
    UiUIntToText(Tenths % 10, Num);
    UiStrAppend(Out, Num, 24);
    UiStrAppend(Out, Unit == 1024ull * 1024ull ? " MB" : " GB", 24);
}

void
UiFormatTime(uint32_t Seconds, char *Out)
{
    char Num[24];
    UiUIntToText(Seconds / 60, Num);
    UiStrCopy(Out, Num, 24);
    UiStrAppend(Out, ":", 24);
    if ((Seconds % 60) < 10) {
        UiStrAppend(Out, "0", 24);
    }
    UiUIntToText(Seconds % 60, Num);
    UiStrAppend(Out, Num, 24);
}

void
UiText(uint32_t X, uint32_t Y, const char *Str, uint32_t Color, int MaxChars)
{
    int n = UiStrLen(Str);
    int Clipped = n > MaxChars;
    int Draw = Clipped ? MaxChars - 1 : n;

    if (MaxChars <= 0) {
        return;
    }

    for (int i = 0; i < Draw; i++) {
        DrawChar(X + (uint32_t)i * FONT_WIDTH, Y, Str[i], Color);
    }

    if (Clipped) {
        DrawChar(X + (uint32_t)Draw * FONT_WIDTH, Y, '~', COL_DIM);
    }
}

int
UiInside(int X, int Y, uint32_t RX, uint32_t RY, uint32_t RW, uint32_t RH)
{
    return X >= (int)RX && Y >= (int)RY && X < (int)(RX + RW) && Y < (int)(RY + RH);
}

void
UiCursorHide(void)
{
    if (!CursorShown) {
        return;
    }

    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int PX = CursorDrawX + x;
            int PY = CursorDrawY + y;
            if (PX >= 0 && PY >= 0 && PX < (int)UiW && PY < (int)UiH) {
                FrameBuffer[(uint64_t)PY * Stride + PX] = CursorSaved[y * CURSOR_W + x];
            }
        }
    }

    CursorShown = 0;
}

void
UiCursorShow(void)
{
    if (CursorShown) {
        return;
    }

    CursorDrawX = UiMouseX;
    CursorDrawY = UiMouseY;

    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int PX = CursorDrawX + x;
            int PY = CursorDrawY + y;
            if (PX < 0 || PY < 0 || PX >= (int)UiW || PY >= (int)UiH) {
                continue;
            }
            volatile uint32_t *Pixel = &FrameBuffer[(uint64_t)PY * Stride + PX];
            CursorSaved[y * CURSOR_W + x] = *Pixel;
            char Ch = Arrow[y][x];
            if (Ch == 'X') {
                *Pixel = 0x00000000;
            } else if (Ch == '.') {
                *Pixel = 0x00FFFFFF;
            }
        }
    }

    CursorShown = 1;
}

void
UiInit(void)
{
    int Dx;
    int Dy;
    uint8_t Buttons;

    UiW = (uint32_t)ScreenWidth;
    UiH = (uint32_t)ScreenHeight;
    UiMouseX = (int)UiW / 2;
    UiMouseY = (int)UiH / 2;
    CursorShown = 0;

    MouseGetDelta(&Dx, &Dy, &Buttons);
    MouseTakePressed();
}

int
UiPollMouse(uint8_t *Pressed)
{
    int Dx = 0;
    int Dy = 0;
    uint8_t Buttons;
    int Moved = MouseGetDelta(&Dx, &Dy, &Buttons);
    uint8_t P = MouseTakePressed();

    *Pressed = P;

    if (!Moved && !P) {
        return 0;
    }

    UiCursorHide();

    if (Moved) {
        UiMouseX += Dx;
        UiMouseY -= Dy;

        if (UiMouseX < 0) {
            UiMouseX = 0;
        }
        if (UiMouseY < 0) {
            UiMouseY = 0;
        }
        if (UiMouseX >= (int)UiW) {
            UiMouseX = (int)UiW - 1;
        }
        if (UiMouseY >= (int)UiH) {
            UiMouseY = (int)UiH - 1;
        }
    }

    return 1;
}
