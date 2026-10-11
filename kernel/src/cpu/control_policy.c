#include "control_policy.h"

bool cpu_policy_pat_valid(uint64_t pat)
{
    for (unsigned slot = 0; slot < 8; ++slot) {
        unsigned type = (unsigned)((pat >> (slot * 8)) & 255);
        if (type != CPU_PAT_UC && type != CPU_PAT_WC && type != CPU_PAT_WT &&
            type != CPU_PAT_WP && type != CPU_PAT_WB && type != CPU_PAT_UCM)
            return false;
    }
    return true;
}

bool cpu_policy_preserves_slots(uint64_t before, uint64_t after, uint8_t used)
{
    if (!cpu_policy_pat_valid(before) || !cpu_policy_pat_valid(after)) return false;
    for (unsigned slot = 0; slot < 8; ++slot)
        if ((used & (1u << slot)) &&
            ((before >> (slot * 8)) & 255) != ((after >> (slot * 8)) & 255))
            return false;
    return true;
}

cpu_policy_status_t cpu_policy_build(const cpu_policy_features_t *f,
                                     cpu_policy_t *out)
{
    if (!f || !out) return CPU_POLICY_INVALID;
    if (!f->identity_valid) return CPU_POLICY_IDENTITY;
    if (!f->basic_valid || !f->extended_valid ||
        (f->basic_edx & CPU_FEATURE_REQUIRED) != CPU_FEATURE_REQUIRED ||
        !(f->extended_edx & CPU_FEATURE_LM)) return CPU_POLICY_FEATURES;
    bool nx = (f->extended_edx & CPU_FEATURE_NX) != 0;
    *out = (cpu_policy_t){.cr0_protected = CPU_CR0_PROTECTED,
        .cr0_final = CPU_CR0_FINAL, .cr4 = CPU_CR4_FINAL,
        .efer_write = CPU_EFER_LME | (nx ? CPU_EFER_NXE : 0),
        .pat = (uint64_t)CPU_PAT_LOW | ((uint64_t)CPU_PAT_HIGH << 32),
        .required_basic = CPU_FEATURE_REQUIRED,
        .required_extended = CPU_FEATURE_LM | (nx ? CPU_FEATURE_NX : 0),
        .page1g_supported = !!(f->extended_edx & CPU_FEATURE_PAGE1G)};
    return CPU_POLICY_OK;
}

cpu_policy_status_t cpu_policy_validate(const cpu_policy_t *p,
                                        const cpu_policy_features_t *f)
{
    if (!p || !f) return CPU_POLICY_INVALID;
    cpu_policy_t expected;
    cpu_policy_status_t status = cpu_policy_build(f, &expected);
    if (status != CPU_POLICY_OK) return status;
    if ((f->extended_edx & p->required_extended) != p->required_extended)
        return CPU_POLICY_FEATURES;
    if (p->cr0_protected != expected.cr0_protected || p->cr0_final != expected.cr0_final ||
        p->cr4 != expected.cr4 || p->efer_write != expected.efer_write ||
        p->pat != expected.pat || p->required_basic != expected.required_basic ||
        p->required_extended != expected.required_extended ||
        p->page1g_supported != expected.page1g_supported || p->reserved) return CPU_POLICY_INVALID;
    return CPU_POLICY_OK;
}

