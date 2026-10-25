#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint64_t Width;
    uint64_t Height;
    uint64_t PixelsPerScanLine;
    uint64_t MemoryMapBase;
    uint64_t MemoryMapSize;
    uint64_t MemoryMapDescriptorSize;
    uint32_t MemoryMapDescriptorVersion;
    uint64_t AcpiRsdp;
} BOOT_INFO;

#endif
