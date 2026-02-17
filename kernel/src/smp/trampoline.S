.intel_syntax noprefix
/* Mudamos o nome da seção para ser único */
.section .smp_trampoline_blob, "ax"
.code16
.global smp_trampoline_entry

smp_trampoline_entry:
    .byte 0xFA          /* CLI */
    .byte 0x31, 0xC0    /* XOR AX, AX */
    .byte 0x8E, 0xD8    /* MOV DS, AX */
    .byte 0x8E, 0xC0    /* MOV ES, AX */
    .byte 0x8E, 0xD0    /* MOV SS, AX */

    /* DEBUG: 'R' */
    .byte 0xBA, 0xF8, 0x03, 0xB0, 0x52, 0xEE
    
    /* Load GDT */
    .byte 0x67, 0x0F, 0x01, 0x15
    .long (gdtr_phys - smp_trampoline_entry + 0x8000)

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    .byte 0x66, 0xEA
    .long (trampoline_32 - smp_trampoline_entry + 0x8000)
    .word 0x08

.code32
trampoline_32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    
    /* DEBUG: 'P' */
    mov dx, 0x3F8
    mov al, 'P'
    out dx, al

    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    mov edi, 0x8000
    add edi, offset (smp_trampoline_cr3 - smp_trampoline_entry)
    mov eax, [edi]
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) 
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    .byte 0xEA
    .long (trampoline_64 - smp_trampoline_entry + 0x8000)
    .word 0x18

.code64
trampoline_64:
    xor rax, rax
    mov ds, ax
    mov es, ax
    mov ss, ax
    
    /* DEBUG: 'L' */
    mov dx, 0x3F8
    mov al, 'L'
    out dx, al

    mov rbx, 0x8000
    mov rax, offset (smp_trampoline_stack - smp_trampoline_entry)
    add rax, rbx
    mov rsp, [rax]
    
    mov rax, offset (smp_trampoline_entry_ptr - smp_trampoline_entry)
    add rax, rbx
    mov rax, [rax]
    
    call rax

hang:
    cli
    hlt
    jmp hang

.align 16
gdt_start:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF 
    .quad 0x00CF92000000FFFF 
    .quad 0x00AF9A000000FFFF 
gdt_end:

gdtr_phys:
    .word gdt_end - gdt_start - 1
    .long (0x8000 + (gdt_start - smp_trampoline_entry))

.global smp_trampoline_cr3
.global smp_trampoline_stack
.global smp_trampoline_entry_ptr

.align 8
smp_trampoline_cr3:       .quad 0
smp_trampoline_stack:     .quad 0
smp_trampoline_entry_ptr: .quad 0