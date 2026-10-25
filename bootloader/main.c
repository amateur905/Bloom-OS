#include <efi.h>
#include <efilib.h>
#include "logo.h"
#include "ring.h"
#include "boot_info.h"

static UINT32 *FrameBuffer;
static UINT32 PixelsPerScanLine;

static EFI_LOADED_IMAGE *LoadedImage;
static EFI_FILE_HANDLE RootDir;
static EFI_FILE_HANDLE KernelFile;
static EFI_FILE_INFO *KernelInfo;
static UINTN KernelSize;

static INT32
InSegment(INT32 Dx, INT32 Dy, INT32 Index)
{
    long CrossStart = SegStartX[Index] * Dy - SegStartY[Index] * Dx;
    long CrossEnd = Dx * SegEndY[Index] - Dy * SegEndX[Index];
    return (CrossStart >= 0) && (CrossEnd >= 0);
}

static VOID
DrawRing(INT32 CenterX, INT32 CenterY, INT32 CompletedStages, INT32 ErrorStage)
{
    UINT32 PendingColor = 0x00403060;
    UINT32 CompletedColor = 0x00A070E0;
    UINT32 ErrorColor = 0x00D02020;

    INT32 R = RING_OUTER_RADIUS + 2;

    for (INT32 dy = -R; dy <= R; dy++) {
        for (INT32 dx = -R; dx <= R; dx++) {
            INT32 DistSq = dx * dx + dy * dy;
            if (DistSq < RING_INNER_RADIUS * RING_INNER_RADIUS ||
                DistSq > RING_OUTER_RADIUS * RING_OUTER_RADIUS) {
                continue;
            }

            for (INT32 i = 0; i < RING_SEGMENTS; i++) {
                if (InSegment(dx, dy, i)) {
                    UINT32 Color;
                    if (i == ErrorStage) {
                        Color = ErrorColor;
                    } else if (i < CompletedStages) {
                        Color = CompletedColor;
                    } else {
                        Color = PendingColor;
                    }
                    FrameBuffer[(CenterY + dy) * PixelsPerScanLine + (CenterX + dx)] = Color;
                    break;
                }
            }
        }
    }
}

static VOID
DrawLogo(UINT32 ScreenWidth, UINT32 ScreenHeight)
{
    UINT32 OffsetX = ScreenWidth / 2 - LOGO_WIDTH / 2;
    UINT32 OffsetY = ScreenHeight / 2 - LOGO_HEIGHT / 2;

    for (UINT32 y = 0; y < LOGO_HEIGHT; y++) {
        for (UINT32 x = 0; x < LOGO_WIDTH; x++) {
            UINT32 Pixel = BloomLogo[y * LOGO_WIDTH + x];
            UINT8 Alpha = (Pixel >> 24) & 0xFF;
            if (Alpha == 0) {
                continue;
            }
            FrameBuffer[(OffsetY + y) * PixelsPerScanLine + (OffsetX + x)] = Pixel & 0x00FFFFFF;
        }
    }
}

typedef struct {
    CHAR16 *Name;
    INT32 Success;
    EFI_STATUS Status;
} BOOT_STAGE_RESULT;

static BOOT_STAGE_RESULT
RunStage(INT32 StageIndex, EFI_HANDLE ImageHandle)
{
    BOOT_STAGE_RESULT Result;
    Result.Success = 1;
    Result.Status = EFI_SUCCESS;

    switch (StageIndex) {

    case 0:
        Result.Name = L"Locate loaded image info";
        Result.Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle,
            &gEfiLoadedImageProtocolGuid, (void **)&LoadedImage);
        break;

    case 1:
        Result.Name = L"Open root directory";
        RootDir = LibOpenRoot(LoadedImage->DeviceHandle);
        if (RootDir == NULL) {
            Result.Status = EFI_NOT_FOUND;
        }
        break;

    case 2:
        Result.Name = L"Open kernel file";
        Result.Status = uefi_call_wrapper(RootDir->Open, 5, RootDir, &KernelFile,
            L"KERNEL.BIN", EFI_FILE_MODE_READ, 0);
        break;

    case 3: {
        Result.Name = L"Load kernel at fixed address";
        KernelInfo = LibFileInfo(KernelFile);
        if (KernelInfo == NULL) {
            Result.Status = EFI_NOT_FOUND;
            break;
        }
        KernelSize = KernelInfo->FileSize;

        EFI_PHYSICAL_ADDRESS Addr = KERNEL_LOAD_ADDRESS;
        UINTN Pages = (KernelSize + 0xFFF) / 0x1000;

        Result.Status = uefi_call_wrapper(BS->AllocatePages, 4,
            AllocateAddress, EfiLoaderData, Pages, &Addr);
        if (EFI_ERROR(Result.Status)) {
            break;
        }

        Result.Status = uefi_call_wrapper(KernelFile->Read, 3, KernelFile, &KernelSize, (VOID *)(UINTN)Addr);
        break;
    }

    default:
        Result.Name = L"Unknown stage";
        break;
    }

    if (EFI_ERROR(Result.Status)) {
        Result.Success = 0;
    }

    uefi_call_wrapper(BS->Stall, 1, 300000);

    return Result;
}

