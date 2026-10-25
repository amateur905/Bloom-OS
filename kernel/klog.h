#pragma once

#include <stdint.h>

void KLog(const char *s);
void KLogUInt(uint64_t Value);
void KLogHex(uint64_t Value, int MinDigits);
void KLogPci(const char *Tag, uint8_t Bus, uint8_t Device, uint8_t Function, uint16_t VendorId, uint16_t DeviceId);
const char *KLogText(void);
