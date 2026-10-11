#include "cmd_memory_observation.h"
#include "../../memory/paging_inspect.h"
#include "../../memory/memory_observation.h"
#include "../../memory/owned_mapping.h"
#include "../../memory/physical_translation.h"
#include "../../drivers/serial.h"
#include "../../libc/string.h"
#include "../../acpi/madt.h"
#include "../../timer/hpet.h"
#include "../../drivers/pci.h"
#include "../diagnostic_result.h"
#include <stdarg.h>

#define U64(value) ((unsigned long long)(value))
static uint64_t g_observation_id;
static memory_mtrr_observation_t g_mtrr;
static uint32_t g_memmap_owner;
extern uint64_t _kernel_start;

static bool record(const char *format, ...) __attribute__((format(printf, 1, 2)));
static bool record(const char *format, ...)
{
    char line[1200];
    va_list args;
    va_start(args, format);
    int required = kvsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (required < 0 || (size_t)required >= sizeof(line))
    {
        serial_write_all("\n[MEMORY_OBSERVATION][ERROR] reason=record_capacity\n");
        return false;
    }
    serial_write_all(line);
    return true;
}

static const char *field(const pt_entry_t *e, uint16_t bit)
{
    return !(e->applicable & bit) ? "NA" : (e->fields & bit) ? "1" : "0";
}

static int inspect_one(uint64_t id, const char *sample, const char *origin, uint64_t va)
{
    cpu_control_sample_t cpu;
    pt_result_t result;
    paging_inspect(va, &cpu, &result);
    const pt_controls_t *c = &cpu.paging;
    bool ok = record("\n[PAGE_TABLE][CONTROL] schema=1 id=%llu sample=%s origin=%s "
        "va=0x%016llx slot=%u apic=%u cr0=0x%016llx cr3=0x%016llx cr4=0x%016llx "
        "efer=0x%016llx efer_valid=%u pat=0x%016llx pat_supported=%u pat_valid=%u "
        "physical_bits=%u nx_supported=%u page1g_supported=%u\n", U64(id), sample,
        origin, U64(va), cpu.slot, cpu.apic_id, U64(c->cr0), U64(c->cr3), U64(c->cr4),
        U64(c->efer), c->efer_valid, U64(c->pat), c->pat_supported, c->pat_valid,
        c->physical_bits, c->nx_supported, c->page1g_supported);
    for (unsigned i = 0; i < result.count; i++)
    {
        const pt_entry_t *e = &result.entries[i];
        ok &= record("\n[PAGE_TABLE][ENTRY] schema=1 id=%llu sample=%s level=%u "
            "table=0x%016llx index=%u raw=0x%016llx P=%s RW=%s US=%s PWT=%s PCD=%s "
            "A=%s D=%s PS=%s PAT=%s G=%s NX=%s\n", U64(id), sample, e->level,
            U64(e->table), e->index, U64(e->raw), field(e, PT_FIELD_P), field(e, PT_FIELD_RW),
            field(e, PT_FIELD_US), field(e, PT_FIELD_PWT), field(e, PT_FIELD_PCD),
            field(e, PT_FIELD_A), field(e, PT_FIELD_D), field(e, PT_FIELD_PS),
            field(e, PT_FIELD_PAT), field(e, PT_FIELD_G), field(e, PT_FIELD_NX));
    }
    ok &= record("\n[PAGE_TABLE][RESULT] schema=1 id=%llu sample=%s va=0x%016llx "
        "root=0x%016llx status=%s reason=%s stopped=%u entries=%u leaf=%u "
        "page_size=%llu base=0x%016llx offset=0x%llx physical=0x%016llx "
        "effective_rw=%u effective_us=%u effective_nx=%u supervisor_write_protected=%u "
        "pat_index=%u pat_type_raw=%u pat_type_valid=%u pat_type=%s effective_type=NOT_COMPUTED\n",
        U64(id), sample, U64(va), U64(result.root), pt_status_name(result.status), result.reason,
        result.stopped_level, result.count, result.leaf_level, U64(result.page_size),
        U64(result.base), U64(result.offset), U64(result.physical), result.rw, result.user,
        result.nx, result.supervisor_write_protected, result.pat_index, result.pat_type,
        result.pat_type_valid, result.status != PT_MAPPED ? "NA" :
        !c->pat_valid ? "UNAVAILABLE" : pt_memory_type_name(result.pat_type));
    return ok && (result.status == PT_MAPPED || result.status == PT_NOT_PRESENT) ? 0 : 1;
}

