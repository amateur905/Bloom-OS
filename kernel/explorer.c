#include <stdint.h>
#include "explorer.h"
#include "ui.h"
#include "errors.h"
#include "viewer.h"
#include "wav.h"
#include "console.h"
#include "keyboard.h"
#include "fs.h"
#include "player.h"
#include "idt.h"
#include "klog.h"

#define MAX_ENTRIES   1024
#define NAME_STORED   120
#define ROW_H         22
#define TITLE_H       28
#define PATH_H        32
#define HEAD_H        22
#define NOW_H         28
#define HINT_H        28
#define SIDE_W        200
#define SIDE_HEAD_H   26
#define SIDE_ROW_H    28
#define SCROLL_W      10
#define FULL_MAX      (FS_PATH_MAX + FAT_NAME_MAX + 40)

#define KIND_DIR  0
#define KIND_WAV  1
#define KIND_TEXT 2
#define KIND_FILE 3

#define SORT_NAME 0
#define SORT_SIZE 1
#define SORT_TYPE 2

#define MSG_INFO  0
#define MSG_ERROR 1
#define MSG_WARN  2

#define DF_PATH   0x01
#define DF_SIDE   0x02
#define DF_LIST   0x04
#define DF_NOW    0x08
#define DF_HINT   0x10
#define DF_ALL    0x1F

#define CTRL_A 0x01
#define CTRL_D 0x04
#define CTRL_P 0x10
#define CTRL_R 0x12
#define CTRL_S 0x13

typedef struct {
    char Name[NAME_STORED];
    uint32_t Size;
    uint8_t Kind;
    uint8_t Usable;
    uint8_t Hidden;
} ENTRY;

static ENTRY Entries[MAX_ENTRIES];
static uint16_t Order[MAX_ENTRIES];
static int EntryCount;
static int Truncated;

static int Volume;
static char Dir[FS_PATH_MAX];
static FS_NODE DirNode;

static int SortMode;
static int ShowHidden;
static int Autoplay;

static int Selected;
static int Scroll;
static int RowDirtyA;
static int RowDirtyB;
static uint32_t Flags;

static char Message[220];
static int MessageKind;
static uint64_t MessageUntil;

static char PlayingFull[FULL_MAX];
static char PlayingDir[FS_PATH_MAX];
static char PlayingName[FAT_NAME_MAX];
static int PlayingVolume;
static int PlayingShown;
static uint32_t ShownSecond;

static int Visible;
static uint32_t ListY;
static uint32_t ListH;
static uint32_t SideTop;

static uint64_t LastClickTick;
static int LastClickItem;

static const char *const TextExtensions[] = {
    "txt", "md", "log", "ini", "cfg", "conf", "c", "h", "cpp", "hpp", "asm", "s", "py", "js",
    "json", "xml", "csv", "html", "htm", "css", "sh", "bat", "cmd", "yml", "yaml", "toml",
    "rs", "go", "java", "lua", "tex", "ld", "mk", "makefile", "license", "readme",
};

static const char *
ExtensionOf(const char *Name)
{
    const char *Dot = 0;

    for (const char *p = Name; *p; p++) {
        if (*p == '.') {
            Dot = p + 1;
        }
    }

    return Dot ? Dot : "";
}

static int
ExtIs(const char *Name, const char *Ext)
{
    return UiCompareNames(ExtensionOf(Name), Ext) == 0;
}

static int
ClassifyFile(const char *Name)
{
    if (ExtIs(Name, "wav")) {
        return KIND_WAV;
    }

    for (unsigned i = 0; i < sizeof(TextExtensions) / sizeof(TextExtensions[0]); i++) {
        if (ExtIs(Name, TextExtensions[i]) || UiCompareNames(Name, TextExtensions[i]) == 0) {
            return KIND_TEXT;
        }
    }

    return KIND_FILE;
}

static int
AtRoot(void)
{
    return Dir[0] == '/' && Dir[1] == 0;
}

static int
ItemCount(void)
{
    return EntryCount + (AtRoot() ? 0 : 1);
}

static const ENTRY *
ItemEntry(int Index)
{
    int Shift = AtRoot() ? 0 : 1;

    if (Index < Shift || Index - Shift >= EntryCount) {
        return 0;
    }

    return &Entries[Order[Index - Shift]];
}

static void
SetMessage(const char *Text, int Kind)
{
    UiStrCopy(Message, Text, (int)sizeof(Message));
    MessageKind = Kind;
    MessageUntil = TickCount + (Kind == MSG_INFO ? 5000 : 7000);
    Flags |= DF_HINT;
}

