.intel_syntax noprefix
.section .boot_text, "ax"
.global _start

.set PAGE_PRESENT, (1 << 0)
.set PAGE_RW,      (1 << 1)
.set PAGE_HUGE,    (1 << 7)

_start:
    cli

    mov r15, rdi
    mov r14, rcx

    mov rsp, offset boot_stack_top
    
    mov rdi, offset boot_pml4

    mov rax, offset boot_pdpt_low
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi], rax

    mov rax, offset boot_pdpt_high
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 511 * 8], rax

    
    mov rdi, offset boot_pdpt_low
    mov rax, offset boot_pd0
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 0*8], rax

    mov rax, offset boot_pd1
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 1*8], rax

    mov rax, offset boot_pd2
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 2*8], rax

    mov rax, offset boot_pd3
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 3*8], rax


    mov rdi, offset boot_pd0
    mov rax, 0
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd

    mov rdi, offset boot_pd1
    mov rax, 0x40000000
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd

    mov rdi, offset boot_pd2
    mov rax, 0x80000000
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd

    mov rdi, offset boot_pd3
    mov rax, 0xC0000000
    or rax, (PAGE_PRESENT | PAGE_RW | PAGE_HUGE)
    call fill_pd

    mov rdi, offset boot_pdpt_high
    
    mov rax, offset boot_pd0
    or rax, (PAGE_PRESENT | PAGE_RW)
    mov [rdi + 510 * 8], rax
    
    mov rax, offset boot_pml4
    mov cr3, rax
    mov rdi, r15
    
    mov rax, offset kernel_main_high
    jmp rax

fill_pd:
    mov ecx, 512
    xor rbx, rbx
1:
    mov [rdi + rbx * 8], rax
    add rax, 0x200000
    inc rbx
    loop 1b
    ret

.section .boot_data
.align 4096

.global boot_pml4
boot_pml4:
    .skip 4096
boot_pdpt_low:
    .skip 4096
boot_pdpt_high:
    .skip 4096

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