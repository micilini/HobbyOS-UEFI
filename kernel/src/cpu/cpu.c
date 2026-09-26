#include "cpu.h"

#include <stddef.h>

typedef struct
{
    uint64_t instruction;
    uint64_t fixup;
} cpu_msr_fixup_entry_t;

extern const cpu_msr_fixup_entry_t _cpu_msr_fixup_start[]
    __attribute__((weak));
extern const cpu_msr_fixup_entry_t _cpu_msr_fixup_end[]
    __attribute__((weak));
extern const char _text_start[];
extern const char _text_end[];

/* Keep the architectural instruction boundary visible to the F5 opcode audit. */
static __attribute__((noinline)) void cpu_cpuid_raw(
    uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
    uint32_t *ecx, uint32_t *edx)
{
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;

    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf)
                     : "memory");
    if (eax)
        *eax = a;
    if (ebx)
        *ebx = b;
    if (ecx)
        *ecx = c;
    if (edx)
        *edx = d;
}

static void cpu_cpuid_zero(uint32_t *eax, uint32_t *ebx,
                           uint32_t *ecx, uint32_t *edx)
{
    if (eax)
        *eax = 0;
    if (ebx)
        *ebx = 0;
    if (ecx)
        *ecx = 0;
    if (edx)
        *edx = 0;
}

static bool cpu_cpuid_leaf_in_domain(uint32_t leaf)
{
    uint32_t maximum = 0;

    if (leaf < 0x40000000u)
    {
        cpu_cpuid_raw(0, 0, &maximum, NULL, NULL, NULL);
        return leaf <= maximum;
    }

    if (leaf < 0x50000000u)
    {
        uint32_t basic_maximum = 0;
        uint32_t feature_ecx = 0;
        cpu_cpuid_raw(0, 0, &basic_maximum, NULL, NULL, NULL);
        if (basic_maximum < 1u)
            return false;
        cpu_cpuid_raw(1, 0, NULL, NULL, &feature_ecx, NULL);
        if ((feature_ecx & (1u << 31)) == 0)
            return false;
        cpu_cpuid_raw(0x40000000u, 0, &maximum, NULL, NULL, NULL);
        return maximum >= 0x40000000u && maximum < 0x50000000u &&
               leaf <= maximum;
    }

    if (leaf < 0x80000000u)
        return false;

    cpu_cpuid_raw(0x80000000u, 0, &maximum, NULL, NULL, NULL);
    return maximum >= 0x80000000u && leaf <= maximum;
}

static bool cpu_cpuid_subleaf_exists(uint32_t leaf, uint32_t subleaf)
{
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;

    switch (leaf)
    {
        case 4u:
        case 0x8000001du:
            cpu_cpuid_raw(leaf, subleaf, &eax, NULL, NULL, NULL);
            return (eax & 0x1fu) != 0;
        case 7u:
            cpu_cpuid_raw(7u, 0, &eax, NULL, NULL, NULL);
            return subleaf <= eax;
        case 0xbu:
        case 0x1fu:
            cpu_cpuid_raw(leaf, subleaf, NULL, &ebx, NULL, NULL);
            return ebx != 0;
        case 0xdu:
            if (subleaf <= 1u)
                return true;
            if (subleaf >= 64u)
                return false;
            cpu_cpuid_raw(0xdu, 0, &eax, NULL, NULL, &edx);
            uint64_t user_states = ((uint64_t)edx << 32) | eax;
            cpu_cpuid_raw(0xdu, 1, NULL, NULL, &ecx, &edx);
            uint64_t supervisor_states = ((uint64_t)edx << 32) | ecx;
            return ((user_states | supervisor_states) &
                    (1ULL << subleaf)) != 0;
        case 0x14u:
        case 0x17u:
        case 0x18u:
            cpu_cpuid_raw(leaf, 0, &eax, NULL, NULL, NULL);
            return subleaf <= eax;
        default:
            return subleaf == 0;
    }
}

bool cpu_get_cpuid_count(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx)
{
    cpu_cpuid_zero(eax, ebx, ecx, edx);
    if (!cpu_cpuid_leaf_in_domain(leaf) ||
        !cpu_cpuid_subleaf_exists(leaf, subleaf))
        return false;

    cpu_cpuid_raw(leaf, subleaf, eax, ebx, ecx, edx);
    return true;
}

void cpu_get_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx)
{
    (void)cpu_get_cpuid_count(leaf, 0, eax, ebx, ecx, edx);
}