int cmd_ptinspect(int argc, char **argv)
{
    uint64_t id = __atomic_add_fetch(&g_observation_id, 1, __ATOMIC_RELAXED), va;
    int status = 1;
    if (argc == 2 && pt_parse_address(argv[1], &va))
        status = inspect_one(id, "query", "argument", va);
    else if (argc == 2 && !strcmp(argv[1], "samples"))
    {
        cpu_control_sample_t cpu;
        pt_result_t result;
        uint64_t kernel = (uintptr_t)&_kernel_start;
        paging_inspect(kernel, &cpu, &result);
        status = inspect_one(id, "kernel", "linker_kernel_start", kernel);
        void *root_view = NULL;
        if (!physical_pointer_range(result.root, 4096, PHYSICAL_RAM, &root_view))
            return diagnostic_result_complete("ptinspect", 1);
        status |= inspect_one(id, "ram", "active_root_ram_view", (uintptr_t)root_view);
        uint64_t mmio_candidates[] = {get_ioapic_base(), get_lapic_base()};
        const char *mmio_origins[] = {"owned_madt_ioapic_base", "owned_madt_lapic_base"};
        bool mmio_found = false;
        for (unsigned i = 0; i < 2; i++)
        {
            mapping_view_t view;
            if (!mmio_candidates[i] || !owned_mapping_device_view(mmio_candidates[i], 1, &view)) continue;
            uint64_t representative = (uintptr_t)view.cpu;
            paging_inspect(representative, &cpu, &result);
            if (result.status == PT_MAPPED && result.count &&
                (result.entries[result.count - 1].fields & PT_FIELD_PCD))
            {
                status |= inspect_one(id, "mmio", mmio_origins[i], representative);
                mmio_found = true;
                break;
            }
        }
        if (!mmio_found)
        { record("\n[PAGE_TABLE][ERROR] id=%llu reason=mapped_pcd_mmio_unavailable\n", U64(id)); status = 1; }
        mapping_view_t framebuffer;
        if (!owned_mapping_device_view(memory_observation_boot()->framebuffer,
            memory_observation_boot()->framebuffer_bytes, &framebuffer)) status = 1;
        else status |= inspect_one(id, "framebuffer", "owned_BootInfo_framebuffer", (uintptr_t)framebuffer.cpu);
        bool found = false;
        for (uint64_t i = 0; i < 16; i++)
        {
            va = UINT64_C(0x4000000000) + (i << 39);
            paging_inspect(va, &cpu, &result);
            if (result.status == PT_NOT_PRESENT)
            { status |= inspect_one(id, "unmapped", "bounded_walker_search", va); found = true; break; }
        }
        if (!found) { record("\n[PAGE_TABLE][ERROR] id=%llu reason=unmapped_sample_unavailable\n", U64(id)); status = 1; }
        paging_table_count_t count;
        bool counted = paging_inspect_tables(&count);
        if (!record("\n[PAGE_TABLE][TABLES] schema=1 id=%llu point=post_smp_command "
            "root=0x%llx reachable=%u extra_roots=%u pmm_allocations=%llu "
            "initial_pmm_allocations=%llu static_frames=0 complete=%u consistent=%u "
            "epoch_before=%llu epoch_after=%llu\n", U64(id), U64(count.root),
            count.reachable, count.extra_roots, U64(count.allocations),
            U64(paging_inspect_initial_allocations()), count.complete, count.consistent,
            U64(count.epoch_before), U64(count.epoch_after)) || !counted) status = 1;
    }
    else record("\n[PAGE_TABLE][ERROR] schema=1 id=%llu reason=usage\n", U64(id));
    return diagnostic_result_complete("ptinspect", status);
}

