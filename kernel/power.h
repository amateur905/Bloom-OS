#pragma once

#include <stdint.h>

void PowerInit(uint64_t Rsdp);
int PowerShutdown(void);
void PowerReboot(void);
