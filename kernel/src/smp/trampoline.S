.intel_syntax noprefix
#include "../cpu/control_policy_bits.h"
#include "../memory/paging_boot_bits.h"
.section .smp_trampoline_blob, "ax"
.code16
.global smp_trampoline_entry
.global _trampoline_start
.global _trampoline_end

_trampoline_start:

smp_trampoline_entry:
    cli
    cld             
    
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax  
    
    mov sp, 0x1000 

    mov dx, 0x3F8
    mov al, 'R'
    out dx, al

    wbinvd

    mov di, (smp_trampoline_gdtr - smp_trampoline_entry)
    lgdt [di]

    mov eax, CPU_CR0_BOOTSTRAP
    mov cr0, eax

    mov ax, cs
    movzx eax, ax
    shl eax, 4
    add eax, (trampoline_32 - smp_trampoline_entry)
    
    push 0x00       
    push 0x08       
    push eax        
    
    .byte 0x66
    retf

.code32
trampoline_32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    
    call .get_eip
.get_eip:
    pop ebp
    sub ebp, (.get_eip - smp_trampoline_entry)
    
    mov esp, ebp
    add esp, 0x1000

    mov dx, 0x3F8
    mov al, 'P'
    out dx, al

    /* EBP remains the relocation base across each bootstrap CPUID. */
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x200000
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    test eax, 0x200000
    jz trampoline_no_cpuid
    xor eax, eax
    xor ecx, ecx
trampoline_cpuid_basic_max:
    cpuid
    cmp eax, 1
    jb trampoline_basic_feature
    mov eax, 1
    xor ecx, ecx
trampoline_cpuid_basic_features:
    cpuid
    mov esi, ecx
    and edx, CPU_FEATURE_REQUIRED
    cmp edx, CPU_FEATURE_REQUIRED
    jne trampoline_basic_feature
    cmp dword ptr [ebp + (smp_trampoline_x2apic_flag - smp_trampoline_entry)], 0
    je 2f
    test esi, CPU_FEATURE_X2APIC
    jz trampoline_x2apic_feature
2:
    mov eax, 0x80000000
    xor ecx, ecx
trampoline_cpuid_extended_max:
    cpuid
    cmp eax, 0x80000008
    jb trampoline_extended_feature
    mov eax, 0x80000001
    xor ecx, ecx
trampoline_cpuid_extended_features:
    cpuid
    test edx, CPU_FEATURE_LM
    jz trampoline_extended_feature
    mov eax, [ebp + (smp_trampoline_paging_features - smp_trampoline_entry)]
    test eax, ~CPU_FEATURE_PAGE1G
    jnz trampoline_bad_parameters
    and eax, edx
    cmp eax, [ebp + (smp_trampoline_paging_features - smp_trampoline_entry)]
    jne trampoline_extended_feature
    mov esi, CPU_EFER_LME
    test edx, CPU_FEATURE_NX
    jz 3f
    or esi, CPU_EFER_NXE
3:
    cmp esi, dword ptr [ebp + (smp_trampoline_efer - smp_trampoline_entry)]
    jne trampoline_policy_mismatch
    cmp dword ptr [ebp + (smp_trampoline_efer - smp_trampoline_entry) + 4], 0
    jne trampoline_bad_parameters
    mov eax, 0x80000008
    xor ecx, ecx
trampoline_cpuid_address_width:
    cpuid
    and eax, 255
    cmp eax, PAGING_BOOT_MAX_WIDTH
    ja trampoline_extended_feature
    mov ecx, [ebp + (smp_trampoline_physical_bits - smp_trampoline_entry)]
    cmp ecx, PAGING_BOOT_MIN_WIDTH
    jb trampoline_bad_parameters
    cmp ecx, PAGING_BOOT_MAX_WIDTH
    ja trampoline_bad_parameters
    cmp eax, ecx
    jb trampoline_extended_feature
    cmp dword ptr [ebp + (smp_trampoline_cr4 - smp_trampoline_entry)], CPU_CR4_FINAL
    jne trampoline_bad_parameters
    cmp dword ptr [ebp + (smp_trampoline_cr4 - smp_trampoline_entry) + 4], 0
    jne trampoline_bad_parameters
    cmp dword ptr [ebp + (smp_trampoline_cr0 - smp_trampoline_entry)], CPU_CR0_FINAL
    jne trampoline_bad_parameters
    cmp dword ptr [ebp + (smp_trampoline_cr0 - smp_trampoline_entry) + 4], 0
    jne trampoline_bad_parameters
    cmp dword ptr [ebp + (smp_trampoline_pat - smp_trampoline_entry)], CPU_PAT_LOW
    jne trampoline_bad_parameters
    cmp dword ptr [ebp + (smp_trampoline_pat - smp_trampoline_entry) + 4], CPU_PAT_HIGH
    jne trampoline_bad_parameters
    cmp dword ptr [ebp + (smp_trampoline_cr3 - smp_trampoline_entry) + 4], 0
    jne trampoline_bad_parameters
    mov eax, [ebp + (smp_trampoline_cr3 - smp_trampoline_entry)]
    test eax, eax
    jz trampoline_bad_parameters
    test eax, 4095
    jnz trampoline_bad_parameters

    /* Cache-disabled, paging-disabled RAM-only initialization. */
    wbinvd
    mov ecx, CPU_MSR_PAT
    mov eax, [ebp + (smp_trampoline_pat - smp_trampoline_entry)]
    mov edx, [ebp + (smp_trampoline_pat - smp_trampoline_entry) + 4]
