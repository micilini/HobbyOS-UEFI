#include "image_permissions.h"

extern char _text_start[], _text_end[], _trampoline_start[], _trampoline_end[];
extern char _rodata_start[], _rodata_end[], _cpu_msr_fixup_start[], _cpu_msr_fixup_end[];
extern char _data_start[], _data_end[], _bss_start[], _bss_end[];
extern char _boot_text_start[], _boot_text_end[], _boot_data_start[], _boot_data_end[], _phys_end_boot[];
extern char bsp_stack_guard_start[], bsp_stack_guard_end[], bsp_stack_bottom[], bsp_stack_top[];

bool image_permissions_from_linker(image_permissions_t *out, const physical_contract_t *c)
{
#define IMAGE_SECTION(a,b,r) {(uintptr_t)(a)-KERNEL_IMAGE_OFFSET,(uintptr_t)(b)-KERNEL_IMAGE_OFFSET,r}
    const image_permission_input_t input={
        .sections={IMAGE_SECTION(_text_start,_text_end,IMAGE_TEXT),
            IMAGE_SECTION(_trampoline_start,_trampoline_end,IMAGE_TRAMPOLINE),
            IMAGE_SECTION(_rodata_start,_rodata_end,IMAGE_RODATA),
            IMAGE_SECTION(_cpu_msr_fixup_start,_cpu_msr_fixup_end,IMAGE_MSR_FIXUP),
            IMAGE_SECTION(_data_start,_data_end,IMAGE_DATA),
            IMAGE_SECTION(_bss_start,_bss_end,IMAGE_BSS),
            {(uintptr_t)_boot_text_start,(uintptr_t)_boot_text_end,IMAGE_BOOT_TEXT},
            {(uintptr_t)_boot_data_start,(uintptr_t)_boot_data_end,IMAGE_BOOT_DATA}},
        .boot_first=(uintptr_t)_boot_text_start, .boot_end=(uintptr_t)_phys_end_boot,
        .bsp_guard_first=(uintptr_t)bsp_stack_guard_start-KERNEL_IMAGE_OFFSET,
        .bsp_guard_end=(uintptr_t)bsp_stack_guard_end-KERNEL_IMAGE_OFFSET,
        .bsp_stack_first=(uintptr_t)bsp_stack_bottom-KERNEL_IMAGE_OFFSET,
        .bsp_stack_end=(uintptr_t)bsp_stack_top-KERNEL_IMAGE_OFFSET};
#undef IMAGE_SECTION
    return image_permissions_create(out,c,&input);
}
