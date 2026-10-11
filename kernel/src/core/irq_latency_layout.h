#ifndef HOBBYOS_IRQ_LATENCY_LAYOUT_H
#define HOBBYOS_IRQ_LATENCY_LAYOUT_H
#define IRQ_TAIL_END_OFFSET 16
#define IRQ_TAIL_OBSERVED_END_OFFSET 144
#ifdef __ASSEMBLER__
.macro IRQ_LATENCY_PUBLISH_TAIL
    mov r8, rax
    mov r9, rdx
    test r8, r8
    jz .Llatency_tail_unavailable\@
    IRQ_LATENCY_ORDERED_TSC
    shl rdx, 32
    or rax, rdx
    mov qword ptr [r8 + IRQ_TAIL_END_OFFSET], rax
    mov qword ptr [r8 + IRQ_TAIL_OBSERVED_END_OFFSET], rax
    sub qword ptr [r8], 1
    test r9, r9
    jz .Llatency_tail_unavailable\@
    mov qword ptr [r9], rax
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    /* The C-selected probe pointer owns two cells. All GPRs remain saved;
     * the original end and READY publication above are unchanged. */
    IRQ_LATENCY_ORDERED_TSC
    shl rdx, 32
    or rax, rdx
    mov qword ptr [r9 + 8], rax
#endif
.Llatency_tail_unavailable\@:
.endm
#endif
#endif