int cmd_cpustate(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        record("\n[CPU_CONTROL][ERROR] schema=1 reason=usage\n");
        return diagnostic_result_complete("cpustate", 1);
    }
    const cpu_control_capture_t *c = cpu_control_snapshot();
    if (!c)
    {
        record("\n[CPU_CONTROL][ERROR] schema=1 reason=busy\n");
        return diagnostic_result_complete("cpustate", 1);
    }
    uint32_t uniform = 63;
    bool pat_observed = true, efer_observed = true;
    const pt_controls_t *first = 0;
    bool ok = true;
    for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
    {
        if (!(c->target_mask & (1u << slot))) continue;
        ok &= record("\n[CPU_CONTROL][TARGET] schema=1 generation=%llu slot=%u apic=%u\n",
                      U64(c->generation), slot, c->apic_ids[slot]);
        if (!(c->complete_mask & (1u << slot))) continue;
        const cpu_control_sample_t *s = &c->cpus[slot];
        const pt_controls_t *p = &s->paging;
        pat_observed = pat_observed && p->pat_valid;
        efer_observed = efer_observed && p->efer_valid;
        if (first)
        {
            if (p->cr0 != first->cr0) uniform &= ~1u;
            if (p->cr3 != first->cr3) uniform &= ~2u;
            if (p->cr4 != first->cr4) uniform &= ~4u;
            if (p->efer != first->efer || p->efer_valid != first->efer_valid) uniform &= ~8u;
            if (p->pat != first->pat || p->pat_valid != first->pat_valid ||
                p->pat_supported != first->pat_supported) uniform &= ~16u;
            if ((p->cr3 & UINT64_C(0x000ffffffffff000)) !=
                (first->cr3 & UINT64_C(0x000ffffffffff000))) uniform &= ~32u;
        }
        else first = p;
        ok &= record("\n[CPU_CONTROL][CPU] schema=1 generation=%llu slot=%u apic=%u "
            "role=%s identity_valid=%u cr_valid=1 cr0=0x%016llx cr3=0x%016llx cr4=0x%016llx "
            "efer=0x%016llx efer_valid=%u pat=0x%016llx pat_supported=%u pat_valid=%u "
            "cpuid1_edx=0x%08x extended_edx=0x%08x address_eax=0x%08x physical_bits=%u\n",
            U64(c->generation), s->slot, s->apic_id, s->is_bsp ? "BSP" : "AP", s->identity_valid,
            U64(p->cr0), U64(p->cr3), U64(p->cr4), U64(p->efer), p->efer_valid,
            U64(p->pat), p->pat_supported, p->pat_valid, s->cpuid1_edx, s->extended_edx,
            s->address_eax, p->physical_bits);
    }
    ok &= record("\n[CPU_CONTROL][SUMMARY] schema=1 generation=%llu target=0x%08x "
        "acknowledged=0x%08x errors=0x%08x missing=0x%08x collection_complete=%s "
        "controls_uniform=%s uniform_cr0=%u uniform_cr3_raw=%u uniform_cr4=%u "
        "uniform_efer=%s uniform_pat=%s uniform_cr3_root=%u apic_mode=%s reason=%s\n",
        U64(c->generation), c->target_mask, c->complete_mask, c->error_mask,
        c->target_mask & ~c->complete_mask, c->complete ? "true" : "false",
        c->complete && uniform == 63 && pat_observed && efer_observed ? "true" : "false", !!(uniform & 1),
        !!(uniform & 2), !!(uniform & 4), !efer_observed ? "NA" : (uniform & 8) ? "1" : "0",
        !pat_observed ? "NA" : (uniform & 16) ? "1" : "0",
        !!(uniform & 32), c->x2apic ? "x2apic" : "xapic", c->reason);
    int status = ok && c->complete ? 0 : 1;
    cpu_control_snapshot_release();
    return diagnostic_result_complete("cpustate", status);
}

static bool correlate(uint64_t id, const char *name, const char *origin,
                       uint64_t base, uint64_t bytes)
{
    bool valid = bytes && base <= UINT64_MAX - bytes;
    bool ok = record("\n[MEMORY_MAP][RANGE] schema=1 id=%llu name=%s origin=%s "
        "base=0x%016llx bytes=%llu end=0x%016llx valid=%u\n", U64(id), name,
        origin, U64(base), U64(bytes), U64(valid ? base + bytes : 0), valid);
    uint32_t intersections = 0;
    const memory_boot_observation_t *boot = memory_observation_boot();
    for (uint64_t i = 0; valid && i < boot->count; i++)
    {
        memory_descriptor_t d;
        uint64_t start, end;
        if (!memory_observation_descriptor(i, &d)) return false;
        if (d.range_valid && memory_interval_overlap(base, base + bytes,
                                                      d.physical, d.end, &start, &end))
        {
            intersections++;
            ok &= record("\n[MEMORY_MAP][INTERSECTION] schema=1 id=%llu name=%s "
                "descriptor=%llu start=0x%016llx end=0x%016llx\n", U64(id), name,
                U64(i), U64(start), U64(end));
        }
    }
    ok &= record("\n[MEMORY_MAP][CORRELATION] schema=1 id=%llu name=%s intersections=%u\n",
                  U64(id), name, intersections);
    for (unsigned i = 0; valid && i < g_mtrr.count; i++)
    {
        memory_mtrr_range_t r;
        const memory_mtrr_pair_t *p = &g_mtrr.pairs[i];
        memory_mtrr_decode(p->base, p->mask, g_mtrr.cpu.paging.physical_bits,
            g_mtrr.supported, p->base_valid && p->mask_valid, &r);
        uint64_t start, end;
        if (r.valid && r.enabled && memory_interval_overlap(base, base + bytes,
                                                             r.base, r.end, &start, &end))
            ok &= record("\n[MTRR][INTERSECTION] schema=1 id=%llu name=%s range=%u "
                "start=0x%016llx end=0x%016llx effective_type=NOT_COMPUTED\n",
                U64(id), name, i, U64(start), U64(end));
    }
    return ok;
}

