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
    /* Thread retornou — faz exit limpo */
    call thread_exit
    /* thread_exit nunca retorna, mas por segurança: */
    cli
    hlt
    jmp .-2

/*
 * irq_timer_entry - stub assembly para o vetor 32 (timer IRQ).
 *
 * Salva todos os registradores, chama o handler C, e depois tenta
 * preemptar chamando scheduler_preempt_from_irq().
 * Se preemptar, switch_context salva/restaura RSP que inclui este
 * frame inteiro. Quando a task voltar, retorna aqui e faz iretq.
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

    /* Chama o handler C (EOI, timer_handler, etc) */
    call irq_timer_handler_inner

    /* Tenta preempcao: esta funcao chama schedule() internamente
     * se need_resched estiver setado e o quantum tiver esgotado.
     * Se fizer switch_context, o RSP atual (com todo este frame)
     * eh salvo em prev->rsp. Quando a task for re-escalonada,
     * switch_context restaura RSP, retorna aqui, e faz iretq. */
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