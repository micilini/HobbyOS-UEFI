#include "irq_latency_tsc.h"
.text

.macro MASK_IRQ
#ifdef IRQ_LATENCY_HOSTED_BOUNDARY
    movq $0, irq_latency_host_if(%rip)
#else
    cli
#endif
.endm

.macro RESTORE_FLAGS
#ifdef IRQ_LATENCY_HOSTED_BOUNDARY
    popq %r11
    movq %r11, irq_latency_host_if(%rip)
    pushq %r11
#endif
    popfq
.endm

/* The layout is asserted by the C adapter. Entry RFLAGS is captured before
 * any flag-changing observer instruction. The C observer always sees DF=0. */
.macro SAVE_BOUNDARY
    pushfq
    cld
    subq $96, %rsp
    movq 96(%rsp), %rax
#ifdef IRQ_LATENCY_HOSTED_BOUNDARY
    andq $-513, %rax
    movq irq_latency_host_if(%rip), %r11
    andq $512, %r11
    orq %r11, %rax
#endif
    movq %rax, 80(%rsp)
    movq 104(%rsp), %rax
    movq %rax, 8(%rsp)
    /* Use the live return-address slot, not the caller's pre-CALL RSP.
     * A newly entered thread wrapper has pre-CALL RSP exactly at payload end. */
    leaq 104(%rsp), %rax
    movq %rax, 16(%rsp)
.endm

.macro ORDERED_TSC
    IRQ_LATENCY_ORDERED_TSC
.endm

.macro STORE_TSC
    shlq $32, %rdx
    orq %rdx, %rax
    movq %rax, 24(%rsp)
.endm

.global irq_latency_save
.type irq_latency_save, @function
irq_latency_save:
    SAVE_BOUNDARY
    movq 80(%rsp), %rax
    movq %rax, 0(%rsp)
    movq %rsp, %rdi
    call irq_latency_before_mask
    testl %eax, %eax
    jz .Lsave_unavailable
    ORDERED_TSC
    MASK_IRQ
    STORE_TSC
    movq %rsp, %rdi
    call irq_latency_after_mask
    jmp .Lsave_return
.Lsave_unavailable:
    MASK_IRQ
.Lsave_return:
    movq 0(%rsp), %rax
    movq %rax, %r11
    andq $-513, %r11
    leaq 104(%rsp), %rsp
    pushq %r11
    RESTORE_FLAGS
    ret
.size irq_latency_save, .-irq_latency_save

.global irq_latency_restore
.type irq_latency_restore, @function
irq_latency_restore:
    SAVE_BOUNDARY
    movq %rdi, 0(%rsp)
    movq %rsp, %rdi
    call irq_latency_before_restore
    testl %eax, %eax
    jz .Lrestore_return
    movq 0(%rsp), %r11
    pushq %r11
    RESTORE_FLAGS
    ORDERED_TSC
    STORE_TSC
    cld
    movq %rsp, %rdi
    call irq_latency_after_restore
.Lrestore_return:
    movq 0(%rsp), %r11
    leaq 104(%rsp), %rsp
    pushq %r11
    RESTORE_FLAGS
    ret
.size irq_latency_restore, .-irq_latency_restore

.global irq_latency_enable
.type irq_latency_enable, @function
irq_latency_enable:
    pushfq
    popq %rdi
    orq $512, %rdi
    jmp irq_latency_restore
.size irq_latency_enable, .-irq_latency_enable

.global irq_latency_disable
.type irq_latency_disable, @function
irq_latency_disable:
    jmp irq_latency_save
.size irq_latency_disable, .-irq_latency_disable

#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
.section .text.irq_latency_writeback,"ax",@progbits
.global irq_latency_writeback_execute
.type irq_latency_writeback_execute, @function
irq_latency_writeback_execute:
    pushfq
    popq %r8
    ORDERED_TSC
    shlq $32, %rdx
    orq %rdx, %rax
    movq %rax, %r9
#ifdef IRQ_LATENCY_HOSTED_WRITEBACK
    /* Hosted ABI/ordering test only; never a privileged execution claim. */
    nop
#else
    wbinvd
#endif
    ORDERED_TSC
    shlq $32, %rdx
    orq %rdx, %rax
    pushfq
    popq %r10
    movq %r9, 0(%rdi)
    movq %rax, 8(%rdi)
    movq %r8, 16(%rdi)
    movq %r10, 24(%rdi)
    ret
.size irq_latency_writeback_execute, .-irq_latency_writeback_execute
#endif

.section .note.GNU-stack,"",@progbits
