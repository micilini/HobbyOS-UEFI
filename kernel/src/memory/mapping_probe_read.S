.section .text
.global mapping_probe_read
.global mapping_probe_fault
.global mapping_probe_recovery
.type mapping_probe_read, @function
mapping_probe_read:
mapping_probe_fault:
    movq (%rdi), %rax
    ret
mapping_probe_recovery:
    xor %eax, %eax
    ret
.size mapping_probe_read, .-mapping_probe_read
.section .note.GNU-stack,"",@progbits
