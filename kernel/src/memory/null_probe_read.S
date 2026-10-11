.section .text
.global null_probe_read, null_probe_fault, null_probe_recovery
.type null_probe_read, @function
null_probe_read:
    xor %eax, %eax
null_probe_fault:
    movq (%rax), %rax
    ret
null_probe_recovery:
    xor %eax, %eax
    ret
.size null_probe_read, .-null_probe_read
.global null_destructive_read, null_destructive_fault
.type null_destructive_read, @function
null_destructive_read:
    xor %eax, %eax
null_destructive_fault:
    movq (%rax), %rax
    ret
.size null_destructive_read, .-null_destructive_read
.section .note.GNU-stack,"",@progbits
