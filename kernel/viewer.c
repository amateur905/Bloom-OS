#include <stdint.h>
#include "viewer.h"
#include "ui.h"
#include "errors.h"
#include "fs.h"
#include "keyboard.h"
#include "player.h"
#include "idt.h"

#define VIEW_MAX_BYTES (256 * 1024)
#define VIEW_MAX_LINES 16384
#define TITLE_H        28
#define HINT_H         28
#define VROW           18
#define SCROLL_W       10
#define GUTTER_W       64
#define TEXT_X         72
#define SEARCH_MAX     48

#define VD_TITLE   0x01
#define VD_CONTENT 0x02
#define VD_HINT    0x04
#define VD_ALL     0x07

static uint8_t Buf[VIEW_MAX_BYTES];
static uint32_t Len;
static uint32_t TotalSize;
static uint32_t LineOff[VIEW_MAX_LINES + 1];
static uint32_t LineNum[VIEW_MAX_LINES];
static int LineCount;
static int LinesTruncated;

static int HexMode;
static int Top;
static int Rows;
static int Cols;
static uint32_t Flags;

static char Title[FAT_NAME_MAX];
static char Detail[160];

static char Message[200];
static int MessageKind;
static uint64_t MessageUntil;

static char SearchText[SEARCH_MAX];
static int SearchLen;
static int Searching;
static int HaveMatch;
static uint32_t MatchStart;

const char *
ViewerDetail(void)
{
    return Detail;
}

static void
Say(int Code, const char *Info)
{
    char Text[200];

    ErrorFormat(Code, Info, Text, (int)sizeof(Text));
    ErrorLog("viewer", Code, Info);
    UiStrCopy(Message, Text, (int)sizeof(Message));
    MessageKind = ErrorIsWarning(Code) ? 2 : 1;
    MessageUntil = TickCount + 6000;
    Flags |= VD_HINT;
}

static int
LoadFile(const char *Path)
{
    FS_NODE Node;
    int Status = FsResolve(Path, &Node);

    Detail[0] = 0;
    Len = 0;
    TotalSize = 0;

    if (Status != FS_OK) {
        UiStrCopy(Detail, FsErrorText(Status), (int)sizeof(Detail));
        return E_FILE_OPEN;
    }

    if (Node.IsDir) {
        UiStrCopy(Detail, "is a folder", (int)sizeof(Detail));
        return E_FILE_OPEN;
    }

    TotalSize = Node.Entry.Size;

    if (TotalSize == 0) {
        return 0;
    }

    uint32_t Want = TotalSize > VIEW_MAX_BYTES ? VIEW_MAX_BYTES : TotalSize;

    while (Len < Want) {
        uint32_t Chunk = Want - Len;

        if (Chunk > 16384) {
            Chunk = 16384;
        }

        uint32_t Got = FsRead(&Node, Len, Buf + Len, Chunk);

        if (Got == 0 || Got > Chunk) {
            break;
        }

        Len += Got;
    }

    if (Len == 0) {
        UiStrCopy(Detail, "no data could be read", (int)sizeof(Detail));
        return E_FILE_READ;
    }

    if (Len < Want) {
        char Num[24];
        UiStrCopy(Detail, "stopped at byte ", (int)sizeof(Detail));
        UiUIntToText(Len, Num);
        UiStrAppend(Detail, Num, (int)sizeof(Detail));
        return -E_FILE_READ;
    }

    return 0;
}

static int
LooksBinary(void)
{
    uint32_t Check = Len < 4096 ? Len : 4096;
    uint32_t Odd = 0;

    for (uint32_t i = 0; i < Check; i++) {
        uint8_t c = Buf[i];

        if (c == 0) {
            return 1;
        }

        if (c < 9 || (c > 13 && c < 32 && c != 27)) {
            Odd++;
        }
    }

    return Check > 0 && Odd * 20 > Check;
}

