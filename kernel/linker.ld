ENTRY(_start)
SECTIONS
{
    . = 0x100000;
    .text : {
        *(.text.start)
        *(.text)
    }
    .rodata : {
        *(.rodata*)
    }
    .data : {
        *(.data)
        *(.bss)
        *(COMMON)
    }
}