static bool correlate_pointer(uint64_t id, const char *name, const char *origin,
                               const void *pointer, uint64_t bytes)
{
    uint64_t physical=0;
    if(bytes && !physical_address_range(pointer,bytes,&physical))return false;
    return correlate(id,name,origin,physical,bytes) && record(
        "\n[MEMORY_MAP][VIEW] schema=1 id=%llu name=%s cpu=0x%llx physical=0x%llx bytes=%llu\n",
        U64(id),name,U64((uintptr_t)pointer),U64(physical),U64(bytes));
}

static bool map_dump(uint64_t id)
{
    const memory_boot_observation_t *b = memory_observation_boot();
    bool ok = record("\n[MEMORY_MAP][HEADER] schema=1 id=%llu base=0x%016llx "
        "bytes=%llu stride=%llu count=%llu descriptor_version=NOT_IN_HANDOFF valid=%u\n",
        U64(id), U64(b->base), U64(b->bytes), U64(b->stride), U64(b->count), b->valid);
    if (!b->valid) return false;
    for (uint64_t i = 0; i < b->count; i++)
    {
        memory_descriptor_t d;
        if (!memory_observation_descriptor(i, &d)) return false;
        ok &= record("\n[MEMORY_MAP][DESCRIPTOR] schema=1 id=%llu index=%llu type=%u "
            "name=%s physical=0x%016llx virtual=0x%016llx pages=%llu end=0x%016llx "
            "attributes=0x%016llx range_valid=%u\n", U64(id), U64(i), d.type,
            memory_efi_type(d.type), U64(d.physical), U64(d.virtual_address), U64(d.pages),
            U64(d.end), U64(d.attributes), d.range_valid);
        if (!d.range_valid) ok = false;
        /* Preserve future descriptor extensions without interpreting their layout. */
        for (uint32_t offset = 40; offset < b->stride; offset += 64)
        {
            uint8_t raw[64];
            char hex[129];
            uint32_t size = b->stride - offset < 64 ? b->stride - offset : 64;
            static const char digits[] = "0123456789abcdef";
            if (!memory_observation_bytes(i, offset, raw, size)) return false;
            for (uint32_t j = 0; j < size; j++)
            { hex[j * 2] = digits[raw[j] >> 4]; hex[j * 2 + 1] = digits[raw[j] & 15]; }
            hex[size * 2] = 0;
            ok &= record("\n[MEMORY_MAP][EXTENSION] schema=1 id=%llu index=%llu offset=%u bytes=%u hex=%s\n",
                          U64(id), U64(i), offset, size, hex);
        }
    }
    memory_observation_mtrr(&g_mtrr);
    ok &= record("\n[MTRR][CONTROL] schema=1 id=%llu slot=%u apic=%u supported=%u "
        "cpuid1_edx=0x%x physical_bits=%u capability=0x%016llx capability_valid=%u "
        "defaults=0x%016llx defaults_valid=%u count=%u enabled=%u fixed_supported=%u "
        "fixed_enabled=%u default_type=%u wc_supported=%u fixed_coverage=NOT_COLLECTED effective_type=NOT_COMPUTED\n",
        U64(id), g_mtrr.cpu.slot, g_mtrr.cpu.apic_id, g_mtrr.supported,
        g_mtrr.cpu.cpuid1_edx, g_mtrr.cpu.paging.physical_bits, U64(g_mtrr.capability),
        g_mtrr.capability_valid, U64(g_mtrr.defaults), g_mtrr.defaults_valid,
        g_mtrr.count, !!(g_mtrr.defaults & 0x800), !!(g_mtrr.capability & 0x100),
        !!(g_mtrr.defaults & 0x400), (uint8_t)g_mtrr.defaults, !!(g_mtrr.capability & 0x400));
    for (unsigned i = 0; i < g_mtrr.count; i++)
    {
        const memory_mtrr_pair_t *p = &g_mtrr.pairs[i];
        memory_mtrr_range_t r;
        memory_mtrr_decode(p->base, p->mask, g_mtrr.cpu.paging.physical_bits,
                            g_mtrr.supported, p->base_valid && p->mask_valid, &r);
        ok &= record("\n[MTRR][RANGE] schema=1 id=%llu index=%u base_raw=0x%016llx "
            "mask_raw=0x%016llx base_valid=%u mask_valid=%u enabled=%u valid=%u "
            "base=0x%016llx end=0x%016llx size=%llu type=%u reason=%s\n", U64(id), i,
            U64(p->base), U64(p->mask), p->base_valid, p->mask_valid, r.enabled, r.valid,
            U64(r.base), U64(r.end), U64(r.size), r.type, r.reason);
    }
    ok &= correlate(id, "framebuffer", "BootInfo.BufferSize", b->framebuffer, b->framebuffer_bytes);
    ok &= correlate(id, "lapic", "MADT.page_extent", get_lapic_base(), 4096);
    for (uint32_t i = 0; i < madt_get_ioapic_count(); i++)
    {
        madt_ioapic_t io;
        char name[32];
        if (!madt_ioapic_at(i, &io)) return false;
        (void)ksnprintf(name, sizeof(name), "ioapic%u", i);
        ok &= correlate(id, name, "MADT.page_extent", io.mmio_base, 4096);
    }
    static const char *const signatures[] = {"APIC", "FACP", "HPET", "MCFG", "WAET"};
    for (unsigned i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++)
    {
        uint64_t physical;
        const AcpiSdtHeader *table = acpi_retained_table(signatures[i], &physical);
        if (!table) { ok &= correlate(id, signatures[i], "acpi_retained_table.unavailable", 0, 0); continue; }
        ok &= correlate_pointer(id, signatures[i], "acpi_retained_table.header_length", table, table->length);
        if (!strcmp(signatures[i], "HPET") && table->length >= sizeof(HpetTable))
        {
            const HpetTable *hpet = (const HpetTable *)table;
            if (hpet->address_space_id == 0)
                ok &= correlate(id, "hpet", "HPET.GAS.page_extent", hpet->address, 4096);
        }
        if (!strcmp(signatures[i], "MCFG") && table->length >= sizeof(McfgHeader))
        {
            uint32_t count = (table->length - sizeof(McfgHeader)) / sizeof(McfgDeviceConfig);
            if (count > 256) return false;
            const McfgDeviceConfig *configs = (const McfgDeviceConfig *)
                ((const uint8_t *)table + sizeof(McfgHeader));
            for (uint32_t j = 0; j < count; j++)
            {
                char name[32]; (void)ksnprintf(name, sizeof(name), "ecam%u", j);
                uint64_t bytes = configs[j].EndBus >= configs[j].StartBus ?
                    ((uint64_t)configs[j].EndBus - configs[j].StartBus + 1) << 20 : 0;
                ok &= correlate(id, name, "MCFG.bus_span", configs[j].BaseAddress, bytes);
            }
        }
    }
    uint64_t dsdt_physical;
    const AcpiSdtHeader *dsdt = acpi_retained_table("DSDT", &dsdt_physical);
    ok &= correlate_pointer(id, "DSDT", "acpi_retained_table.header_length", dsdt,
                     dsdt ? dsdt->length : 0);
    static const char *const roots[] = {"RSDP", "XSDT", "RSDT"};
    for (unsigned i = 0; i < 3; i++)
    {
        const void *address;
        uint32_t bytes;
        bool available = acpi_observe_root(i, &address, &bytes);
        ok &= correlate_pointer(id, roots[i], "acpi_observe_root.retained_metadata",
                         available ? address : NULL, available ? bytes : 0);
    }
    return ok;
}

int cmd_memmap(int argc, char **argv)
{
    (void)argv;
    uint32_t expected = 0;
    if (argc != 1 || !__atomic_compare_exchange_n(&g_memmap_owner, &expected, 1, false,
                                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        record("\n[MEMORY_MAP][ERROR] schema=1 reason=usage_or_busy\n");
        return diagnostic_result_complete("memmap", 1);
    }
    uint64_t id = __atomic_add_fetch(&g_observation_id, 1, __ATOMIC_RELAXED);
    bool ok = map_dump(id);
    ok &= record("\n[MEMORY_MAP][RESULT] schema=1 id=%llu complete=%u "
                  "effective_type=NOT_COMPUTED\n", U64(id), ok);
    __atomic_store_n(&g_memmap_owner, 0, __ATOMIC_RELEASE);
    return diagnostic_result_complete("memmap", ok ? 0 : 1);
}