static void
CharInfo(uint32_t Pos, int Col, int *Width, char *Glyph, uint32_t *Next)
{
    uint8_t c = Buf[Pos];

    *Width = 1;

    if (c == '\t') {
        *Width = 4 - (Col % 4);
        *Glyph = ' ';
        *Next = Pos + 1;
    } else if (c >= 0xC0) {
        int Need = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
        uint32_t End = Pos + 1;

        while (End < Len && End < Pos + (uint32_t)Need && (Buf[End] & 0xC0) == 0x80) {
            End++;
        }

        *Glyph = '?';
        *Next = End;
    } else if (c >= 0x80) {
        *Glyph = '?';
        *Next = Pos + 1;
    } else if (c < 32 || c == 127) {
        *Glyph = '.';
        *Next = Pos + 1;
    } else {
        *Glyph = (char)c;
        *Next = Pos + 1;
    }
}

static void
BuildLines(void)
{
    uint32_t Pos = 0;
    uint32_t Logical = 0;
    int NewLogical = 1;

    LineCount = 0;
    LinesTruncated = 0;

    for (;;) {
        if (LineCount >= VIEW_MAX_LINES) {
            LinesTruncated = 1;
            break;
        }

        LineOff[LineCount] = Pos;
        LineNum[LineCount] = NewLogical ? ++Logical : 0;
        NewLogical = 0;
        LineCount++;

        int Col = 0;

        while (Pos < Len) {
            uint8_t c = Buf[Pos];

            if (c == '\n') {
                Pos++;
                NewLogical = 1;
                break;
            }

            if (c == '\r') {
                Pos++;
                continue;
            }

            int Width;
            char Glyph;
            uint32_t Next;

            CharInfo(Pos, Col, &Width, &Glyph, &Next);

            if (Col + Width > Cols && Col > 0) {
                break;
            }

            Col += Width;
            Pos = Next;
        }

        if (Pos >= Len) {
            break;
        }
    }

    LineOff[LineCount] = Len;
}

static int
TotalRows(void)
{
    if (HexMode) {
        return (int)((Len + 15) / 16);
    }

    return LineCount;
}

static void
ClampTop(void)
{
    int Total = TotalRows();
    int Max = Total > Rows ? Total - Rows : 0;

    if (Top > Max) {
        Top = Max;
    }
    if (Top < 0) {
        Top = 0;
    }
}

static int
LineForOffset(uint32_t Off)
{
    int Lo = 0;
    int Hi = LineCount - 1;

    while (Lo < Hi) {
        int Mid = (Lo + Hi + 1) / 2;

        if (LineOff[Mid] <= Off) {
            Lo = Mid;
        } else {
            Hi = Mid - 1;
        }
    }

    return Lo;
}

static uint32_t
TopOffset(void)
{
    if (HexMode) {
        return (uint32_t)Top * 16;
    }

    return Top < LineCount ? LineOff[Top] : 0;
}

static void
ShowOffset(uint32_t Off)
{
    if (HexMode) {
        Top = (int)(Off / 16) - 2;
    } else {
        Top = LineForOffset(Off) - 2;
    }

    ClampTop();
    Flags |= VD_CONTENT | VD_TITLE;
}

static void
ToggleHex(void)
{
    uint32_t Off = TopOffset();

    HexMode = !HexMode;

    if (HexMode) {
        Top = (int)(Off / 16);
    } else {
        Top = LineForOffset(Off);
    }

    ClampTop();
    Flags |= VD_CONTENT | VD_TITLE | VD_HINT;
}

static int
MatchAt(uint32_t Pos)
{
    for (int i = 0; i < SearchLen; i++) {
        if (UiLower((char)Buf[Pos + (uint32_t)i]) != UiLower(SearchText[i])) {
            return 0;
        }
    }

    return 1;
}

static void
FindText(uint32_t From)
{
    if (SearchLen == 0) {
        return;
    }

    if (Len < (uint32_t)SearchLen) {
        HaveMatch = 0;
        Say(E_TEXT_NOTFOUND, SearchText);
        return;
    }

    uint32_t Last = Len - (uint32_t)SearchLen;

    if (From > Last) {
        From = 0;
    }

    for (uint32_t Pass = 0; Pass < 2; Pass++) {
        uint32_t Start = Pass == 0 ? From : 0;
        uint32_t Stop = Pass == 0 ? Last : (From > 0 ? From - 1 : 0);

        if (Pass == 1 && From == 0) {
            break;
        }

        for (uint32_t p = Start; p <= Stop; p++) {
            if (MatchAt(p)) {
                HaveMatch = 1;
                MatchStart = p;
                ShowOffset(p);
                return;
            }
        }
    }

    HaveMatch = 0;
    Flags |= VD_CONTENT;
    Say(E_TEXT_NOTFOUND, SearchText);
}

