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