static void
SetError(int Code, const char *Detail)
{
    char Text[220];

    ErrorFormat(Code, Detail, Text, (int)sizeof(Text));
    ErrorLog("explorer", Code, Detail);
    SetMessage(Text, ErrorIsWarning(Code) ? MSG_WARN : MSG_ERROR);
}

static void
BuildFull(char *Out, const char *Name)
{
    UiStrCopy(Out, FsVolumeName(Volume), FULL_MAX);
    UiStrAppend(Out, ":", FULL_MAX);
    UiStrAppend(Out, Dir, FULL_MAX);

    if (!AtRoot()) {
        UiStrAppend(Out, "/", FULL_MAX);
    }

    UiStrAppend(Out, Name, FULL_MAX);
}

static void
DirJoin(const char *Name)
{
    if (!AtRoot()) {
        UiStrAppend(Dir, "/", FS_PATH_MAX);
    }
    UiStrAppend(Dir, Name, FS_PATH_MAX);
}

static void
DirUp(char *Last, int Max)
{
    int n = UiStrLen(Dir);
    int Cut = n;

    while (Cut > 0 && Dir[Cut - 1] != '/') {
        Cut--;
    }

    UiStrCopy(Last, &Dir[Cut], Max);

    if (Cut <= 1) {
        Dir[0] = '/';
        Dir[1] = 0;
    } else {
        Dir[Cut - 1] = 0;
    }
}

static int
ListCallback(const FAT_ENTRY *E, void *Context)
{
    (void)Context;

    int Hidden = (E->Attr & (FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM)) != 0;

    if (Hidden && !ShowHidden) {
        return 0;
    }

    if (EntryCount >= MAX_ENTRIES) {
        Truncated = 1;
        return 1;
    }

    ENTRY *Dst = &Entries[EntryCount];

    Dst->Usable = UiStrLen(E->Name) < NAME_STORED;
    UiStrCopy(Dst->Name, E->Name, NAME_STORED);
    Dst->Size = E->Size;
    Dst->Hidden = (uint8_t)Hidden;
    Dst->Kind = (E->Attr & FAT_ATTR_DIRECTORY) ? KIND_DIR : (uint8_t)ClassifyFile(E->Name);

    EntryCount++;
    return 0;
}

static int
EntryLess(int a, int b)
{
    const ENTRY *A = &Entries[a];
    const ENTRY *B = &Entries[b];
    int AD = A->Kind == KIND_DIR;
    int BD = B->Kind == KIND_DIR;

    if (AD != BD) {
        return AD;
    }

    if (!AD && SortMode == SORT_SIZE && A->Size != B->Size) {
        return A->Size > B->Size;
    }

    if (!AD && SortMode == SORT_TYPE) {
        int Cmp = UiCompareNames(ExtensionOf(A->Name), ExtensionOf(B->Name));

        if (Cmp != 0) {
            return Cmp < 0;
        }
    }

    return UiCompareNames(A->Name, B->Name) < 0;
}

static void
SortEntries(void)
{
    for (int i = 0; i < EntryCount; i++) {
        Order[i] = (uint16_t)i;
    }

    for (int Gap = EntryCount / 2; Gap > 0; Gap /= 2) {
        for (int i = Gap; i < EntryCount; i++) {
            uint16_t Tmp = Order[i];
            int j = i;
            while (j >= Gap && EntryLess(Tmp, Order[j - Gap])) {
                Order[j] = Order[j - Gap];
                j -= Gap;
            }
            Order[j] = Tmp;
        }
    }
}

static void
ClampScroll(void)
{
    int Count = ItemCount();
    int MaxScroll = Count > Visible ? Count - Visible : 0;

    if (Selected < 0 || (Count > 0 && Selected >= Count)) {
        SetError(E_INTERNAL_SEL, "selection index out of range");
        Selected = Count > 0 ? Count - 1 : 0;
        if (Selected < 0) {
            Selected = 0;
        }
    }

    if (Scroll > MaxScroll) {
        Scroll = MaxScroll;
    }
    if (Scroll < 0) {
        Scroll = 0;
    }
    if (Selected < Scroll) {
        Scroll = Selected;
    }
    if (Selected >= Scroll + Visible) {
        Scroll = Selected - Visible + 1;
    }
}

static int
FindItemByName(const char *Name)
{
    for (int i = 0; i < ItemCount(); i++) {
        const ENTRY *E = ItemEntry(i);
        if (E && UiStrEqual(E->Name, Name)) {
            return i;
        }
    }

    return -1;
}

