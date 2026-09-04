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
    sti
    mov rdi, r13
    call r12

    call thread_exit_normal

    cli
    hlt
    jmp .-2

.section .note.GNU-stack,"",@progbits
