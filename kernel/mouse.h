#pragma once

#include <stdint.h>

void InitMouse(void);
int MouseGetDelta(int *Dx, int *Dy, uint8_t *Buttons);
uint8_t MouseTakePressed(void);
