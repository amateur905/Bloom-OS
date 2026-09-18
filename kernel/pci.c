#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static uint32_t
PciConfigReadDWord(uint8_t Bus, uint8_t Device, uint8_t Function, uint8_t Offset)
{
    uint32_t Address = (uint32_t)((1u << 31) |
        ((uint32_t)Bus << 16) |
        ((uint32_t)Device << 11) |
        ((uint32_t)Function << 8) |
        (Offset & 0xFC));

    OutL(PCI_CONFIG_ADDRESS, Address);
    return InL(PCI_CONFIG_DATA);
}

AHCI_LOCATION
FindAhciController(void)
{
    AHCI_LOCATION Result;
    Result.Found = 0;
    Result.Bus = 0;
    Result.Device = 0;
    Result.Function = 0;
    Result.VendorId = 0;
    Result.DeviceId = 0;
    Result.Abar = 0;

    for (uint32_t Bus = 0; Bus < 256; Bus++) {
        for (uint32_t Device = 0; Device < 32; Device++) {
            for (uint32_t Function = 0; Function < 8; Function++) {
                uint32_t IdReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x00);
                uint16_t VendorId = (uint16_t)(IdReg & 0xFFFF);

                if (VendorId == 0xFFFF) {
                    if (Function == 0) {
                        break;
                    }
                    continue;
                }

                uint32_t ClassReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x08);
                uint8_t ClassCode = (uint8_t)(ClassReg >> 24);
                uint8_t SubClass = (uint8_t)(ClassReg >> 16);
                uint8_t ProgIf = (uint8_t)(ClassReg >> 8);

                if (ClassCode == 0x01 && SubClass == 0x06 && ProgIf == 0x01) {
                    uint32_t Bar5 = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x24);

                    Result.Found = 1;
                    Result.Bus = (uint8_t)Bus;
                    Result.Device = (uint8_t)Device;
                    Result.Function = (uint8_t)Function;
                    Result.VendorId = VendorId;
                    Result.DeviceId = (uint16_t)(IdReg >> 16);
                    Result.Abar = Bar5 & 0xFFFFFFF0;
                    return Result;
                }

                uint32_t HeaderTypeReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x0C);
                uint8_t HeaderType = (uint8_t)(HeaderTypeReg >> 16);

                if (Function == 0 && !(HeaderType & 0x80)) {
                    break;
                }
            }
        }
    }

    return Result;
}

HDA_LOCATION
FindHdaController(void)
{
    HDA_LOCATION Result;
    Result.Found = 0;
    Result.Bus = 0;
    Result.Device = 0;
    Result.Function = 0;
    Result.VendorId = 0;
    Result.DeviceId = 0;
    Result.Bar0 = 0;

    for (uint32_t Bus = 0; Bus < 256; Bus++) {
        for (uint32_t Device = 0; Device < 32; Device++) {
            for (uint32_t Function = 0; Function < 8; Function++) {
                uint32_t IdReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x00);
                uint16_t VendorId = (uint16_t)(IdReg & 0xFFFF);

                if (VendorId == 0xFFFF) {
                    if (Function == 0) {
                        break;
                    }
                    continue;
                }

                uint32_t ClassReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x08);
                uint8_t ClassCode = (uint8_t)(ClassReg >> 24);
                uint8_t SubClass = (uint8_t)(ClassReg >> 16);

                if (ClassCode == 0x04 && SubClass == 0x03) {
                    uint32_t Bar0 = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x10);

                    Result.Found = 1;
                    Result.Bus = (uint8_t)Bus;
                    Result.Device = (uint8_t)Device;
                    Result.Function = (uint8_t)Function;
                    Result.VendorId = VendorId;
                    Result.DeviceId = (uint16_t)(IdReg >> 16);
                    Result.Bar0 = Bar0 & 0xFFFFFFF0;
                    return Result;
                }

                uint32_t HeaderTypeReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x0C);
                uint8_t HeaderType = (uint8_t)(HeaderTypeReg >> 16);

                if (Function == 0 && !(HeaderType & 0x80)) {
                    break;
                }
            }
        }
    }

    return Result;
}

NVME_LOCATION
FindNvmeController(void)
{
    NVME_LOCATION Result;
    Result.Found = 0;
    Result.Bus = 0;
    Result.Device = 0;
    Result.Function = 0;
    Result.VendorId = 0;
    Result.DeviceId = 0;
    Result.Bar0 = 0;

    for (uint32_t Bus = 0; Bus < 256; Bus++) {
        for (uint32_t Device = 0; Device < 32; Device++) {
            for (uint32_t Function = 0; Function < 8; Function++) {
                uint32_t IdReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x00);
                uint16_t VendorId = (uint16_t)(IdReg & 0xFFFF);

                if (VendorId == 0xFFFF) {
                    if (Function == 0) {
                        break;
                    }
                    continue;
                }

                uint32_t ClassReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x08);
                uint8_t ClassCode = (uint8_t)(ClassReg >> 24);
                uint8_t SubClass = (uint8_t)(ClassReg >> 16);
                uint8_t ProgIf = (uint8_t)(ClassReg >> 8);

                if (ClassCode == 0x01 && SubClass == 0x08 && ProgIf == 0x02) {
                    uint32_t Bar0Low = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x10);
                    uint32_t Bar0High = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x14);

                    Result.Found = 1;
                    Result.Bus = (uint8_t)Bus;
                    Result.Device = (uint8_t)Device;
                    Result.Function = (uint8_t)Function;
                    Result.VendorId = VendorId;
                    Result.DeviceId = (uint16_t)(IdReg >> 16);
                    Result.Bar0 = ((uint64_t)Bar0High << 32) | (Bar0Low & 0xFFFFFFF0);
                    return Result;
                }

                uint32_t HeaderTypeReg = PciConfigReadDWord((uint8_t)Bus, (uint8_t)Device, (uint8_t)Function, 0x0C);
                uint8_t HeaderType = (uint8_t)(HeaderTypeReg >> 16);

                if (Function == 0 && !(HeaderType & 0x80)) {
                    break;
                }
            }
        }
    }

    return Result;
}