static int
LoadDir(const char *Focus)
{
    char Input[FULL_MAX];

    EntryCount = 0;
    Truncated = 0;
    Selected = 0;
    Scroll = 0;
    Flags |= DF_ALL;
    RowDirtyA = -1;
    RowDirtyB = -1;

    if (FsVolumeCount() == 0) {
        SetError(E_NO_VOLUMES, "");
        return 0;
    }

    UiStrCopy(Input, FsVolumeName(Volume), FULL_MAX);
    UiStrAppend(Input, ":", FULL_MAX);
    UiStrAppend(Input, Dir, FULL_MAX);

    int Status = FsResolve(Input, &DirNode);

    if (Status == FS_OK && !DirNode.IsDir) {
        Status = FS_ERR_NOTDIR;
    }

    if (Status == FS_OK) {
        Status = FsList(&DirNode, ListCallback, 0);
    }

    SortEntries();

    if (Focus && Focus[0]) {
        int Found = FindItemByName(Focus);
        if (Found >= 0) {
            Selected = Found;
        }
    } else if (!AtRoot() && ItemCount() > 1) {
        Selected = 1;
    }

    ClampScroll();

    if (Status != FS_OK) {
        SetError(E_FOLDER_READ, FsErrorText(Status));
        return 0;
    }

    if (Truncated) {
        SetError(E_FOLDER_LARGE, "");
    }

    return 1;
}

static void
Select(int Index)
{
    int Count = ItemCount();

    if (Count == 0) {
        return;
    }

    if (Index < 0) {
        Index = 0;
    }
    if (Index >= Count) {
        Index = Count - 1;
    }

    if (Index == Selected) {
        return;
    }

    int OldScroll = Scroll;
    int Old = Selected;

    Selected = Index;
    ClampScroll();

    if (Scroll != OldScroll) {
        Flags |= DF_LIST;
    } else {
        RowDirtyA = Old;
        RowDirtyB = Selected;
    }
}

static void
SwitchVolume(int Index)
{
    if (Index < 0 || Index >= FsVolumeCount()) {
        return;
    }

    Volume = Index;
    Dir[0] = '/';
    Dir[1] = 0;
    LoadDir(0);
}

static void
GoUp(void)
{
    char Last[FAT_NAME_MAX];

    if (AtRoot()) {
        return;
    }

    DirUp(Last, FAT_NAME_MAX);
    LoadDir(Last);
}

static int
PlayerCodeToError(int PlayerCode)
{
    switch (PlayerCode) {
    case PLAYER_ERR_FS:
    case PLAYER_ERR_DIR:
        return E_FILE_OPEN;
    case PLAYER_ERR_NOTWAV:
        return E_AUDIO_NOTWAV;
    case PLAYER_ERR_RATE:
        return E_AUDIO_RATE;
    case PLAYER_ERR_MEMORY:
        return E_AUDIO_MEMORY;
    case PLAYER_ERR_DEVICE:
        return E_AUDIO_DEVICE;
    case PLAYER_ERR_READ:
        return E_AUDIO_READ;
    default:
        return E_AUDIO_FORMAT;
    }
}

static int
StartPlay(const ENTRY *E)
{
    char Full[FULL_MAX];
    const char *Error = "";

    if (!E->Usable) {
        SetError(E_NAME_LONG, E->Name);
        return 0;
    }

    BuildFull(Full, E->Name);

    if (!PlayerStart(Full, &Error)) {
        SetError(PlayerCodeToError(PlayerLastError()), E->Name);
        return 0;
    }

    UiStrCopy(PlayingFull, Full, FULL_MAX);
    UiStrCopy(PlayingDir, Dir, FS_PATH_MAX);
    UiStrCopy(PlayingName, E->Name, FAT_NAME_MAX);
    PlayingVolume = Volume;
    PlayingShown = 1;
    Flags |= DF_LIST | DF_NOW;
    return 1;
}

static void
OpenViewer(const ENTRY *E)
{
    char Full[FULL_MAX];

    if (!E->Usable) {
        SetError(E_NAME_LONG, E->Name);
        return;
    }

    BuildFull(Full, E->Name);

    int Code = ViewerRun(Full, E->Name);

    FillScreen(COL_BG);
    Flags |= DF_ALL;

    if (Code != 0) {
        SetError(Code, ViewerDetail()[0] ? ViewerDetail() : E->Name);
    }
}

static void
Activate(int Index)
{
    const ENTRY *E = ItemEntry(Index);

    if (!E) {
        GoUp();
        return;
    }

    if (E->Kind == KIND_DIR) {
        if (!E->Usable) {
            SetError(E_NAME_LONG, E->Name);
            return;
        }

        char Saved[FS_PATH_MAX];
        UiStrCopy(Saved, Dir, FS_PATH_MAX);
        DirJoin(E->Name);

        if (UiStrLen(Dir) >= FS_PATH_MAX - 2) {
            UiStrCopy(Dir, Saved, FS_PATH_MAX);
            SetError(E_PATH_LONG, E->Name);
            return;
        }

        if (!LoadDir(0)) {
            UiStrCopy(Dir, Saved, FS_PATH_MAX);
            LoadDir(0);
        }
        return;
    }

    if (E->Kind == KIND_WAV) {
        StartPlay(E);
        return;
    }

    OpenViewer(E);
}

