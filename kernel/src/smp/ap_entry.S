.intel_syntax noprefix
.section .text

.global ap_kernel_entry_asm
.type ap_kernel_entry_asm, @function
.extern ap_kernel_entry

/*
 * The copied low-memory trampoline is a layout-stable ABI object. Finish the
 * transition in normal kernel text so its byte-for-byte contract remains
 * unchanged while C still receives a SysV call-shaped stack.
 */
ap_kernel_entry_asm:
    cld
    and rsp, -16
    sub rsp, 8

    mov rdi, rsp
    and edi, 15
    pushfq
    pop rsi
    shr rsi, 10
    and esi, 1

    xor rbp, rbp
    jmp ap_kernel_entry
.size ap_kernel_entry_asm, . - ap_kernel_entry_asm

.section .note.GNU-stack,"",@progbits
