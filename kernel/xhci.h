#pragma once

#include <stdint.h>

#define XHCI_OK 1
#define XHCI_FAIL 0
#define XHCI_STALL -2

#define USB_SPEED_FULL 1
#define USB_SPEED_LOW 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4
#define USB_SPEED_SUPER_PLUS 5

#define USB_MAX_DEVICES 32

typedef struct {
    uint32_t P0;
    uint32_t P1;
    uint32_t S;
    uint32_t C;
} XHCI_TRB;

typedef struct {
    volatile XHCI_TRB *Base;
    uint64_t Phys;
    uint32_t Enq;
    uint32_t Cycle;
    uint32_t Size;
} XHCI_RING;

typedef struct {
    int Used;
    int Gone;
    int Attached;
    int Controller;
    int Slot;
    int Port;
    int Speed;
    uint16_t VendorId;
    uint16_t ProductId;
    uint8_t DevClass;
    uint8_t DevSubClass;
    uint8_t DevProtocol;
    int IsMass;
    uint8_t ConfigValue;
    uint8_t Interface;
    uint8_t BulkInAddr;
    uint8_t BulkOutAddr;
    uint16_t BulkInMps;
    uint16_t BulkOutMps;
    uint8_t BulkInBurst;
    uint8_t BulkOutBurst;
    int HubSeen;
    uint32_t Ep0Mps;
    uint8_t *OutCtx;
    uint8_t *InCtx;
    XHCI_RING Ring[32];
} USB_DEVICE;

int XhciRescan(void);
int XhciControllerCount(void);
int XhciDeviceCount(void);
USB_DEVICE *XhciDevice(int Index);
void XhciRelease(USB_DEVICE *Dev);
int XhciControl(USB_DEVICE *Dev, uint8_t RequestType, uint8_t Request, uint16_t Value, uint16_t Index, void *Data, uint16_t Length);
int XhciBulk(USB_DEVICE *Dev, int In, void *Buffer, uint32_t Length, uint32_t *Actual);
int XhciClearHalt(USB_DEVICE *Dev, int In);
uint32_t XhciMaxBulk(void);
const char *UsbSpeedName(int Speed);
