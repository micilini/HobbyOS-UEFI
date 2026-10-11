.intel_syntax noprefix
.global switch_context
.global thread_wrapper

switch_context:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    sub rsp, 40
    mov [rsp], rdi
    mov [rsp + 8], rsi
    mov [rsp + 16], rdx
    mov [rsp + 24], rcx
    call irq_latency_context_switch
    mov rdi, [rsp]
    mov rsi, [rsp + 8]
    mov rdx, [rsp + 16]
    mov rcx, [rsp + 24]
    add rsp, 40
#endif
    mov [rdi], rsp
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    mov rdi, rdx
    mov rsi, rcx
    sub rsp, 8
    call scheduler_finish_switch
    add rsp, 8
    ret

thread_wrapper:
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    call irq_latency_enable
#else
    sti
#endif
    mov rdi, r13
    call r12

    call thread_exit_normal

    cli
    hlt
    jmp .-2

.section .note.GNU-stack,"",@progbits
