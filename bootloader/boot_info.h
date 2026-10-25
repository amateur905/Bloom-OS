#ifndef BOOT_INFO_H
#define BOOT_INFO_H

typedef struct {
    UINT64 FrameBufferBase;
    UINT64 FrameBufferSize;
    UINT64 Width;
    UINT64 Height;
    UINT64 PixelsPerScanLine;
    UINT64 MemoryMapBase;
    UINT64 MemoryMapSize;
    UINT64 MemoryMapDescriptorSize;
    UINT32 MemoryMapDescriptorVersion;
    UINT64 AcpiRsdp;
} BOOT_INFO;

#define KERNEL_LOAD_ADDRESS 0x100000

#endif
