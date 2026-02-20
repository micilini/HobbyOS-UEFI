.intel_syntax noprefix
.global switch_context
.global thread_wrapper
.global irq_timer_entry

/*
 * switch_context(prev, next)
 *   rdi = prev task_t*  (salva rsp em [rdi+0])
 *   rsi = next task_t*  (carrega rsp de [rsi+0])
 *
 * Salva/restaura apenas callee-saved (chamada voluntaria).
 */
switch_context:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp          /* prev->rsp = rsp */
    mov rsp, [rsi]          /* rsp = next->rsp */

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

/*
 * thread_wrapper - ponto de entrada para threads novas.
 * r12 = entry_point, r13 = arg (montados por thread_create na stack).
 */
thread_wrapper:
    sti
    mov rdi, r13
    call r12
    cli
    hlt
    jmp .-2

/*
 * irq_timer_entry - stub assembly para o vetor 32 (timer IRQ).
 *
 * Salva todos os registradores, chama o handler C, e retorna via iretq.
 * NAO chama schedule() — a preempcao acontece no ponto seguro dos
 * loops BSP/AP que consomem need_resched e chamam schedule_voluntary().
 */
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