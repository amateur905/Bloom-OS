#pragma once

#include <stdint.h>

typedef struct {
    uint8_t Bus;
    uint8_t Device;
    uint8_t Function;
    uint16_t VendorId;
    uint16_t DeviceId;
    uint32_t Abar;
    int Found;
} AHCI_LOCATION;

AHCI_LOCATION FindAhciController(void);

typedef struct {
    uint8_t Bus;
    uint8_t Device;
    uint8_t Function;
    uint16_t VendorId;
    uint16_t DeviceId;
    uint32_t Bar0;
    int Found;
} HDA_LOCATION;

HDA_LOCATION FindHdaController(void);

typedef struct {
    uint8_t Bus;
    uint8_t Device;
    uint8_t Function;
    uint16_t VendorId;
    uint16_t DeviceId;
    uint64_t Bar0;
    int Found;
} NVME_LOCATION;

NVME_LOCATION FindNvmeController(void);

typedef struct {
    uint8_t Bus;
    uint8_t Device;
    uint8_t Function;
    uint16_t VendorId;
    uint16_t DeviceId;
    uint8_t ClassCode;
    uint8_t SubClass;
    uint8_t ProgIf;
    uint64_t Bar0;
} PCI_DEVICE_INFO;

int PciFindByClass(uint8_t ClassCode, uint8_t SubClass, int ProgIf, int Skip, PCI_DEVICE_INFO *Out);

uint16_t PciEnableDevice(uint8_t Bus, uint8_t Device, uint8_t Function);
uint32_t PciConfigRead32(uint8_t Bus, uint8_t Device, uint8_t Function, uint8_t Offset);
void PciConfigWrite32(uint8_t Bus, uint8_t Device, uint8_t Function, uint8_t Offset, uint32_t Value);