static void
DrawScrollbar(void)
{
    uint32_t X = UiW - SCROLL_W;
    uint32_t Y = TITLE_H;
    uint32_t Height = UiH - TITLE_H - HINT_H;
    int Total = TotalRows();

    FillRect(X, Y, SCROLL_W, Height, COL_PANEL);

    if (Total <= Rows) {
        return;
    }

    uint32_t ThumbH = Height * (uint32_t)Rows / (uint32_t)Total;

    if (ThumbH < 20) {
        ThumbH = 20;
    }

    uint32_t ThumbY = Y + (Height - ThumbH) * (uint32_t)Top / (uint32_t)(Total - Rows);

    FillRect(X + 1, ThumbY, SCROLL_W - 2, ThumbH, COL_DIM);
}

static void
DrawTextLine(int Index, uint32_t Y)
{
    uint32_t Pos = LineOff[Index];
    uint32_t End = LineOff[Index + 1];
    int Col = 0;

    if (LineNum[Index] && Len > 0) {
        char Num[24];
        UiUIntToText(LineNum[Index], Num);
        int n = UiStrLen(Num);
        UiText(GUTTER_W - 8 - (uint32_t)n * FONT_WIDTH, Y, Num, COL_DIM, 8);
    }

    while (Pos < End) {
        uint8_t c = Buf[Pos];

        if (c == '\n' || c == '\r') {
            Pos++;
            continue;
        }

        int Width;
        char Glyph;
        uint32_t Next;

        CharInfo(Pos, Col, &Width, &Glyph, &Next);

        uint32_t X = TEXT_X + (uint32_t)Col * FONT_WIDTH;

        if (HaveMatch && Pos < MatchStart + (uint32_t)SearchLen && Next > MatchStart) {
            FillRect(X, Y, (uint32_t)Width * FONT_WIDTH, VROW - 2, COL_MATCH);
        }

        if (Glyph != ' ') {
            DrawChar(X, Y, Glyph, Glyph == '?' || Glyph == '.' ? COL_DIM : COL_TEXT);
        }

        Col += Width;
        Pos = Next;
    }
}

static void
DrawHexRow(int Index, uint32_t Y)
{
    static const char Digits[] = "0123456789ABCDEF";
    uint32_t Off = (uint32_t)Index * 16;
    char Line[12];

    for (int i = 0; i < 8; i++) {
        Line[i] = Digits[(Off >> (28 - i * 4)) & 0xF];
    }
    Line[8] = 0;
    UiText(12, Y, Line, COL_DIM, 9);

    for (uint32_t i = 0; i < 16; i++) {
        uint32_t X = 12 + 10 * FONT_WIDTH + i * 3 * FONT_WIDTH + (i >= 8 ? FONT_WIDTH : 0);
        uint32_t AX = 12 + 10 * FONT_WIDTH + 16 * 3 * FONT_WIDTH + 2 * FONT_WIDTH + i * FONT_WIDTH;

        if (Off + i >= Len) {
            break;
        }

        uint8_t b = Buf[Off + i];
        char Pair[3];
        Pair[0] = Digits[b >> 4];
        Pair[1] = Digits[b & 15];
        Pair[2] = 0;

        int Hit = HaveMatch && Off + i >= MatchStart && Off + i < MatchStart + (uint32_t)SearchLen;

        if (Hit) {
            FillRect(X, Y, 2 * FONT_WIDTH, VROW - 2, COL_MATCH);
            FillRect(AX, Y, FONT_WIDTH, VROW - 2, COL_MATCH);
        }

        UiText(X, Y, Pair, COL_TEXT, 3);
        DrawChar(AX, Y, (b >= 32 && b < 127) ? (char)b : '.', (b >= 32 && b < 127) ? COL_ACCENT : COL_DIM);
    }
}

