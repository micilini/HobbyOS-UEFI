.intel_syntax noprefix
#include "irq_latency_tsc.h"
#include "irq_latency_layout.h"
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
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    sub rsp, 16
    push rax
    push rdx
    mov qword ptr [rsp + 16], 0
    cmp dword ptr [rip + g_irq_latency_identity_ready], 0
    je .Lentry_checkpoint_unavailable
    IRQ_LATENCY_ORDERED_TSC
    shl rdx, 32
    or rax, rdx
    mov qword ptr [rsp + 16], rax
.Lentry_checkpoint_unavailable:
    pop rdx
    pop rax
#define IRQ_FRAME_EXTRA 16
#else
#define IRQ_FRAME_EXTRA 0
#endif
    IRQ_SAVE_ALL
    sub rsp, 8
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    mov qword ptr [rsp], 0
    cmp dword ptr [rip + g_irq_latency_identity_ready], 0
    je .Llatency_entry_unavailable
    IRQ_LATENCY_ORDERED_TSC
    shl rdx, 32
    or rax, rdx
    mov rdi, rax
    mov esi, dword ptr [rsp + 128 + IRQ_FRAME_EXTRA]
    mov rdx, qword ptr [rsp + 152 + IRQ_FRAME_EXTRA]
    mov rcx, qword ptr [rsp + 160 + IRQ_FRAME_EXTRA]
    cld
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    mov r8, qword ptr [rsp + 128]
    call irq_latency_interrupt_enter_checkpoint
#else
    call irq_latency_interrupt_enter
#endif
    mov qword ptr [rsp], rax
.Llatency_entry_unavailable:
#endif
    mov edi, dword ptr [rsp + 128 + IRQ_FRAME_EXTRA]
    cld
#if HOBBYOS_DEBUG_ASSERT
    pushfq
    pop rsi
    shr rsi, 10
    and esi, 1
#else
    xor esi, esi
#endif
    call irq_external_dispatch
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    mov edi, dword ptr [rsp + 128 + IRQ_FRAME_EXTRA]
    mov rsi, qword ptr [rsp + 152 + IRQ_FRAME_EXTRA]
    mov rdx, qword ptr [rsp]
    call irq_latency_interrupt_exit
    IRQ_LATENCY_PUBLISH_TAIL
#endif
    add rsp, 8
    IRQ_RESTORE_ALL
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    add rsp, IRQ_FRAME_EXTRA
#endif
    add rsp, 8
#ifdef IRQ_ENTRY_HOSTED
    push qword ptr [rsp + 16]
    popfq
    ret 32
#else
    iretq
#endif

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
