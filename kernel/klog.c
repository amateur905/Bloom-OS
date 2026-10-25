#include <stdint.h>
#include "klog.h"

#define KLOG_SIZE 16384

static char Buffer[KLOG_SIZE];
static uint32_t Length;

static void
PutChar(char c)
{
    if (Length < KLOG_SIZE - 1) {
        Buffer[Length++] = c;
    }
}

void
KLog(const char *s)
{
    while (*s) {
        PutChar(*s++);
    }
}

void
KLogUInt(uint64_t Value)
{
    char Digits[21];
    int Pos = 20;
    Digits[Pos] = 0;

    if (Value == 0) {
        Digits[--Pos] = '0';
    } else {
        while (Value > 0) {
            Digits[--Pos] = (char)('0' + (Value % 10));
            Value /= 10;
        }
    }

    KLog(&Digits[Pos]);
}

void
KLogHex(uint64_t Value, int MinDigits)
{
    char Digits[17];
    int Pos = 16;
    Digits[Pos] = 0;

    do {
        uint8_t Nibble = (uint8_t)(Value & 0xF);
        Digits[--Pos] = (char)(Nibble < 10 ? '0' + Nibble : 'a' + Nibble - 10);
        Value >>= 4;
    } while ((Value > 0 || (16 - Pos) < MinDigits) && Pos > 0);

    KLog("0x");
    KLog(&Digits[Pos]);
}

void
KLogPci(const char *Tag, uint8_t Bus, uint8_t Device, uint8_t Function, uint16_t VendorId, uint16_t DeviceId)
{
    KLog(Tag);
    KLog(": found at ");
    KLogHex(Bus, 2);
    KLog(":");
    KLogHex(Device, 2);
    KLog(".");
    KLogUInt(Function);
    KLog(" id ");
    KLogHex(VendorId, 4);
    KLog(":");
    KLogHex(DeviceId, 4);
    KLog("\n");
}

const char *
KLogText(void)
{
    Buffer[Length] = 0;
    return Buffer;
}