static void
DrawContent(void)
{
    uint32_t Height = UiH - TITLE_H - HINT_H;
    uint32_t Width = UiW - SCROLL_W;

    FillRect(0, TITLE_H, Width, Height, COL_BG);

    if (!HexMode) {
        FillRect(0, TITLE_H, GUTTER_W, Height, COL_PANEL);
    }

    if (Len == 0) {
        UiText(TEXT_X, TITLE_H + 12, "(empty file)", COL_DIM, 40);
    }

    for (int r = 0; r < Rows; r++) {
        int Index = Top + r;
        uint32_t Y = TITLE_H + 6 + (uint32_t)r * VROW;

        if (Index >= TotalRows()) {
            break;
        }

        if (HexMode) {
            DrawHexRow(Index, Y);
        } else {
            DrawTextLine(Index, Y);
        }
    }

    DrawScrollbar();
}

static void
DrawTitle(void)
{
    char Info[64];
    char Num[24];
    int Total = TotalRows();
    int Span = Total > Rows ? Total - Rows : 1;
    uint32_t Percent = Total > Rows ? (uint32_t)Top * 100 / (uint32_t)Span : 100;

    FillRect(0, 0, UiW, TITLE_H, COL_HEADER);
    FillRect(8, 2, 56, 24, COL_BUTTON);
    UiText(8 + 12, 6, "Back", COL_TEXT, 6);
    UiText(76, 6, Title, COL_ACCENT, (int)((UiW - 76 - 260) / FONT_WIDTH));

    Info[0] = 0;
    UiStrAppend(Info, HexMode ? "Hex " : "Text ", 64);
    UiUIntToText(Percent, Num);
    UiStrAppend(Info, Num, 64);
    UiStrAppend(Info, "%", 64);
    UiText(UiW - 250, 6, Info, COL_DIM, 14);

    FillRect(UiW - 130, 2, 120, 24, COL_BUTTON);
    UiText(UiW - 130 + 8, 6, HexMode ? "Show as text" : "Show as hex", COL_TEXT, 14);
}

static void
DrawHint(void)
{
    uint32_t Y = UiH - HINT_H;
    int Fit = (int)((UiW - 24) / FONT_WIDTH);

    FillRect(0, Y, UiW, HINT_H, COL_PANEL);

    if (Searching) {
        char Line[SEARCH_MAX + 16];
        UiStrCopy(Line, "Find: ", (int)sizeof(Line));
        UiStrAppend(Line, SearchText, (int)sizeof(Line));
        UiStrAppend(Line, "_", (int)sizeof(Line));
        UiText(12, Y + 6, Line, COL_ACCENT, Fit);
        return;
    }

    if (Message[0]) {
        UiText(12, Y + 6, Message, MessageKind == 1 ? COL_ERROR : (MessageKind == 2 ? COL_WARN : COL_ACCENT), Fit);
        return;
    }

    UiText(12, Y + 6, "Up/Down scroll   PgUp/PgDn page   Home/End   / find   N next   H hex/text   Esc back",
           COL_DIM, Fit);
}

static void
Present(void)
{
    if (Flags) {
        UiCursorHide();

        if (Flags & VD_CONTENT) {
            DrawContent();
        }
        if (Flags & VD_TITLE) {
            DrawTitle();
        }
        if (Flags & VD_HINT) {
            DrawHint();
        }

        Flags = 0;
    }

    UiCursorShow();
}

static void
Scroll(int Delta)
{
    int Old = Top;

    Top += Delta;
    ClampTop();

    if (Top != Old) {
        Flags |= VD_CONTENT | VD_TITLE;
    }
}

static int
HandleSearchKey(int Key)
{
    if (Key == 27) {
        Searching = 0;
        Flags |= VD_HINT;
        return 0;
    }

    if (Key == '\n') {
        Searching = 0;
        Flags |= VD_HINT;
        SearchText[SearchLen] = 0;
        FindText(HaveMatch ? MatchStart : TopOffset());
        return 0;
    }

    if (Key == '\b') {
        if (SearchLen > 0) {
            SearchLen--;
            SearchText[SearchLen] = 0;
            Flags |= VD_HINT;
        }
        return 0;
    }

    if (Key >= 32 && Key <= 126 && SearchLen < SEARCH_MAX - 1) {
        SearchText[SearchLen++] = (char)Key;
        SearchText[SearchLen] = 0;
        Flags |= VD_HINT;
    }

    return 0;
}

