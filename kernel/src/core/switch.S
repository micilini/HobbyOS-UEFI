.intel_syntax noprefix
.global switch_context
.global thread_wrapper
.global irq_timer_entry

switch_context:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

thread_wrapper:
    sti
    mov rdi, r13
    call r12

    call thread_exit

    cli
    hlt
    jmp .-2

irq_timer_entry:
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

    call irq_timer_handler_inner

    call scheduler_preempt_from_irq

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

    iretq

.section .note.GNU-stack,"",@progbits