bool cpu_local_arch_id(uint32_t *cpu_id)
{
    uint32_t maximum = 0;
    uint32_t ebx = 0;
    uint32_t edx = 0;

    if (!cpu_id)
        return false;
    *cpu_id = 0;
    cpu_cpuid_raw(0, 0, &maximum, NULL, NULL, NULL);
    if (maximum >= 0x1fu)
    {
        cpu_cpuid_raw(0x1fu, 0, NULL, &ebx, NULL, &edx);
        if (ebx != 0)
        {
            *cpu_id = edx;
            return true;
        }
    }
    if (maximum >= 0xbu)
    {
        cpu_cpuid_raw(0xbu, 0, NULL, &ebx, NULL, &edx);
        if (ebx != 0)
        {
            *cpu_id = edx;
            return true;
        }
    }
    if (maximum >= 1u)
    {
        cpu_cpuid_raw(1, 0, NULL, &ebx, NULL, NULL);
        *cpu_id = ebx >> 24;
        return true;
    }
    return false;
}

uint64_t cpu_read_msr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile("rdmsr"
                     : "=a"(low), "=d"(high)
                     : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void cpu_write_msr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"(low), "d"(high)
                     : "memory");
}

bool cpu_read_msr_safe(uint32_t msr, uint64_t *out)
{
    uint32_t low;
    uint32_t high;
    uint32_t completed = 0;

    if (!out)
        return false;
    __asm__ volatile(
        ".globl cpu_read_msr_safe_site\n\t"
        ".globl cpu_read_msr_safe_fixup\n\t"
        "cpu_read_msr_safe_site:\n\t"
        "rdmsr\n\t"
        "movl $1, %2\n\t"
        "cpu_read_msr_safe_fixup:\n\t"
        ".pushsection .cpu_msr_fixup,\"a\",@progbits\n\t"
        ".balign 16\n\t"
        ".quad cpu_read_msr_safe_site, cpu_read_msr_safe_fixup\n\t"
        ".popsection\n\t"
        : "=a"(low), "=d"(high), "+r"(completed)
        : "c"(msr)
        : "memory");
    if (!completed)
        return false;
    *out = ((uint64_t)high << 32) | low;
    return true;
}

bool cpu_write_msr_safe(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    uint32_t completed = 0;

    __asm__ volatile(
        ".globl cpu_write_msr_safe_site\n\t"
        ".globl cpu_write_msr_safe_fixup\n\t"
        "cpu_write_msr_safe_site:\n\t"
        "wrmsr\n\t"
        "movl $1, %0\n\t"
        "cpu_write_msr_safe_fixup:\n\t"
        ".pushsection .cpu_msr_fixup,\"a\",@progbits\n\t"
        ".balign 16\n\t"
        ".quad cpu_write_msr_safe_site, cpu_write_msr_safe_fixup\n\t"
        ".popsection\n\t"
        : "+r"(completed)
        : "c"(msr), "a"(low), "d"(high)
        : "memory");
    return completed != 0;
}

bool cpu_msr_fixup_lookup(uint64_t instruction, uint64_t error_code,
                          uint64_t code_segment, uint64_t *fixup)
{
    uintptr_t start = (uintptr_t)_cpu_msr_fixup_start;
    uintptr_t end = (uintptr_t)_cpu_msr_fixup_end;
    uintptr_t text_start = (uintptr_t)_text_start;
    uintptr_t text_end = (uintptr_t)_text_end;

    if (!fixup || error_code != 0 || (code_segment & 3u) != 0 ||
        start == 0 || end == 0 ||
        start > end || (start % _Alignof(cpu_msr_fixup_entry_t)) != 0 ||
        (end - start) % sizeof(cpu_msr_fixup_entry_t) != 0)
        return false;

    const cpu_msr_fixup_entry_t *entries = _cpu_msr_fixup_start;
    size_t count = (end - start) / sizeof(*entries);
    bool found = false;
    uint64_t destination_found = 0;
    if (count == 0 || count > 64u)
        return false;
    for (size_t index = 0; index < count; index++)
    {
        uint64_t site = entries[index].instruction;
        uint64_t destination = entries[index].fixup;
        if (site < text_start || site >= text_end ||
            destination < text_start || destination >= text_end ||
            site == destination)
            return false;
        for (size_t prior = 0; prior < index; prior++)
            if (entries[prior].instruction == site)
                return false;
        if (site == instruction)
        {
            found = true;
            destination_found = destination;
        }
    }
    if (!found)
        return false;
    *fixup = destination_found;
    return true;
}

void init_cpu(void)
{
}