static void
ShowInfo(void)
{
    const ENTRY *E = ItemEntry(Selected);
    char Text[220];
    char Num[24];

    if (!E) {
        SetMessage("Parent folder", MSG_INFO);
        return;
    }

    UiStrCopy(Text, E->Name, (int)sizeof(Text));

    if (E->Kind == KIND_DIR) {
        UiStrAppend(Text, ": folder", (int)sizeof(Text));
        SetMessage(Text, MSG_INFO);
        return;
    }

    UiStrAppend(Text, ": ", (int)sizeof(Text));
    UiUIntToText(E->Size, Num);
    UiStrAppend(Text, Num, (int)sizeof(Text));
    UiStrAppend(Text, " bytes", (int)sizeof(Text));

    if (E->Kind == KIND_WAV) {
        char Full[FULL_MAX];
        FS_NODE Node;
        WAV_INFO Info;

        BuildFull(Full, E->Name);

        if (FsResolve(Full, &Node) != FS_OK) {
            SetError(E_INFO_FAILED, E->Name);
            return;
        }

        int Status = WavParse(&Node, &Info);

        if (Status != 1) {
            SetError(Status == WAV_ERR_NOTWAV ? E_AUDIO_NOTWAV : E_AUDIO_FORMAT, E->Name);
            return;
        }

        UiStrAppend(Text, ", WAV ", (int)sizeof(Text));
        UiUIntToText(Info.SampleRate, Num);
        UiStrAppend(Text, Num, (int)sizeof(Text));
        UiStrAppend(Text, " Hz ", (int)sizeof(Text));
        UiUIntToText(Info.BitsPerSample, Num);
        UiStrAppend(Text, Num, (int)sizeof(Text));
        UiStrAppend(Text, "-bit ", (int)sizeof(Text));
        UiUIntToText(Info.Channels, Num);
        UiStrAppend(Text, Num, (int)sizeof(Text));
        UiStrAppend(Text, Info.Channels == 1 ? " channel, " : " channels, ", (int)sizeof(Text));
        UiFormatTime(Info.DataBytes / Info.BlockAlign / Info.SampleRate, Num);
        UiStrAppend(Text, Num, (int)sizeof(Text));
    } else {
        UiStrAppend(Text, E->Kind == KIND_TEXT ? ", text file" : ", file", (int)sizeof(Text));
    }

    SetMessage(Text, MSG_INFO);
}

static void
JumpToLetter(char Ch)
{
    int Count = ItemCount();
    char Wanted = UiLower(Ch);

    for (int Step = 1; Step <= Count; Step++) {
        int Index = (Selected + Step) % Count;
        const ENTRY *E = ItemEntry(Index);

        if (E && UiLower(E->Name[0]) == Wanted) {
            Select(Index);
            return;
        }
    }
}

static void
Refresh(void)
{
    char Focus[FAT_NAME_MAX];
    const ENTRY *E = ItemEntry(Selected);

    Focus[0] = 0;

    if (E) {
        UiStrCopy(Focus, E->Name, FAT_NAME_MAX);
    }

    LoadDir(Focus);
}

static void
CycleSort(void)
{
    char Focus[FAT_NAME_MAX];
    const ENTRY *E = ItemEntry(Selected);

    Focus[0] = 0;

    if (E) {
        UiStrCopy(Focus, E->Name, FAT_NAME_MAX);
    }

    SortMode = (SortMode + 1) % 3;
    SortEntries();

    int Found = Focus[0] ? FindItemByName(Focus) : -1;
    Selected = Found >= 0 ? Found : 0;
    Scroll = 0;
    ClampScroll();
    Flags |= DF_PATH | DF_LIST;
}

static void
AdvanceTrack(void)
{
    if (PlayingVolume != Volume || !UiStrEqual(PlayingDir, Dir)) {
        return;
    }

    int Current = FindItemByName(PlayingName);

    if (Current < 0) {
        return;
    }

    for (int i = Current + 1; i < ItemCount(); i++) {
        const ENTRY *E = ItemEntry(i);

        if (E && E->Kind == KIND_WAV) {
            Select(i);
            StartPlay(E);
            return;
        }
    }

    SetMessage("Autoplay: end of folder", MSG_INFO);
}

static int
Layout(void)
{
    SideTop = TITLE_H + PATH_H;
    ListY = SideTop + HEAD_H;
    ListH = UiH - NOW_H - HINT_H - ListY;
    Visible = (int)(ListH / ROW_H);
    return Visible >= 3;
}

