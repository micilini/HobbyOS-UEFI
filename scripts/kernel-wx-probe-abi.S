/* Hosted-only caller of the actual fixed probe helpers. No fault is synthesized. */
.section .text
.global protection_host_ret
.type protection_host_ret,@function
protection_host_ret:
    ret
.size protection_host_ret,.-protection_host_ret

/* (kind, target, original, output): output has fourteen uint64_t fields.
 * Preserve the host caller and seed all System V callee-saved registers. */
.global protection_host_abi
.type protection_host_abi,@function
protection_host_abi:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    sub $72,%rsp
    mov %rcx,16(%rsp)
    mov %rsi,24(%rsp)
    mov %edi,32(%rsp)
    mov %edx,36(%rsp)
    pushfq
    pop %rax
    mov %rax,40(%rsp)
    mov %rsp,48(%rsp)
    movabs $0x1020304050607080,%rbx
    movabs $0x8877665544332211,%rbp
    movabs $0x1122334455667788,%r12
    movabs $0x99aabbccddeeff00,%r13
    movabs $0x0f1e2d3c4b5a6978,%r14
    movabs $0xf0e1d2c3b4a5968,%r15
    mov %rbx,0(%rsp)
    mov %rbp,8(%rsp)
    mov 24(%rsp),%rdi
    mov 36(%rsp),%esi
    cmpl $0,32(%rsp)
    je .Lstore
    cmpl $1,32(%rsp)
    je .Lfetch
    xor %eax,%eax
    call protection_store_recovery
    jmp .Ldone
.Lstore:
    call protection_probe_store
    jmp .Ldone
.Lfetch:
    call protection_probe_fetch
.Ldone:
    pushfq
    pop %rdx
    mov 16(%rsp),%rcx
    mov %rax,0(%rcx)
    mov %rdx,8(%rcx)
    mov 48(%rsp),%rax
    mov %rax,16(%rcx)
    mov %rsp,24(%rcx)
    mov 0(%rsp),%rax
    mov %rax,32(%rcx)
    mov 8(%rsp),%rax
    mov %rax,40(%rcx)
    mov %rbx,48(%rcx)
    mov %rbp,56(%rcx)
    mov %r12,64(%rcx)
    mov %r13,72(%rcx)
    mov %r14,80(%rcx)
    mov %r15,88(%rcx)
    mov 40(%rsp),%rax
    mov %rax,96(%rcx)
    mov %rdi,104(%rcx)
    add $72,%rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.size protection_host_abi,.-protection_host_abi
.section .note.GNU-stack,"",@progbits
