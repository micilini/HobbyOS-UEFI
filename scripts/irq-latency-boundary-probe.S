#include "../kernel/src/core/irq_latency_tsc.h"
#include "../kernel/src/core/irq_latency_layout.h"
.text
.global irq_latency_host_save_probe
.type irq_latency_host_save_probe, @function
irq_latency_host_save_probe:
    pushq %r12
    pushq %r13
    subq $8, %rsp
    movq %rsi, %r12
    movq %rdx, %r13
    movq %rdi, irq_latency_host_if(%rip)
    pushq %rdi
    popfq
    call irq_latency_save
    movq %rax, (%r12)
    pushfq
    popq %rax
    movq %rax, (%r13)
    cld
    addq $8, %rsp
    popq %r13
    popq %r12
    ret
.size irq_latency_host_save_probe, .-irq_latency_host_save_probe

.global irq_latency_host_restore_probe
.type irq_latency_host_restore_probe, @function
irq_latency_host_restore_probe:
    pushq %rbx
    movq %rsi, %rbx
    call irq_latency_restore
    pushfq
    popq %rax
    movq %rax, (%rbx)
    cld
    popq %rbx
    ret
.size irq_latency_host_restore_probe, .-irq_latency_host_restore_probe
.intel_syntax noprefix
.global irq_latency_host_tail_probe
.type irq_latency_host_tail_probe, @function
irq_latency_host_tail_probe:
    mov rax, rdi
    mov rdx, rsi
    IRQ_LATENCY_PUBLISH_TAIL
    ret
.size irq_latency_host_tail_probe, .-irq_latency_host_tail_probe
.section .note.GNU-stack,"",@progbits
