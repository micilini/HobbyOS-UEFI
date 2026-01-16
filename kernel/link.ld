OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x2000000; /* 32MB */

    . = ALIGN(0x1000);
    .text : { *(.text .text.*) }

    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(0x1000);
    .data : { *(.data .data.*) }

    . = ALIGN(0x1000);
    .bss : { *(.bss .bss.*) *(COMMON) }
}