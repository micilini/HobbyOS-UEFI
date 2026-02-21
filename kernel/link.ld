OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_P_BASE = 0x02000000;
KERNEL_V_BASE_OFFSET = 0xFFFFFFFF82000000;
VIRT_TO_PHYS_OFFSET = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_P_BASE;

    .boot_text : {
        KEEP(*(.boot_text))
    }

    .boot_data : {
        KEEP(*(.boot_data))
    }

    . = ALIGN(4096);
    _phys_end_boot = .;

    . = KERNEL_V_BASE_OFFSET + (_phys_end_boot - KERNEL_P_BASE);

    .text : AT(ADDR(.text) - VIRT_TO_PHYS_OFFSET) {
        _kernel_start = .;
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .trampoline : AT(ADDR(.trampoline) - VIRT_TO_PHYS_OFFSET) {
        _trampoline_start = .;
        KEEP(*(.smp_trampoline_blob)) 
        _trampoline_end = .;
    }
    . = ALIGN(4096);

    .rodata : AT(ADDR(.rodata) - VIRT_TO_PHYS_OFFSET) {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - VIRT_TO_PHYS_OFFSET) {
        *(.data .data.*)
    }

    .bss : AT(ADDR(.bss) - VIRT_TO_PHYS_OFFSET) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    .extra : AT(ADDR(.extra) - VIRT_TO_PHYS_OFFSET) {
        *(.*) 
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}