#include "cmd_cpu.h"
#include <stdbool.h>
#include "../../graphics/console.h"
#include "../../libc/string.h"
#include "../../libc/memory.h"

static inline void cpuid_ex(uint32_t leaf, uint32_t subleaf,
                            uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));
    if (eax)
        *eax = a;
    if (ebx)
        *ebx = b;
    if (ecx)
        *ecx = c;
    if (edx)
        *edx = d;
}

static void print_bool_feature(const char *name, bool ok)
{
    if (!ok)
        return;
    console_write(name);
    console_write(" ");
}

int cmd_cpu(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t eax, ebx, ecx, edx;

    cpuid_ex(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_basic = eax;

    char vendor[13];
    ((uint32_t *)vendor)[0] = ebx;
    ((uint32_t *)vendor)[1] = edx;
    ((uint32_t *)vendor)[2] = ecx;
    vendor[12] = '\0';

    char brand[49];
    brand[0] = '\0';
    cpuid_ex(0x80000000u, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext = eax;
    if (max_ext >= 0x80000004u)
    {
        uint32_t *p = (uint32_t *)brand;
        cpuid_ex(0x80000002u, 0, &p[0], &p[1], &p[2], &p[3]);
        cpuid_ex(0x80000003u, 0, &p[4], &p[5], &p[6], &p[7]);
        cpuid_ex(0x80000004u, 0, &p[8], &p[9], &p[10], &p[11]);
        brand[48] = '\0';
    }

    console_set_color(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("CPU / Plataforma\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    console_write("Vendor: ");
    console_write(vendor);
    console_write("\n");

    if (brand[0])
    {
        console_write("Brand : ");
        console_write(brand);
        console_write("\n");
    }

    console_write("CPUID max basic: 0x");
    console_print_hex(max_basic);
    console_write(" | max ext: 0x");
    console_print_hex(max_ext);
    console_write("\n");

    cpuid_ex(1, 0, &eax, &ebx, &ecx, &edx);

    uint32_t stepping = (eax >> 0) & 0xF;
    uint32_t model = (eax >> 4) & 0xF;
    uint32_t family = (eax >> 8) & 0xF;
    uint32_t ext_model = (eax >> 16) & 0xF;
    uint32_t ext_family = (eax >> 20) & 0xFF;

    uint32_t disp_family = family;
    uint32_t disp_model = model;

    if (family == 0xF)
        disp_family = family + ext_family;
    if (family == 0x6 || family == 0xF)
        disp_model = (ext_model << 4) | model;

    console_write("Family: ");
    console_print_dec(disp_family);
    console_write(" | Model: ");
    console_print_dec(disp_model);
    console_write(" | Stepping: ");
    console_print_dec(stepping);
    console_write("\n");

    bool f_sse = (edx >> 25) & 1;
    bool f_sse2 = (edx >> 26) & 1;
    bool f_htt = (edx >> 28) & 1;

    bool f_sse3 = (ecx >> 0) & 1;
    bool f_ssse3 = (ecx >> 9) & 1;
    bool f_sse41 = (ecx >> 19) & 1;
    bool f_sse42 = (ecx >> 20) & 1;
    bool f_aes = (ecx >> 25) & 1;
    bool f_avx = (ecx >> 28) & 1;
    bool f_fma = (ecx >> 12) & 1;

    bool f_avx2 = false;
    bool f_bmi1 = false;
    bool f_bmi2 = false;
    if (max_basic >= 7)
    {
        uint32_t a7, b7, c7, d7;
        cpuid_ex(7, 0, &a7, &b7, &c7, &d7);
        f_bmi1 = (b7 >> 3) & 1;
        f_avx2 = (b7 >> 5) & 1;
        f_bmi2 = (b7 >> 8) & 1;
    }

    bool f_lm = false;
    bool f_1g_pages = false;
    if (max_ext >= 0x80000001u)
    {
        uint32_t ae, be, ce, de;
        cpuid_ex(0x80000001u, 0, &ae, &be, &ce, &de);
        f_lm = (de >> 29) & 1;
        f_1g_pages = (de >> 26) & 1;
    }

    console_write("Features: ");
    print_bool_feature("LM(x86_64)", f_lm);
    print_bool_feature("SSE", f_sse);
    print_bool_feature("SSE2", f_sse2);
    print_bool_feature("SSE3", f_sse3);
    print_bool_feature("SSSE3", f_ssse3);
    print_bool_feature("SSE4.1", f_sse41);
    print_bool_feature("SSE4.2", f_sse42);
    print_bool_feature("AES", f_aes);
    print_bool_feature("FMA", f_fma);
    print_bool_feature("AVX", f_avx);
    print_bool_feature("AVX2", f_avx2);
    print_bool_feature("BMI1", f_bmi1);
    print_bool_feature("BMI2", f_bmi2);
    print_bool_feature("1GPages", f_1g_pages);
    print_bool_feature("HTT", f_htt);
    console_write("\n");

    return 0;
}