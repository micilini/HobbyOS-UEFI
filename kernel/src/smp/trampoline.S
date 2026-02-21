.intel_syntax noprefix
.section .smp_trampoline_blob, "ax"
.code16
.global smp_trampoline_entry
.global _trampoline_start
.global _trampoline_end

_trampoline_start:

smp_trampoline_entry:
    cli
    cld             
    
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax  
    
    mov sp, 0x1000 

    mov dx, 0x3F8
    mov al, 'R'
    out dx, al

    wbinvd

    mov di, (smp_trampoline_gdtr - smp_trampoline_entry)
    lgdt [di]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    mov ax, cs
    movzx eax, ax
    shl eax, 4
    add eax, (trampoline_32 - smp_trampoline_entry)
    
    push 0x00       
    push 0x08       
    push eax        
    
    .byte 0x66
    retf

.code32
trampoline_32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    
    call .get_eip
.get_eip:
    pop ebp
    sub ebp, (.get_eip - smp_trampoline_entry)
    
    mov esp, ebp
    add esp, 0x1000

    mov dx, 0x3F8
    mov al, 'P'
    out dx, al

    mov eax, [ebp + (smp_trampoline_cr4 - smp_trampoline_entry)]
    mov cr4, eax

    mov eax, [ebp + (smp_trampoline_cr3 - smp_trampoline_entry)]
    mov cr3, eax

    mov ecx, 0xC0000080
    mov eax, [ebp + (smp_trampoline_efer - smp_trampoline_entry)]
    mov edx, [ebp + (smp_trampoline_efer - smp_trampoline_entry) + 4]
    wrmsr

    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    mov eax, ebp
    add eax, (trampoline_64 - smp_trampoline_entry)
    
    push 0x18
    push eax
    retf

.code64
trampoline_64:
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov dx, 0x3F8
    mov al, 'L'
    out dx, al

    mov eax, dword ptr [rbp + (smp_trampoline_x2apic_flag - smp_trampoline_entry)]
    test eax, eax
    jz .skip_x2apic

    mov ecx, 0x1B
    rdmsr
    or eax, (1 << 11) | (1 << 10)
    wrmsr

.skip_x2apic:
    mov rsp, qword ptr [rbp + (smp_trampoline_stack - smp_trampoline_entry)]
    
    test rsp, rsp
    jz .bad_stack

    and rsp, -16

    mov rax, qword ptr [rbp + (smp_trampoline_entry_ptr - smp_trampoline_entry)]
    
    test rax, rax
    jz .bad_entry

    mov rbx, 0xFFFFFFFF80000000
    cmp rax, rbx
    jb .bad_kernel_ptr

    xor rbp, rbp
    jmp rax

hang:
    cli
    hlt
    jmp hang

.bad_stack:
    mov dx, 0x3F8
    mov al, 'S'     
    out dx, al
    jmp hang

.bad_entry:
    mov dx, 0x3F8
    mov al, 'Z'     
    out dx, al
    jmp hang

.bad_kernel_ptr:
    mov dx, 0x3F8
    mov al, 'K'
    out dx, al
    jmp hang

.align 16
.global smp_trampoline_gdt_start
smp_trampoline_gdt_start:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF 
    .quad 0x00CF92000000FFFF 
    .quad 0x00AF9A000000FFFF 
smp_trampoline_gdt_end:

.global smp_trampoline_gdtr
smp_trampoline_gdtr:
    .word smp_trampoline_gdt_end - smp_trampoline_gdt_start - 1
    .long 0   

.global smp_trampoline_cr3
.global smp_trampoline_cr4
.global smp_trampoline_efer
.global smp_trampoline_stack
.global smp_trampoline_entry_ptr
.global smp_trampoline_x2apic_flag
.global smp_trampoline_base

.align 8
smp_trampoline_cr3:           .quad 0
smp_trampoline_cr4:           .quad 0
smp_trampoline_efer:          .quad 0
smp_trampoline_stack:         .quad 0
smp_trampoline_entry_ptr:     .quad 0
smp_trampoline_x2apic_flag:   .long 0
smp_trampoline_base:          .long 0

_trampoline_end:

.section .note.GNU-stack,"",@progbits