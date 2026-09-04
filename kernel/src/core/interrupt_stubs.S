.intel_syntax noprefix
.section .text
.global irq_vector_stub_table

.macro IRQ_SAVE_ALL
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
.endm

.macro IRQ_RESTORE_ALL
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
.endm

irq_external_common_entry:
    IRQ_SAVE_ALL
    sub rsp, 8
    mov edi, dword ptr [rsp + 128]
    call irq_external_dispatch
    add rsp, 8
    IRQ_RESTORE_ALL
    add rsp, 8
    iretq

.macro IRQ_VECTOR_STUB vector
    .global irq_vector_\vector
irq_vector_\vector:
    push \vector
    jmp irq_external_common_entry
.endm

.macro IRQ_VECTOR_POINTER vector
    .quad irq_vector_\vector
.endm

.altmacro
.set irq_vector_number, 32
.rept 224
    IRQ_VECTOR_STUB %irq_vector_number
    .set irq_vector_number, irq_vector_number + 1
.endr

.align 8
irq_vector_stub_table:
.set irq_vector_number, 32
.rept 224
    IRQ_VECTOR_POINTER %irq_vector_number
    .set irq_vector_number, irq_vector_number + 1
.endr
.noaltmacro

.section .note.GNU-stack,"",@progbits