static void
DrawIcon(int Kind, uint32_t X, uint32_t Y)
{
    Y += 3;

    if (Kind == KIND_DIR) {
        FillRect(X, Y + 1, 7, 3, COL_FOLDER);
        FillRect(X, Y + 3, 16, 11, COL_FOLDER);
        FillRect(X, Y + 5, 16, 1, COL_FOLDER_DK);
    } else if (Kind == KIND_WAV) {
        FillRect(X + 1, Y + 5, 4, 6, COL_ACCENT);
        FillRect(X + 5, Y + 3, 2, 10, COL_ACCENT);
        FillRect(X + 7, Y + 1, 2, 14, COL_ACCENT);
        FillRect(X + 11, Y + 4, 1, 8, COL_ACCENT);
        FillRect(X + 14, Y + 2, 1, 12, COL_ACCENT);
    } else {
        uint32_t Color = Kind == KIND_TEXT ? COL_TEXT : COL_FILE;
        FillRect(X + 2, Y + 1, 12, 14, Color);
        FillRect(X + 4, Y + 5, 8, 1, COL_BG);
        FillRect(X + 4, Y + 8, 8, 1, COL_BG);
        FillRect(X + 4, Y + 11, 6, 1, COL_BG);
    }
}

static int
IsPlayingEntry(const ENTRY *E)
{
    char Full[FULL_MAX];

    if (!PlayingShown || !PlayerActive() || !E || E->Kind != KIND_WAV) {
        return 0;
    }

    BuildFull(Full, E->Name);
    return UiStrEqual(Full, PlayingFull);
}

static void
DrawRow(int Index)
{
    if (Index < Scroll || Index >= Scroll + Visible || Index >= ItemCount()) {
        return;
    }

    uint32_t Y = ListY + (uint32_t)(Index - Scroll) * ROW_H;
    uint32_t Width = UiW - SIDE_W - SCROLL_W;
    uint32_t Bg = (Index & 1) ? COL_BG_ALT : COL_BG;
    const ENTRY *E = ItemEntry(Index);

    if (Index == Selected) {
        Bg = COL_SELECT;
    }

    FillRect(SIDE_W, Y, Width, ROW_H, Bg);

    int NameChars = (int)((UiW - SIDE_W - SCROLL_W - 40 - 230) / FONT_WIDTH);
    uint32_t NameX = SIDE_W + 40;
    uint32_t SizeX = UiW - SCROLL_W - 220;
    uint32_t TypeX = UiW - SCROLL_W - 110;

    if (!E) {
        FillRect(SIDE_W + 12, Y + 7, 16, 8, COL_FOLDER_DK);
        UiText(NameX, Y + 3, "..", COL_TEXT, NameChars);
        UiText(TypeX, Y + 3, "Up", COL_DIM, 12);
        return;
    }

    DrawIcon(E->Kind, SIDE_W + 12, Y);

    int Playing = IsPlayingEntry(E);

    if (Playing) {
        DrawChar(SIDE_W + 2, Y + 3, '>', COL_ACCENT);
    }

    uint32_t NameColor = Playing ? COL_ACCENT : (E->Hidden ? COL_DIM : COL_TEXT);

    UiText(NameX, Y + 3, E->Name, NameColor, NameChars);

    if (E->Kind != KIND_DIR) {
        char Size[24];
        UiFormatSize(E->Size, Size);
        uint32_t Len = (uint32_t)UiStrLen(Size);
        UiText(SizeX + (88 - Len * FONT_WIDTH), Y + 3, Size, COL_DIM, 11);
    }

    const char *Type = E->Kind == KIND_DIR ? "Folder" :
                       (E->Kind == KIND_WAV ? "WAV audio" : (E->Kind == KIND_TEXT ? "Text" : "File"));
    UiText(TypeX, Y + 3, Type, E->Kind == KIND_WAV ? COL_ACCENT : COL_DIM, 12);
}

static void
DrawScrollbar(void)
{
    uint32_t X = UiW - SCROLL_W;
    FillRect(X, ListY, SCROLL_W, ListH, COL_PANEL);

    int Count = ItemCount();

    if (Count <= Visible) {
        return;
    }

    uint32_t ThumbH = ListH * (uint32_t)Visible / (uint32_t)Count;

    if (ThumbH < 20) {
        ThumbH = 20;
    }

    uint32_t Range = ListH - ThumbH;
    uint32_t ThumbY = ListY + Range * (uint32_t)Scroll / (uint32_t)(Count - Visible);

    FillRect(X + 1, ThumbY, SCROLL_W - 2, ThumbH, COL_DIM);
}

