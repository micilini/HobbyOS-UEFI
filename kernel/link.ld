/* kernel/link.ld */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x100000; /* Load kernel in addr upper to 1MB */

    .text : { *(.text) }
    .data : { *(.data) }
    .bss  : { *(.bss)  }
}