typedef void (*KERNEL_ENTRY)(BOOT_INFO *);

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS Status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

    InitializeLib(ImageHandle, SystemTable);

    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    Print(L"Bloom-OS bootloader\r\n");
    Print(L"UEFI firmware initialized. Boot Services active.\r\n");

    Status = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (void **)&Gop);
    if (EFI_ERROR(Status)) {
        Print(L"GOP not found: %r\r\n", Status);
        goto hang;
    }

    FrameBuffer = (UINT32 *)Gop->Mode->FrameBufferBase;
    PixelsPerScanLine = Gop->Mode->Info->PixelsPerScanLine;
    UINT32 ScreenWidth = Gop->Mode->Info->HorizontalResolution;
    UINT32 ScreenHeight = Gop->Mode->Info->VerticalResolution;

    UINT32 BackgroundColor = 0x00201040;
    for (UINT32 y = 0; y < ScreenHeight; y++) {
        for (UINT32 x = 0; x < ScreenWidth; x++) {
            FrameBuffer[y * PixelsPerScanLine + x] = BackgroundColor;
        }
    }

    INT32 CenterX = ScreenWidth / 2;
    INT32 CenterY = ScreenHeight / 2;

    DrawLogo(ScreenWidth, ScreenHeight);
    DrawRing(CenterX, CenterY, 0, -1);

    for (INT32 Stage = 0; Stage < RING_SEGMENTS; Stage++) {
        BOOT_STAGE_RESULT Result = RunStage(Stage, ImageHandle);

        if (!Result.Success) {
            DrawRing(CenterX, CenterY, Stage, Stage);
            Print(L"Boot stage failed: %s (%r)\r\n", Result.Name, Result.Status);
            goto hang;
        }

        Print(L"Stage OK: %s\r\n", Result.Name);
        DrawRing(CenterX, CenterY, Stage + 1, -1);
    }

    Print(L"Boot stages complete. Kernel size: %d bytes\r\n", KernelSize);

    static BOOT_INFO Info;
    Info.FrameBufferBase = Gop->Mode->FrameBufferBase;
    Info.FrameBufferSize = Gop->Mode->FrameBufferSize;
    Info.Width = ScreenWidth;
    Info.Height = ScreenHeight;
    Info.PixelsPerScanLine = PixelsPerScanLine;
    Info.AcpiRsdp = 0;

    EFI_GUID Acpi20Guid = ACPI_20_TABLE_GUID;

    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (CompareGuid(&ST->ConfigurationTable[i].VendorGuid, &Acpi20Guid) == 0) {
            Info.AcpiRsdp = (UINT64)(UINTN)ST->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    if (Info.AcpiRsdp == 0) {
        for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
            if (CompareGuid(&ST->ConfigurationTable[i].VendorGuid, &AcpiTableGuid) == 0) {
                Info.AcpiRsdp = (UINT64)(UINTN)ST->ConfigurationTable[i].VendorTable;
                break;
            }
        }
    }

    UINTN MemMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemMap = NULL;
    UINTN MapKey = 0;
    UINTN DescriptorSize = 0;
    UINT32 DescriptorVersion = 0;

    const INT32 MaxExitAttempts = 5;
    INT32 Attempt;

    for (Attempt = 0; Attempt < MaxExitAttempts; Attempt++) {
        MemMapSize = 0;
        Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &MemMapSize, MemMap, &MapKey, &DescriptorSize, &DescriptorVersion);

        MemMapSize += DescriptorSize * 8;

        if (MemMap != NULL) {
            uefi_call_wrapper(BS->FreePool, 1, MemMap);
            MemMap = NULL;
        }

        Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, MemMapSize, (void **)&MemMap);
        if (EFI_ERROR(Status)) {
            Print(L"AllocatePool for memory map failed: %r\r\n", Status);
            goto hang;
        }

        Status = uefi_call_wrapper(BS->GetMemoryMap, 5, &MemMapSize, MemMap, &MapKey, &DescriptorSize, &DescriptorVersion);
        if (EFI_ERROR(Status)) {
            Print(L"GetMemoryMap failed (attempt %d): %r\r\n", Attempt, Status);
            continue;
        }

        Status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, MapKey);
        if (!EFI_ERROR(Status)) {
            break;
        }
    }

    if (EFI_ERROR(Status)) {
        Print(L"ExitBootServices failed after %d attempts: %r\r\n", MaxExitAttempts, Status);
        goto hang;
    }

    Info.MemoryMapBase = (UINT64)(UINTN)MemMap;
    Info.MemoryMapSize = (UINT64)MemMapSize;
    Info.MemoryMapDescriptorSize = (UINT64)DescriptorSize;
    Info.MemoryMapDescriptorVersion = DescriptorVersion;

    KERNEL_ENTRY KernelMain = (KERNEL_ENTRY)(UINTN)KERNEL_LOAD_ADDRESS;
    KernelMain(&Info);

hang:
    while (1) {
        __asm__ __volatile__("hlt");
    }

    return EFI_SUCCESS;
}
