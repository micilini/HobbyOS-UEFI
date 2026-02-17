OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* Bases exatas para o HobbyOS:
   Físico: 32MB (0x02000000)
   Virtual: -2GB + 32MB Offset (0xFFFFFFFF82000000)
*/
KERNEL_P_BASE = 0x02000000;
KERNEL_V_BASE_OFFSET = 0xFFFFFFFF82000000;
VIRT_TO_PHYS_OFFSET = 0xFFFFFFFF80000000;

SECTIONS
{
    /* --- 1. PARTE FÍSICA (Identity Mapped / Loader) --- */
    /* O .boot_text deve ser o primeiro para o entry point bater com o início do arquivo ELF */
    . = KERNEL_P_BASE;

    .boot_text : {
        KEEP(*(.boot_text))
    }

    .boot_data : {
        KEEP(*(.boot_data))
    }

    . = ALIGN(4096);
    _phys_end_boot = .;

    /* --- 2. PARTE VIRTUAL (Higher Half Kernel) --- */
    
    /* Forçamos o VMA para começar exatamente onde o BSP espera */
    . = KERNEL_V_BASE_OFFSET + (_phys_end_boot - KERNEL_P_BASE);

    /* Código Principal do Kernel */
    .text : AT(ADDR(.text) - VIRT_TO_PHYS_OFFSET) {
        _kernel_start = .;
        *(.text .text.*)
    }

    /* TRAMPOLINE: Inserido aqui para garantir que o bootloader o carregue na RAM.
       Usamos a seção única .smp_trampoline_blob definida no novo trampoline.S */
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

    /* Seção extra para qualquer outro dado desalinhado */
    .extra : AT(ADDR(.extra) - VIRT_TO_PHYS_OFFSET) {
        *(.*) 
    }

    _kernel_end = .;

    /* Remove lixo que pode confundir o bootloader ou aumentar o binário */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}