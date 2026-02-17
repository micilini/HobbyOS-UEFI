.intel_syntax noprefix
.section .boot_text, "ax"
.global _start

.set PAGE_PRESENT, (1 << 0)
.set PAGE_RW,      (1 << 1)
.set PAGE_HUGE,    (1 << 7)

_start:
    cli

    /* Salva argumentos do bootloader (RDI = boot_info) */
    mov r15, rdi
    mov r14, rcx

    /* Configura stack */
    mov rsp, offset boot_stack_top
    
    /* --- CONFIGURANDO PAGINAÇÃO (4GB IDENTITY) --- */
    mov rdi, offset boot_pml4

    /* 1. Configurar PML4 */
    /* PML4[0] -> Identity Map (0-512GB) */
    mov rax, offset boot_pdpt_low
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi], rax

    /* PML4[511] -> Higher Half (-512GB) */
    mov rax, offset boot_pdpt_high
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 511 * 8], rax

    /* 2. Configurar PDPT Low (Identity 0-4GB) */
    /* Precisamos de 4 entradas apontando para 4 PDs diferentes */
    
    /* PDPT[0] -> 0-1GB */
    mov rdi, offset boot_pdpt_low
    mov rax, offset boot_pd0
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 0*8], rax

    /* PDPT[1] -> 1-2GB (Onde seu boot_info está!) */
    mov rax, offset boot_pd1
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 1*8], rax

    /* PDPT[2] -> 2-3GB */
    mov rax, offset boot_pd2
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 2*8], rax

    /* PDPT[3] -> 3-4GB */
    mov rax, offset boot_pd3
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 3*8], rax


    /* 3. Preencher as 4 Page Directories (PDs) */
    /* Vamos usar uma subrotina ou macro? Não, vamos fazer no loop bruto pra ser simples. */
    
    /* Configura PD0 (0-1GB) */
    mov rdi, offset boot_pd0
    mov rax, 0  /* Começa no físico 0 */
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd

    /* Configura PD1 (1-2GB) */
    mov rdi, offset boot_pd1
    mov rax, 0x40000000 /* Começa no físico 1GB */
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd

    /* Configura PD2 (2-3GB) */
    mov rdi, offset boot_pd2
    mov rax, 0x80000000 /* Começa no físico 2GB */
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd

    /* Configura PD3 (3-4GB) */
    mov rdi, offset boot_pd3
    mov rax, 0xC0000000 /* Começa no físico 3GB */
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd


    /* 4. Configurar PDPT High (Higher Half Kernel) */
    /* Mapeamos os primeiros 2GB no final do virtual também, pra garantir */
    mov rdi, offset boot_pdpt_high
    
    /* Mapa -2GB (Kernel Space) -> PD0 (0-1GB Físico) */
    /* Isso cobre 0xFFFFFFFF80000000 até ...C0000000 */
    /* Seu kernel está em 32MB, então PD0 resolve. */
    mov rax, offset boot_pd0
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 510 * 8], rax
    
    /* (Opcional) Mapear o próximo GB se o kernel fosse gigante */
    /* mov rax, offset boot_pd1 ... [rdi + 511 * 8] ... */

    /* --- ATIVAR --- */
    mov rax, offset boot_pml4
    mov cr3, rax
    mov rdi, r15 /* Restaura boot_info */
    
    mov rax, offset kernel_main_high
    jmp rax

/* Subrotina simples para preencher 512 entradas de uma PD */
/* RDI = Endereço da PD, RAX = Endereço Físico Inicial + Flags */
fill_pd:
    mov ecx, 512
    xor rbx, rbx
1:
    mov [rdi + rbx * 8], rax
    add rax, 0x200000 /* +2MB */
    inc rbx
    loop 1b
    ret

/* --- DADOS (SEÇÃO BSS/DATA) --- */
.section .boot_data
.align 4096

.global boot_pml4
boot_pml4:
    .skip 4096
boot_pdpt_low:
    .skip 4096
boot_pdpt_high:
    .skip 4096

/* Precisamos de 4 PDs para cobrir 4GB */
boot_pd0:
    .skip 4096
boot_pd1:
    .skip 4096
boot_pd2:
    .skip 4096
boot_pd3:
    .skip 4096

.align 16
.skip 16384
boot_stack_top: