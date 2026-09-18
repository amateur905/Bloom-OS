#include "blockdev.h"

static BLOCK_READ_FN ActiveReadFn;
static const char *ActiveName = "none";

void
RegisterBlockDevice(BLOCK_READ_FN ReadFn, const char *Name)
{
    ActiveReadFn = ReadFn;
    ActiveName = Name;
}

int
BlockDeviceReady(void)
{
    return ActiveReadFn != 0;
}

int
BlockReadSectors(uint64_t Lba, uint32_t Count, void *Buffer)
{
    if (!ActiveReadFn) {
        return 0;
    }

    return ActiveReadFn(Lba, Count, Buffer);
}

const char *
BlockDeviceName(void)
{
    return ActiveName;
}
