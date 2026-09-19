#include <stdint.h>
#include "keyboard.h"
#include "idt.h"

#define KEY_BUFFER_SIZE 256

static int KeyBuffer[KEY_BUFFER_SIZE];
static int KeyBufferHead = 0;
static int KeyBufferTail = 0;

static int ShiftHeld = 0;
static int CtrlHeld = 0;
static int AltHeld = 0;
static int ExtendedPrefix = 0;

static int CapsLockOn = 0;
static int NumLockOn = 0;
static int ScrollLockOn = 0;

static const char ScancodeToAscii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*',
    0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7',
    '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,
};

static const char ScancodeToAsciiShift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^',
    '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*',
    0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7',
    '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,
};

static void
OutB(uint16_t port, uint8_t value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t
InB(uint16_t port)
{
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void
WaitInputBufferEmpty(void)
{
    while (InB(0x64) & 0x02) {
    }
}

static void
UpdateLeds(void)
{
    uint8_t Mask = 0;

    if (ScrollLockOn) {
        Mask |= 0x01;
    }
    if (NumLockOn) {
        Mask |= 0x02;
    }
    if (CapsLockOn) {
        Mask |= 0x04;
    }

    WaitInputBufferEmpty();
    OutB(0x60, 0xED);
    WaitInputBufferEmpty();
    OutB(0x60, Mask);
}

static void
PushChar(int c)
{
    int Next = (KeyBufferHead + 1) % KEY_BUFFER_SIZE;

    if (Next == KeyBufferTail) {
        return;
    }

    KeyBuffer[KeyBufferHead] = c;
    KeyBufferHead = Next;
}

static void
KeyboardIrqHandler(void)
{
    uint8_t Status = InB(0x64);

    if (Status & 0x20) {
        InB(0x60);
        return;
    }

    uint8_t Scancode = InB(0x60);

    if (Scancode == 0xE0) {
        ExtendedPrefix = 1;
        return;
    }

    int Released = Scancode & 0x80;
    uint8_t Code = Scancode & 0x7F;

    if (ExtendedPrefix) {
        ExtendedPrefix = 0;

        if (Code == 0x1D) {
            CtrlHeld = !Released;
        } else if (Code == 0x38) {
            AltHeld = !Released;
        } else if (!Released) {
            switch (Code) {
            case 0x48: PushChar(KEY_UP); break;
            case 0x50: PushChar(KEY_DOWN); break;
            case 0x4B: PushChar(KEY_LEFT); break;
            case 0x4D: PushChar(KEY_RIGHT); break;
            case 0x47: PushChar(KEY_HOME); break;
            case 0x4F: PushChar(KEY_END); break;
            case 0x53: PushChar(KEY_DELETE); break;
            case 0x49: PushChar(KEY_PGUP); break;
            case 0x51: PushChar(KEY_PGDN); break;
            case 0x1C: PushChar('\n'); break;
            case 0x35: PushChar('/'); break;
            default: break;
            }
        }

        return;
    }

    if (Code == 0x2A || Code == 0x36) {
        ShiftHeld = !Released;
        return;
    }

    if (Code == 0x1D) {
        CtrlHeld = !Released;
        return;
    }

    if (Code == 0x38) {
        AltHeld = !Released;
        return;
    }

    if (Released) {
        return;
    }

    if (Code == 0x3A) {
        CapsLockOn = !CapsLockOn;
        UpdateLeds();
        return;
    }

    if (Code == 0x45) {
        NumLockOn = !NumLockOn;
        UpdateLeds();
        return;
    }

    if (Code == 0x46) {
        ScrollLockOn = !ScrollLockOn;
        UpdateLeds();
        return;
    }

    char Ascii = ShiftHeld ? ScancodeToAsciiShift[Code] : ScancodeToAscii[Code];

    if (Ascii == 0) {
        return;
    }

    if (CapsLockOn && Ascii >= 'a' && Ascii <= 'z') {
        Ascii = Ascii - 'a' + 'A';
    } else if (CapsLockOn && Ascii >= 'A' && Ascii <= 'Z') {
        Ascii = Ascii - 'A' + 'a';
    }

    if (CtrlHeld || AltHeld) {
        return;
    }

    PushChar(Ascii);
}

void
InitKeyboard(void)
{
    while (InB(0x64) & 0x01) {
        InB(0x60);
    }

    RegisterIrqHandler(1, KeyboardIrqHandler);
    UnmaskIrq(1);
    UpdateLeds();
}

int
KeyboardGetKey(void)
{
    if (KeyBufferTail == KeyBufferHead) {
        return 0;
    }

    int Key = KeyBuffer[KeyBufferTail];
    KeyBufferTail = (KeyBufferTail + 1) % KEY_BUFFER_SIZE;
    return Key;
}

char
KeyboardGetChar(void)
{
    int Key = KeyboardGetKey();

    if (Key <= 0 || Key > 0xFF) {
        return 0;
    }

    return (char)Key;
}

int
KeyboardCapsLock(void)
{
    return CapsLockOn;
}

int
KeyboardNumLock(void)
{
    return NumLockOn;
}

int
KeyboardScrollLock(void)
{
    return ScrollLockOn;
}