bool cpu_policy_bootstrap_map(const uint64_t *frames, size_t bytes,
                              uint64_t root, uint64_t cr3)
{
    /* The protected entry blob owns exactly seven contiguous static frames.
     * Check expected links before reading a child; never follow an input address. */
    if (!frames || bytes != 7u * 4096 || !root || (root & 4095) ||
        root > UINT64_C(0x100000000) - bytes || cr3 != root) return false;
    const volatile uint64_t *raw = frames;
    for (unsigned frame = 0; frame < 7; ++frame) {
        for (unsigned index = 0; index < 512; ++index) {
            uint64_t expected = 0;
            if (frame == 0 && index == 0) expected = (root + 4096) | 3;
            if (frame == 0 && index == 511) expected = (root + 8192) | 3;
            if (frame == 1 && index < 4) expected = (root + (3 + index) * 4096) | 3;
            if (frame == 2 && index == 510) expected = (root + 3 * 4096) | 3;
            if (frame >= 3) expected = ((uint64_t)(frame - 3) * 512 + index) * 0x200000 | 0x83;
            uint64_t allowed_ad = expected ? (frame >= 3 ? 0x60 : 0x20) : 0;
            if ((raw[frame * 512 + index] & ~allowed_ad) != expected) return false;
        }
    }
    return true;
}

cpu_policy_status_t cpu_policy_match(const cpu_policy_t *p,
                                     const cpu_policy_features_t *f,
                                     const cpu_policy_readback_t *a,
                                     uint64_t root, uint32_t apic)
{
    cpu_policy_status_t status = cpu_policy_validate(p, f);
    if (status != CPU_POLICY_OK) return status;
    if (!a) return CPU_POLICY_INVALID;
    if (a->apic_id != apic || f->apic_id != apic) return CPU_POLICY_IDENTITY;
    return a->cr0 == p->cr0_final && a->cr3 == root && a->cr4 == p->cr4 &&
        a->efer == (p->efer_write | CPU_EFER_LMA) && a->pat == p->pat ?
        CPU_POLICY_OK : CPU_POLICY_READBACK;
}

