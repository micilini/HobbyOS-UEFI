.intel_syntax noprefix
.text
.global irq_entry_host_probe
.type irq_entry_host_probe,@function
irq_entry_host_probe:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov [rip + irq_entry_host_saved_rsp], rsp
    push 0x23
    push rsp
    push 0x602
    push 0x33
    lea rax, [rip + .Lreturned]
    push rax
    mov rax, 1
    mov rcx, 2
    mov rdx, 3
    mov rbx, 4
    mov rbp, 5
    mov rsi, 6
    mov rdi, 7
    mov r8, 8
    mov r9, 9
    mov r10, 10
    mov r11, 11
    mov r12, 12
    mov r13, 13
    mov r14, 14
    mov r15, 15
    std
    jmp irq_vector_255
.Lreturned:
    pushfq
    pop qword ptr [rip + irq_entry_host_flags]
    cld
    mov [rip + irq_entry_host_values + 0], rax
    mov [rip + irq_entry_host_values + 8], rcx
    mov [rip + irq_entry_host_values + 16], rdx
    mov [rip + irq_entry_host_values + 24], rbx
    mov [rip + irq_entry_host_values + 32], rbp
    mov [rip + irq_entry_host_values + 40], rsi
    mov [rip + irq_entry_host_values + 48], rdi
    mov [rip + irq_entry_host_values + 56], r8
    mov [rip + irq_entry_host_values + 64], r9
    mov [rip + irq_entry_host_values + 72], r10
    mov [rip + irq_entry_host_values + 80], r11
    mov [rip + irq_entry_host_values + 88], r12
    mov [rip + irq_entry_host_values + 96], r13
    mov [rip + irq_entry_host_values + 104], r14
    mov [rip + irq_entry_host_values + 112], r15
    cmp rsp, [rip + irq_entry_host_saved_rsp]
    sete al
    movzx eax, al
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.size irq_entry_host_probe,.-irq_entry_host_probe
.section .note.GNU-stack,"",@progbits
