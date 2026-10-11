.text
.global stack_guard_host_abi
.type stack_guard_host_abi,@function
stack_guard_host_abi:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    sub $72,%rsp
    mov %rdi,0(%rsp)
    mov %rsi,8(%rsp)
    mov %rdx,16(%rsp)
    movabs $0x1020304050607080,%rbx
    movabs $0x8877665544332211,%rbp
    movabs $0x1122334455667788,%r12
    movabs $0x99aabbccddeeff00,%r13
    movabs $0x0f1e2d3c4b5a6978,%r14
    movabs $0xf0e1d2c3b4a5968,%r15
    mov %rbx,24(%rsp)
    mov %rbp,32(%rsp)
    mov %rsp,0(%rdx)
    mov %rsi,%rdi
    xor %eax,%eax
    pushfq
    pop %rcx
    mov %rcx,16(%rdx)
    call *0(%rsp)
    mov 16(%rsp),%rdx
    mov %rax,32(%rdx)
    mov %rsp,8(%rdx)
    pushfq
    pop %rax
    mov %rax,24(%rdx)
    mov 24(%rsp),%rax
    mov %rax,40(%rdx)
    mov 32(%rsp),%rax
    mov %rax,48(%rdx)
    mov %rbx,56(%rdx)
    mov %rbp,64(%rdx)
    mov %r12,72(%rdx)
    mov %r13,80(%rdx)
    mov %r14,88(%rdx)
    mov %r15,96(%rdx)
    add $72,%rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.size stack_guard_host_abi,.-stack_guard_host_abi
.section .note.GNU-stack,"",@progbits
