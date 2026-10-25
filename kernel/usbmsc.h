#pragma once

#include "xhci.h"

int UsbScan(int *NewBlocks, int MaxNew, int *Removed);
int UsbMscBlocks(const USB_DEVICE *Dev, char *Out, int Max);