static void
DrawList(void)
{
    uint32_t Width = UiW - SIDE_W - SCROLL_W;

    FillRect(SIDE_W, ListY, Width, ListH, COL_BG);

    if (FsVolumeCount() == 0) {
        UiText(SIDE_W + 24, ListY + 24, "No readable volumes found", COL_ERROR, 60);
    } else if (ItemCount() == 0) {
        UiText(SIDE_W + 24, ListY + 24, "This folder is empty", COL_DIM, 60);
    }

    for (int i = Scroll; i < Scroll + Visible && i < ItemCount(); i++) {
        DrawRow(i);
    }

    DrawScrollbar();
}

static void
DrawTitle(void)
{
    FillRect(0, 0, UiW, TITLE_H, COL_HEADER);
    UiText(12, 6, "Bloom Explorer", COL_ACCENT, 30);
    UiText(UiW - 8 * 16 - 12, 6, "Esc - exit", COL_DIM, 16);
}

static void
DrawPath(void)
{
    char Full[FULL_MAX];
    char Chips[64];

    FillRect(0, TITLE_H, UiW, PATH_H, COL_PANEL);
    FillRect(8, TITLE_H + 4, 56, 24, COL_BUTTON);
    UiText(8 + 16, TITLE_H + 8, "Up", AtRoot() ? COL_DIM : COL_TEXT, 4);

    Chips[0] = 0;
    UiStrAppend(Chips, "Sort: ", 64);
    UiStrAppend(Chips, SortMode == SORT_NAME ? "name" : (SortMode == SORT_SIZE ? "size" : "type"), 64);
    UiStrAppend(Chips, "   Hidden: ", 64);
    UiStrAppend(Chips, ShowHidden ? "on" : "off", 64);
    UiText(UiW - 8 * 28 - 12, TITLE_H + 8, Chips, COL_DIM, 28);

    UiStrCopy(Full, FsVolumeCount() ? FsVolumeName(Volume) : "-", FULL_MAX);
    UiStrAppend(Full, ":", FULL_MAX);
    UiStrAppend(Full, Dir, FULL_MAX);

    int Fit = (int)((UiW - 84 - 8 * 30) / FONT_WIDTH);
    int Len = UiStrLen(Full);

    if (Len > Fit) {
        UiText(76, TITLE_H + 8, "...", COL_DIM, 3);
        UiText(76 + 3 * FONT_WIDTH, TITLE_H + 8, &Full[Len - (Fit - 3)], COL_TEXT, Fit - 3);
    } else {
        UiText(76, TITLE_H + 8, Full, COL_TEXT, Fit);
    }
}

static void
DrawSidebar(void)
{
    uint32_t Height = UiH - NOW_H - HINT_H - SideTop;

    FillRect(0, SideTop, SIDE_W, Height, COL_PANEL);
    UiText(12, SideTop + 5, "VOLUMES", COL_DIM, 20);

    for (int i = 0; i < FsVolumeCount(); i++) {
        uint32_t Y = SideTop + SIDE_HEAD_H + (uint32_t)i * SIDE_ROW_H;

        if (Y + SIDE_ROW_H > SideTop + Height) {
            break;
        }

        if (i == Volume) {
            FillRect(0, Y, SIDE_W, SIDE_ROW_H, COL_SELECT);
            FillRect(0, Y, 4, SIDE_ROW_H, COL_ACCENT);
        }

        FillRect(14, Y + 7, 14, 10, i == Volume ? COL_ACCENT : COL_FILE);
        UiText(38, Y + 6, FsVolumeName(i), COL_TEXT, 19);
    }
}

static void
DrawHeader(void)
{
    FillRect(SIDE_W, SideTop, UiW - SIDE_W, HEAD_H, COL_HEADER);
    UiText(SIDE_W + 40, SideTop + 3, "Name", COL_DIM, 10);
    UiText(UiW - SCROLL_W - 220 + 88 - 4 * FONT_WIDTH, SideTop + 3, "Size", COL_DIM, 10);
    UiText(UiW - SCROLL_W - 110, SideTop + 3, "Type", COL_DIM, 10);
}

