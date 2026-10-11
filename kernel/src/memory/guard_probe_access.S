.section .text
.global guard_probe_read, guard_probe_site, guard_probe_recovery
.type guard_probe_read,@function
guard_probe_read:
guard_probe_site:
    movzbq (%rdi), %rax
    mov $0, %eax
    ret
guard_probe_recovery:
    mov $1, %eax
    ret
.size guard_probe_read,.-guard_probe_read

.global guard_payload_read
.type guard_payload_read,@function
guard_payload_read:
    movzbq (%rdi), %rax
    ret
.size guard_payload_read,.-guard_payload_read

/* Isolated nonreturning SELFTEST: bounded touches of the current real stack.
 * No scheduler wait, alternate stack, recovery arm or remote operation. */
.global guard_overflow_enter, guard_overflow_step
.type guard_overflow_enter,@function
guard_overflow_enter:
    cli
guard_overflow_step:
    sub $64, %rsp
    movq $0x51, (%rsp)
    jmp guard_overflow_step
.size guard_overflow_enter,.-guard_overflow_enter
.section .note.GNU-stack,"",@progbits
