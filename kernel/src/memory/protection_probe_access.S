.section .text
.global protection_probe_store, protection_store_site, protection_store_recovery
.type protection_probe_store,@function
protection_probe_store:
    xor %eax,%eax
protection_store_site:
    movb %sil,(%rdi)
    ret
protection_store_recovery:
    mov $1,%eax
    ret
.size protection_probe_store,.-protection_probe_store

.global protection_probe_fetch, protection_fetch_call, protection_fetch_recovery
.type protection_probe_fetch,@function
protection_probe_fetch:
    sub $8,%rsp
    xor %eax,%eax
protection_fetch_call:
    call *%rdi
    add $8,%rsp
    ret
/* #PF saves the data target RIP. This executable stub consumes the return
 * address pushed by the indirect call; the caller then restores its padding. */
protection_fetch_recovery:
    mov $1,%eax
    ret
.size protection_probe_fetch,.-protection_probe_fetch

.section .text.protection_target,"ax",@progbits
.balign 8
.global protection_text_target
.type protection_text_target,@object
protection_text_target:
    .quad 0x5a5a5a5a5a5a5a5a
.size protection_text_target,.-protection_text_target
.section .rodata.protection_target,"a",@progbits
.balign 8
.global protection_rodata_target
.type protection_rodata_target,@object
protection_rodata_target:
    .quad 0xa7a7a7a7a7a7a7a7
.size protection_rodata_target,.-protection_rodata_target
.section .data.protection_target,"aw",@progbits
.balign 8
.global protection_data_target
.type protection_data_target,@object
protection_data_target:
    .byte 0xc3,0,0,0,0,0,0,0
.size protection_data_target,.-protection_data_target
.section .note.GNU-stack,"",@progbits