static void
DrawNow(void)
{
    uint32_t Y = UiH - NOW_H - HINT_H;

    FillRect(0, Y, UiW, NOW_H, COL_HEADER);

    if (!PlayerActive()) {
        UiText(12, Y + 6, "Nothing playing", COL_DIM, 40);
        UiText(UiW - 100 - 8 * 14, Y + 6, Autoplay ? "Autoplay: on" : "Autoplay: off", COL_DIM, 14);
        return;
    }

    const PLAYER_INFO *P = PlayerGetInfo();
    char Elapsed[24];
    char Total[24];
    char Line[64];
    uint32_t Seconds = PlayerElapsedSeconds();

    UiFormatTime(Seconds, Elapsed);
    UiFormatTime(P->TotalSeconds, Total);
    UiStrCopy(Line, Elapsed, 64);
    UiStrAppend(Line, " / ", 64);
    UiStrAppend(Line, Total, 64);

    UiText(12, Y + 6, PlayingName, COL_ACCENT, 28);
    UiText(12 + 30 * FONT_WIDTH, Y + 6, Line, COL_TEXT, 16);

    uint32_t BarX = 12 + 48 * FONT_WIDTH;
    uint32_t BarW = UiW - BarX - 120 - 8 * 14;

    FillRect(BarX, Y + 10, BarW, 8, COL_TRACK);

    if (P->TotalSeconds > 0) {
        FillRect(BarX, Y + 10, BarW * Seconds / P->TotalSeconds, 8, COL_ACCENT);
    }

    UiText(UiW - 100 - 8 * 14, Y + 6, Autoplay ? "Autoplay: on" : "Autoplay: off", COL_DIM, 14);
    FillRect(UiW - 100, Y + 3, 88, 22, COL_BUTTON);
    UiText(UiW - 100 + 20, Y + 6, "Stop", COL_TEXT, 6);
}

static void
DrawHint(void)
{
    uint32_t Y = UiH - HINT_H;
    int Fit = (int)((UiW - 24) / FONT_WIDTH);

    FillRect(0, Y, UiW, HINT_H, COL_PANEL);

    if (Message[0]) {
        UiText(12, Y + 6, Message,
               MessageKind == MSG_ERROR ? COL_ERROR : (MessageKind == MSG_WARN ? COL_WARN : COL_ACCENT), Fit);
        return;
    }

    UiText(12, Y + 6,
           "Enter open  Bksp up  Tab volume  Space stop  ^S sort  ^A hidden  ^R refresh  ^D info  ^P autoplay  Esc exit",
           COL_DIM, Fit);
}

static void
Present(void)
{
    if (Flags || RowDirtyA >= 0 || RowDirtyB >= 0) {
        UiCursorHide();

        if (Flags & DF_PATH) {
            DrawTitle();
            DrawPath();
        }
        if (Flags & DF_SIDE) {
            DrawSidebar();
            DrawHeader();
        }
        if (Flags & DF_LIST) {
            DrawList();
        } else {
            if (RowDirtyA >= 0) {
                DrawRow(RowDirtyA);
            }
            if (RowDirtyB >= 0) {
                DrawRow(RowDirtyB);
            }
        }
        if (Flags & DF_NOW) {
            DrawNow();
        }
        if (Flags & DF_HINT) {
            DrawHint();
        }

        Flags = 0;
        RowDirtyA = -1;
        RowDirtyB = -1;
    }

    UiCursorShow();
}

static int
HandleKey(int Key)
{
    switch (Key) {
    case 27:
        return 1;
    case KEY_UP:
        Select(Selected - 1);
        break;
    case KEY_DOWN:
        Select(Selected + 1);
        break;
    case KEY_PGUP:
        Select(Selected - Visible);
        break;
    case KEY_PGDN:
        Select(Selected + Visible);
        break;
    case KEY_HOME:
        Select(0);
        break;
    case KEY_END:
        Select(ItemCount() - 1);
        break;
    case '\n':
        Activate(Selected);
        break;
    case KEY_RIGHT:
        if (ItemEntry(Selected) && ItemEntry(Selected)->Kind == KIND_DIR) {
            Activate(Selected);
        }
        break;
    case '\b':
    case KEY_LEFT:
        GoUp();
        break;
    case '\t':
        if (FsVolumeCount() > 1) {
            SwitchVolume((Volume + 1) % FsVolumeCount());
        }
        break;
    case ' ':
        if (PlayerActive()) {
            PlayerStop();
            Flags |= DF_NOW | DF_LIST;
        }
        break;
    case CTRL_S:
        CycleSort();
        break;
    case CTRL_A:
        ShowHidden = !ShowHidden;
        Flags |= DF_PATH;
        Refresh();
        break;
    case CTRL_R:
        Refresh();
        SetMessage("Refreshed", MSG_INFO);
        break;
    case CTRL_D:
        ShowInfo();
        break;
    case CTRL_P:
        Autoplay = !Autoplay;
        Flags |= DF_NOW;
        break;
    default:
        if (Key > 32 && Key <= 126) {
            JumpToLetter((char)Key);
        }
        break;
    }

    return 0;
}