static int
HandleKey(int Key)
{
    if (Searching) {
        return HandleSearchKey(Key);
    }

    switch (Key) {
    case 27:
    case '\b':
    case 'q':
    case 'Q':
        return 1;
    case KEY_UP:
        Scroll(-1);
        break;
    case KEY_DOWN:
        Scroll(1);
        break;
    case KEY_PGUP:
        Scroll(-(Rows - 1));
        break;
    case KEY_PGDN:
    case ' ':
        Scroll(Rows - 1);
        break;
    case KEY_HOME:
        Scroll(-TotalRows());
        break;
    case KEY_END:
        Scroll(TotalRows());
        break;
    case 'h':
    case 'H':
        ToggleHex();
        break;
    case '/':
        Searching = 1;
        SearchLen = 0;
        SearchText[0] = 0;
        Message[0] = 0;
        Flags |= VD_HINT;
        break;
    case 'n':
    case 'N':
        if (SearchLen > 0) {
            FindText(HaveMatch ? MatchStart + 1 : TopOffset());
        } else {
            Searching = 1;
            Flags |= VD_HINT;
        }
        break;
    default:
        break;
    }

    return 0;
}

static int
HandleClick(void)
{
    if (UiInside(UiMouseX, UiMouseY, 8, 2, 56, 24)) {
        return 1;
    }

    if (UiInside(UiMouseX, UiMouseY, UiW - 130, 2, 120, 24)) {
        ToggleHex();
        return 0;
    }

    if (UiInside(UiMouseX, UiMouseY, UiW - SCROLL_W, TITLE_H, SCROLL_W, UiH - TITLE_H - HINT_H)) {
        int Mid = (int)(TITLE_H + (UiH - TITLE_H - HINT_H) / 2);
        Scroll(UiMouseY < Mid ? -(Rows - 1) : Rows - 1);
    }

    return 0;
}

int
ViewerRun(const char *Path, const char *Name)
{
    int Status = LoadFile(Path);
    int Warn = 0;

    if (Status > 0) {
        ErrorLog("viewer", Status, Detail);
        return Status;
    }

    if (Status < 0) {
        Warn = -Status;
    }

    UiStrCopy(Title, Name, (int)sizeof(Title));

    Rows = (int)((UiH - TITLE_H - HINT_H - 8) / VROW);
    Cols = (int)((UiW - TEXT_X - 16 - SCROLL_W) / FONT_WIDTH);

    if (Rows < 4 || Cols < 20) {
        UiStrCopy(Detail, "viewer area is too small", (int)sizeof(Detail));
        ErrorLog("viewer", E_VIEW_RANGE, Detail);
        return E_VIEW_RANGE;
    }

    HexMode = 0;
    Top = 0;
    Searching = 0;
    SearchLen = 0;
    SearchText[0] = 0;
    HaveMatch = 0;
    Message[0] = 0;
    MessageUntil = 0;

    BuildLines();

    if (LooksBinary()) {
        HexMode = 1;
    }

    ClampTop();
    FillScreen(COL_BG);
    Flags = VD_ALL;

    {
        int Code = 0;
        const char *Info = "";

        if (Warn == E_FILE_READ) {
            Code = E_FILE_READ;
            Info = Detail;
        } else if (Len < TotalSize) {
            Code = E_FILE_TRUNCATED;
        } else if (HexMode) {
            Code = E_FILE_BINARY;
        } else if (LinesTruncated) {
            Code = E_VIEW_LINES;
        } else if (TotalSize == 0) {
            Code = E_FILE_EMPTY;
        }

        if (Code) {
            Say(Code, Info);
        }
    }

    while (KeyboardGetKey() != 0) {
    }

    for (;;) {
        int Key = KeyboardGetKey();

        if (Key != 0 && HandleKey(Key)) {
            break;
        }

        uint8_t Pressed;

        if (UiPollMouse(&Pressed) && (Pressed & 1)) {
            if (HandleClick()) {
                break;
            }
        }

        PlayerPoll();

        if (Message[0] && TickCount >= MessageUntil) {
            Message[0] = 0;
            Flags |= VD_HINT;
        }

        Present();

        if (Key == 0) {
            __asm__ __volatile__("hlt");
        }
    }

    UiCursorHide();
    return 0;
}
