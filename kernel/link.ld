OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_P_BASE = 0x02000000;
KERNEL_V_BASE_OFFSET = 0xFFFFFFFF82000000;
VIRT_TO_PHYS_OFFSET = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_P_BASE;

    .boot_text : {
        _boot_text_start = .;
        KEEP(*(.boot_text))
        _boot_text_end = .;
    }

    .boot_data : {
        _boot_data_start = .;
        KEEP(*(.boot_data))
        _boot_data_end = .;
    }

    . = ALIGN(4096);
    _phys_end_boot = .;

    . = KERNEL_V_BASE_OFFSET + (_phys_end_boot - KERNEL_P_BASE);

    .text : AT(ADDR(.text) - VIRT_TO_PHYS_OFFSET) {
        _kernel_start = .;
        _text_start = .;
        *(.text .text.*)
        _text_end = .;
    }

    . = ALIGN(4096);
    .trampoline : AT(ADDR(.trampoline) - VIRT_TO_PHYS_OFFSET) {
        _trampoline_start = .;
        KEEP(*(.smp_trampoline_blob)) 
        _trampoline_end = .;
    }
    . = ALIGN(4096);

    .rodata : AT(ADDR(.rodata) - VIRT_TO_PHYS_OFFSET) {
        _rodata_start = .;
        *(.rodata .rodata.*)
        _rodata_end = .;
    }

    . = ALIGN(16);
    .cpu_msr_fixup : AT(ADDR(.cpu_msr_fixup) - VIRT_TO_PHYS_OFFSET) {
        _cpu_msr_fixup_start = .;
        KEEP(*(.cpu_msr_fixup))
        _cpu_msr_fixup_end = .;
    }

    . = ALIGN(4096);
    .data : AT(ADDR(.data) - VIRT_TO_PHYS_OFFSET) {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    }

    .bss : AT(ADDR(.bss) - VIRT_TO_PHYS_OFFSET) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        bsp_stack_guard_start = .;
        . += 4096;
        bsp_stack_guard_end = .;
        KEEP(*(.bsp_stack))
        _bss_end = .;
    }

    _kernel_end = .;
    ASSERT(bsp_stack_top - bsp_stack_bottom == 16384, "BSP C stack capacity changed")
    ASSERT(bsp_stack_bottom == bsp_stack_guard_end &&
           bsp_stack_guard_end - bsp_stack_guard_start == 4096 &&
           (bsp_stack_guard_start & 4095) == 0,
           "BSP C stack guard must be one adjacent page")
    ASSERT(bsp_stack_bottom >= _kernel_start && bsp_stack_top <= _kernel_end,
           "BSP C stack is outside the linked image")
    ASSERT(bsp_stack_top - VIRT_TO_PHYS_OFFSET <= 0x40000000,
           "BSP C stack exceeds the entry root high mapping")

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
        *(.comment .comment.*)
    }
}