static void
HandleClick(void)
{
    uint64_t Now = TickCount;

    if (UiInside(UiMouseX, UiMouseY, SIDE_W, ListY, UiW - SIDE_W - SCROLL_W, (uint32_t)Visible * ROW_H)) {
        int Index = Scroll + (UiMouseY - (int)ListY) / ROW_H;

        if (Index < ItemCount()) {
            if (Index == Selected && LastClickItem == Index && Now - LastClickTick < 500) {
                LastClickItem = -1;
                Activate(Index);
            } else {
                Select(Index);
                LastClickItem = Index;
                LastClickTick = Now;
            }
        }
        return;
    }

    if (UiInside(UiMouseX, UiMouseY, UiW - SCROLL_W, ListY, SCROLL_W, ListH)) {
        int Count = ItemCount();

        if (Count > Visible) {
            if (UiMouseY < (int)(ListY + ListH / 2)) {
                Scroll -= Visible;
                Selected -= Visible;
            } else {
                Scroll += Visible;
                Selected += Visible;
            }
            if (Selected < 0) {
                Selected = 0;
            }
            if (Selected >= Count) {
                Selected = Count - 1;
            }
            ClampScroll();
            Flags |= DF_LIST;
        }
        return;
    }

    if (UiInside(UiMouseX, UiMouseY, 0, SideTop + SIDE_HEAD_H, SIDE_W, UiH - NOW_H - HINT_H - SideTop - SIDE_HEAD_H)) {
        SwitchVolume((UiMouseY - (int)(SideTop + SIDE_HEAD_H)) / SIDE_ROW_H);
        return;
    }

    if (UiInside(UiMouseX, UiMouseY, 8, TITLE_H + 4, 56, 24)) {
        GoUp();
        return;
    }

    if (PlayerActive() && UiInside(UiMouseX, UiMouseY, UiW - 100, UiH - NOW_H - HINT_H + 3, 88, 22)) {
        PlayerStop();
        Flags |= DF_NOW | DF_LIST;
        return;
    }

    if (UiInside(UiMouseX, UiMouseY, UiW - 100 - 8 * 14, UiH - NOW_H - HINT_H, 8 * 14, NOW_H)) {
        Autoplay = !Autoplay;
        Flags |= DF_NOW;
    }
}

void
ExplorerRun(void)
{
    UiInit();

    if (UiW < 800 || UiH < 480 || !Layout()) {
        char Text[160];
        ErrorFormat(E_SCREEN_SMALL, "", Text, (int)sizeof(Text));
        ErrorLog("explorer", E_SCREEN_SMALL, "");
        ConsoleSetAttr(CONSOLE_ATTR_ERROR);
        ConsolePrint(Text);
        ConsolePutChar('\n');
        ConsoleSetAttr(CONSOLE_ATTR_NORMAL);
        return;
    }

    Volume = 0;
    Dir[0] = '/';
    Dir[1] = 0;
    Message[0] = 0;
    PlayingShown = 0;
    Selected = 0;
    Scroll = 0;
    RowDirtyA = -1;
    RowDirtyB = -1;
    LastClickItem = -1;
    ShownSecond = 0xFFFFFFFF;
    SortMode = SORT_NAME;
    ShowHidden = 0;
    Autoplay = 1;
    Flags = 0;

    if (FsHasCwd()) {
        for (int i = 0; i < FsVolumeCount(); i++) {
            if (UiStrEqual(FsVolumeName(i), FsCwdVolume())) {
                Volume = i;
                UiStrCopy(Dir, FsCwdPath(), FS_PATH_MAX);
                break;
            }
        }
    }

    FillScreen(COL_BG);
    LoadDir(0);
    PlayerTakeFinished();

    while (KeyboardGetKey() != 0) {
    }

    for (;;) {
        int Key = KeyboardGetKey();

        if (Key != 0 && HandleKey(Key)) {
            break;
        }

        uint8_t Pressed;

        if (UiPollMouse(&Pressed) && (Pressed & 1)) {
            HandleClick();
        }

        PlayerPoll();

        if (PlayerTakeFinished()) {
            Flags |= DF_NOW | DF_LIST;

            if (PlayerReadFailed()) {
                SetError(E_AUDIO_READ, PlayingName);
            } else if (Autoplay) {
                AdvanceTrack();
            }
        }

        if (PlayerActive()) {
            uint32_t Seconds = PlayerElapsedSeconds();
            if (Seconds != ShownSecond) {
                ShownSecond = Seconds;
                Flags |= DF_NOW;
            }
        } else {
            ShownSecond = 0xFFFFFFFF;
        }

        if (Message[0] && TickCount >= MessageUntil) {
            Message[0] = 0;
            Flags |= DF_HINT;
        }

        Present();

        if (Key == 0) {
            __asm__ __volatile__("hlt");
        }
    }

    PlayerStop();
    UiCursorHide();
    ConsoleRefresh();
}
