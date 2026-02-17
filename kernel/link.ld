OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* Definimos as bases exatas:
   Físico: 32MB (0x02000000)
   Virtual: -2GB + 32MB Offset (0xFFFFFFFF82000000)
   
   Isso garante que o índice da Page Table (510) + o Offset (16 * 2MB)
   batam exatamente um com o outro.
*/
KERNEL_P_BASE = 0x02000000;
KERNEL_V_BASE_OFFSET = 0xFFFFFFFF82000000;
VIRT_TO_PHYS_OFFSET = 0xFFFFFFFF80000000;

SECTIONS
{
    /* --- 1. Parte Física (Trampolim) --- */
    . = KERNEL_P_BASE;

    .boot_text : {
        KEEP(*(.boot_text))
    }

    .boot_data : {
        KEEP(*(.boot_data))
    }

    . = ALIGN(4096);
    _phys_end_boot = .;

    /* --- 2. Parte Virtual (Kernel C) --- */
    
    /* Forçamos o VMA para começar exatamente onde queremos */
    . = KERNEL_V_BASE_OFFSET + (_phys_end_boot - KERNEL_P_BASE);

    /* O cálculo AT() usa o offset padrão de -2GB para converter de volta pro físico */
    .text : AT(ADDR(.text) - VIRT_TO_PHYS_OFFSET) {
        _kernel_start = .;
        *(.text .text.*)
    }

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