cpu_policy_status_t cpu_policy_publish_ready(uint8_t *publication,
                                             const cpu_policy_t *policy,
                                             const cpu_policy_features_t *features,
                                             const cpu_policy_readback_t *actual,
                                             uint64_t root, uint32_t expected_apic)
{
    if (!publication) return CPU_POLICY_INVALID;
    cpu_policy_status_t status = cpu_policy_match(policy, features, actual, root, expected_apic);
    if (status != CPU_POLICY_OK) return status;
    uint8_t unconfirmed = 0;
    return __atomic_compare_exchange_n(publication, &unconfirmed, 1, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? CPU_POLICY_OK : CPU_POLICY_DUPLICATE;
}

#ifdef HOBBYOS_CONTROL_POLICY_HOST_TEST
#define read_cr cpu_policy_test_read_cr
#define write_cr cpu_policy_test_write_cr
#define read_msr cpu_policy_test_read_msr
#define write_msr cpu_policy_test_write_msr
#define save_if cpu_policy_test_irq_save
#define restore_if cpu_policy_test_irq_restore
#define flush_cache cpu_policy_test_wbinvd
#else
#include "cpu.h"
#include "../core/idt.h"
#include "../core/panic.h"
#include "../drivers/serial.h"
#include "../libc/string.h"
#include "../smp/smp_topology.h"
#define read_msr cpu_read_msr_safe
#define write_msr cpu_write_msr_safe
#define save_if irq_save
#define restore_if irq_restore
static uint64_t read_cr(unsigned index)
{
    uint64_t value;
    if (index == 0) __asm__ volatile("mov %%cr0, %0" : "=r"(value) :: "memory");
    else if (index == 3) __asm__ volatile("mov %%cr3, %0" : "=r"(value) :: "memory");
    else __asm__ volatile("mov %%cr4, %0" : "=r"(value) :: "memory");
    return value;
}
static void write_cr(unsigned index, uint64_t value)
{
    if (index == 0) __asm__ volatile("mov %0, %%cr0" :: "r"(value) : "memory");
    else if (index == 3) __asm__ volatile("mov %0, %%cr3" :: "r"(value) : "memory");
    else __asm__ volatile("mov %0, %%cr4" :: "r"(value) : "memory");
}
static void flush_cache(void) { __asm__ volatile("wbinvd" ::: "memory"); }
#endif

static bool read_controls(const cpu_policy_features_t *features,
                           cpu_policy_readback_t *out)
{
    *out = (cpu_policy_readback_t){.cr0 = read_cr(0), .cr3 = read_cr(3),
        .cr4 = read_cr(4), .apic_id = features->apic_id};
    return read_msr(CPU_MSR_EFER, &out->efer) && read_msr(CPU_MSR_PAT, &out->pat);
}

cpu_policy_status_t cpu_policy_apply(const cpu_policy_t *p,
                                     const cpu_policy_features_t *f,
                                     uint64_t root, uint8_t used_slots,
                                     cpu_policy_readback_t *out)
{
    uint64_t flags = save_if();
    cpu_policy_status_t status = cpu_policy_validate(p, f);
    cpu_policy_readback_t before, after;
    if (status != CPU_POLICY_OK) goto done;
    if (!out) { status = CPU_POLICY_INVALID; goto done; }
    if (!read_controls(f, &before)) { status = CPU_POLICY_READ; goto done; }
    /* DE/MCE/OSFXSR/OSXMMEXCPT and PGE may be removed. PCIDE, LA57 and
     * unhandled facilities are precondition failures, not transition guesses. */
    if ((before.cr0 & (CPU_CR0_PE | CPU_CR0_PG)) != (CPU_CR0_PE | CPU_CR0_PG) ||
        !(before.cr4 & CPU_CR4_PAE) || (before.cr4 & ~UINT64_C(0x6e8)) ||
        (before.efer & (CPU_EFER_LME | CPU_EFER_LMA)) != (CPU_EFER_LME | CPU_EFER_LMA) ||
        (before.efer & ~UINT64_C(0xd01)) ||
        (!(f->extended_edx & CPU_FEATURE_NX) && (before.efer & CPU_EFER_NXE)) ||
        !root || (root & 4095) || before.cr3 != root) {
        status = CPU_POLICY_INCOMING; goto done;
    }
    if (!used_slots || (used_slots & (1u << 5)) ||
        !cpu_policy_preserves_slots(before.pat, p->pat, used_slots)) {
        status = CPU_POLICY_MAPPING; goto done;
    }
    write_cr(0, p->cr0_final | CPU_CR0_CD);
    flush_cache();
    /* Clearing inherited PGE invalidates globals; PCIDE was rejected above. */
    write_cr(4, p->cr4);
    write_cr(3, before.cr3);
    bool written = write_msr(CPU_MSR_EFER, p->efer_write);
    if (written) written = write_msr(CPU_MSR_PAT, p->pat);
    flush_cache();
    write_cr(3, before.cr3);
    write_cr(0, p->cr0_final);
    if (!written) { status = CPU_POLICY_WRITE; goto done; }
    if (!read_controls(f, &after)) { status = CPU_POLICY_READ; goto done; }
    status = cpu_policy_match(p, f, &after, root, f->apic_id);
    if (status == CPU_POLICY_OK) *out = after;
done:
    restore_if(flags);
    return status;
}

#ifndef HOBBYOS_CONTROL_POLICY_HOST_TEST
static cpu_policy_t g_policy;
static uint32_t g_bsp_identity;
static bool g_policy_published;
static uint8_t g_confirmed[HOBBYOS_MAX_CPUS];
extern uint64_t boot_pml4[];

static cpu_policy_features_t local_features(void)
{
    cpu_policy_features_t f = {0};
    uint32_t a, b, c;
    f.basic_valid = cpu_get_cpuid_count(1, 0, &a, &b, &c, &f.basic_edx);
    f.extended_valid = cpu_get_cpuid_count(0x80000001u, 0, &a, &b, &c, &f.extended_edx);
    f.identity_valid = cpu_local_arch_id(&f.apic_id);
    return f;
}

static void failure(cpu_policy_status_t status, uint32_t apic)
{
    char line[128];
    int count = ksnprintf(line, sizeof(line), "[CPU_STATE][FAIL] apic=%u reason=%u\n", apic, (unsigned)status);
    if (count > 0 && (size_t)count < sizeof(line)) serial_write_all(line);
}

static bool record(const char *stage, const char *role, uint32_t slot,
                    const cpu_policy_features_t *f, const cpu_policy_readback_t *a)
{
    char line[384];
    int count = ksnprintf(line, sizeof(line),
        "\n[CPU_STATE][%s] slot=%u apic=%u role=%s cr0=0x%016llx cr3=0x%016llx "
        "cr4=0x%016llx efer=0x%016llx pat=0x%016llx basic_edx=0x%08x extended_edx=0x%08x\n",
        stage, slot, a->apic_id, role, (unsigned long long)a->cr0,
        (unsigned long long)a->cr3, (unsigned long long)a->cr4,
        (unsigned long long)a->efer, (unsigned long long)a->pat,
        f->basic_edx, f->extended_edx);
    if (count < 0 || (size_t)count >= sizeof(line)) return false;
    serial_write_all(line);
    return true;
}

void cpu_policy_init_bsp(void)
{
    cpu_policy_features_t f = local_features();
    cpu_policy_t policy;
    cpu_policy_status_t status = cpu_policy_build(&f, &policy);
    cpu_policy_readback_t actual;
    uint64_t root = (uint64_t)boot_pml4;
    if (g_policy_published) status = CPU_POLICY_DUPLICATE;
    if (status == CPU_POLICY_OK &&
        !cpu_policy_bootstrap_map(boot_pml4, 7 * 4096, root, read_cr(3))) status = CPU_POLICY_MAPPING;
    if (status == CPU_POLICY_OK) status = cpu_policy_apply(&policy, &f, root, 1, &actual);
    if (status != CPU_POLICY_OK) {
        failure(status, f.apic_id);
        kpanic("Unsupported early CPU control policy");
    }
    g_policy = policy;
    g_bsp_identity = f.apic_id;
    __atomic_store_n(&g_policy_published, true, __ATOMIC_RELEASE);
    if (!record("EARLY", "BSP", UINT32_MAX, &f, &actual)) kpanic("CPU control record overflow");
    if (!(f.extended_edx & CPU_FEATURE_NX)) serial_write_all("[CPU_STATE][LIMIT] nx=UNSUPPORTED nxe=0\n");
}

const cpu_policy_t *cpu_policy_get(void)
{
    return __atomic_load_n(&g_policy_published, __ATOMIC_ACQUIRE) ? &g_policy : NULL;
}

bool cpu_policy_confirm_local(uint32_t expected_apic, uint64_t root, const char *role)
{
    cpu_policy_features_t f = local_features();
    cpu_policy_readback_t actual;
    cpu_slot_t slot = CPU_SLOT_INVALID;
    cpu_policy_status_t status = cpu_policy_validate(cpu_policy_get(), &f);
    if (status == CPU_POLICY_OK && (!smp_cpu_slot_from_apic_id(expected_apic, &slot) ||
        slot >= HOBBYOS_MAX_CPUS || expected_apic != f.apic_id)) status = CPU_POLICY_IDENTITY;
    bool bsp = slot == smp_bsp_cpu_slot();
    if (status == CPU_POLICY_OK && (!role || strcmp(role, bsp ? "BSP" : "AP") ||
        (bsp && f.apic_id != g_bsp_identity))) status = CPU_POLICY_IDENTITY;
    if (status == CPU_POLICY_OK && !read_controls(&f, &actual)) status = CPU_POLICY_READ;
    if (status == CPU_POLICY_OK) status = cpu_policy_publish_ready(&g_confirmed[slot],
        &g_policy, &f, &actual, root, expected_apic);
    if (status != CPU_POLICY_OK) { failure(status, f.apic_id); return false; }
    return record("READY", role, slot, &f, &actual);
}
#endif