trampoline_write_pat:
    wrmsr
    wbinvd
    mov eax, [ebp + (smp_trampoline_cr4 - smp_trampoline_entry)]
    mov cr4, eax

    mov eax, [ebp + (smp_trampoline_cr3 - smp_trampoline_entry)]
    mov cr3, eax

    mov ecx, CPU_MSR_EFER
    mov eax, [ebp + (smp_trampoline_efer - smp_trampoline_entry)]
    mov edx, [ebp + (smp_trampoline_efer - smp_trampoline_entry) + 4]
trampoline_write_efer:
    wrmsr

    mov eax, [ebp + (smp_trampoline_cr3 - smp_trampoline_entry)]
    mov cr3, eax
    mov dword ptr [ebp + (smp_trampoline_status - smp_trampoline_entry)], CPU_BOOT_APPLIED
    mov eax, [ebp + (smp_trampoline_cr0 - smp_trampoline_entry)]
    mov cr0, eax

    mov eax, ebp
    add eax, (trampoline_64 - smp_trampoline_entry)
    
    push 0x18
    push eax
    retf

trampoline_no_cpuid:
    mov eax, CPU_BOOT_NO_CPUID
    jmp trampoline_policy_failed
trampoline_basic_feature:
    mov eax, CPU_BOOT_BASIC_FEATURE
    jmp trampoline_policy_failed
trampoline_extended_feature:
    mov eax, CPU_BOOT_EXTENDED_FEATURE
    jmp trampoline_policy_failed
trampoline_policy_mismatch:
    mov eax, CPU_BOOT_POLICY_MISMATCH
    jmp trampoline_policy_failed
trampoline_x2apic_feature:
    mov eax, CPU_BOOT_X2APIC_FEATURE
    jmp trampoline_policy_failed
trampoline_bad_parameters:
    mov eax, CPU_BOOT_BAD_PARAMETERS
trampoline_policy_failed:
    mov dword ptr [ebp + (smp_trampoline_status - smp_trampoline_entry)], eax
    cli
4:  hlt
    jmp 4b

.code64
trampoline_64:
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov dx, 0x3F8
    mov al, 'L'
    out dx, al

    mov eax, dword ptr [rbp + (smp_trampoline_x2apic_flag - smp_trampoline_entry)]
    test eax, eax
    jz .skip_x2apic

    mov ecx, 0x1B
    rdmsr
    or eax, (1 << 11) | (1 << 10)
    wrmsr

.skip_x2apic:
    mov rsp, qword ptr [rbp + (smp_trampoline_stack - smp_trampoline_entry)]
    
    test rsp, rsp
    jz .bad_stack

    and rsp, -16

    mov rax, qword ptr [rbp + (smp_trampoline_entry_ptr - smp_trampoline_entry)]
    
    test rax, rax
    jz .bad_entry

    mov rbx, 0xFFFFFFFF80000000
    cmp rax, rbx
    jb .bad_kernel_ptr

    xor rbp, rbp
    jmp rax

hang:
    cli
    hlt
    jmp hang

.bad_stack:
    mov dx, 0x3F8
    mov al, 'S'     
    out dx, al
    jmp hang

.bad_entry:
    mov dx, 0x3F8
    mov al, 'Z'     
    out dx, al
    jmp hang

.bad_kernel_ptr:
    mov dx, 0x3F8
    mov al, 'K'
    out dx, al
    jmp hang

.align 16
.global smp_trampoline_gdt_start
smp_trampoline_gdt_start:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF 
    .quad 0x00CF92000000FFFF 
    .quad 0x00AF9A000000FFFF 
smp_trampoline_gdt_end:

.global smp_trampoline_gdtr
smp_trampoline_gdtr:
    .word smp_trampoline_gdt_end - smp_trampoline_gdt_start - 1
    .long 0   

.global smp_trampoline_cr3
.global smp_trampoline_cr4
.global smp_trampoline_efer
.global smp_trampoline_stack
.global smp_trampoline_entry_ptr
.global smp_trampoline_x2apic_flag
.global smp_trampoline_base
.global smp_trampoline_cr0
.global smp_trampoline_pat
.global smp_trampoline_status
.global smp_trampoline_paging_features
.global smp_trampoline_physical_bits

.align 8
smp_trampoline_cr3:           .quad 0
smp_trampoline_cr4:           .quad 0
smp_trampoline_efer:          .quad 0
smp_trampoline_cr0:           .quad 0
smp_trampoline_pat:           .quad 0
smp_trampoline_stack:         .quad 0
smp_trampoline_entry_ptr:     .quad 0
smp_trampoline_x2apic_flag:   .long 0
smp_trampoline_base:          .long 0
smp_trampoline_status:        .long CPU_BOOT_PENDING
smp_trampoline_paging_features: .long 0
smp_trampoline_physical_bits: .long 0

_trampoline_end:

.section .note.GNU-stack,"",@progbits
