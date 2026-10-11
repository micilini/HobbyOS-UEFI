#include "../kernel/src/core/irq_latency_calibration.h"
#include "../kernel/src/core/irq_latency.h"
#include "../kernel/src/core/idt.h"
#include "../kernel/src/cpu/cpu.h"
#include "../kernel/src/timer/hpet.h"
#include <assert.h>
#include <stdio.h>

static bool initialized = true, enabled = true, tsc = true, migrate, sample_valid = true;
static bool intel=true, sse2=true;
static uint64_t ticks = 1000, increment = 100, sequence;
static unsigned reads, timestamps;
static uint32_t mode = HPET_COUNTER_READ_SPLIT64_STABLE;

uint64_t irq_latency_host_read_tsc(void);
uint64_t irq_latency_host_read_tsc(void) { ++timestamps; return ticks * 240; }
bool irq_is_enabled(void) { return enabled; }
bool hpet_is_available(void) { return initialized; }
uint64_t hpet_period_fs(void) { return UINT64_C(100000000); }
bool hpet_read_counter_sample(hpet_counter_sample_t *out)
{
    assert(initialized); ++reads; ticks += increment;
    if (migrate) ++sequence;
    *out = (hpet_counter_sample_t){ticks, 0, mode, sample_valid};
    return sample_valid;
}
bool irq_latency_identity_prepare(void) { return tsc && initialized && enabled; }
bool irq_latency_identity_sample(uint32_t *cpu, uint64_t *switch_sequence)
{ *cpu = 0; *switch_sequence = sequence; return true; }
bool cpu_get_cpuid_count(uint32_t leaf, uint32_t subleaf,
    uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    assert(!subleaf); *a = *b = *c = *d = 0;
    switch (leaf) {
    case 0: *a = 0x15; *b=intel ? 0x756e6547u : 0; *d=0x49656e69u; *c=0x6c65746eu; break;
    case 1: *d = (tsc ? 1u << 4 : 0) | (sse2 ? 1u << 26 : 0); break;
    case 0x15: *a = 1; *b = 100; *c = 24000000; break;
    case 0x80000000: *a = 0x80000007; break;
    case 0x80000001: *d = 1u << 27; break;
    case 0x80000007: *d = 1u << 8; break;
    default: return false;
    }
    return true;
}

int main(void)
{
    irq_latency_capabilities_t caps;
    irq_latency_calibration_window_t windows[IRQ_LATENCY_CALIBRATION_WINDOWS];
    irq_latency_timebase_t timebase;
    assert(irq_latency_read_capabilities(&caps));
#if defined(HOBBYOS_IRQ_LOCAL_TSC_ORDER) && HOBBYOS_IRQ_LOCAL_TSC_ORDER
    intel=false;assert(!irq_latency_read_capabilities(&caps));intel=true;
    sse2=false;assert(!irq_latency_read_capabilities(&caps));sse2=true;
    assert(irq_latency_read_capabilities(&caps));
#endif
    assert(caps.extended1_edx == 1u<<27 && caps.extended7_edx == 1u<<8);
    for (unsigned i = 0; i < IRQ_LATENCY_CALIBRATION_WINDOWS; ++i) {
        assert(irq_latency_reference_window(&windows[i]));
        ticks += 1000;
    }
    assert(irq_latency_calibration_select(windows, &caps, &timebase));
    assert(timebase.declared_hz == UINT64_C(2400000000));
    irq_latency_reference_point_t point;
    unsigned previous_reads = reads, previous_timestamps = timestamps;
    initialized = false;
    assert(!irq_latency_reference_point(&point));
    assert(!irq_latency_reference_window(&windows[0]));
    assert(reads == previous_reads && timestamps == previous_timestamps);
    initialized = true; enabled = false;
    assert(!irq_latency_reference_point(&point));
    assert(!irq_latency_reference_window(&windows[0]));
    assert(reads == previous_reads && timestamps == previous_timestamps);
    enabled = true; tsc = false;
    assert(!irq_latency_reference_window(&windows[0]));
    assert(reads == previous_reads && timestamps == previous_timestamps);
    tsc = true; migrate = true;
    assert(!irq_latency_reference_point(&point) && point.switch_sequence != point.switch_after);
    migrate = false; sample_valid = false;
    assert(!irq_latency_reference_point(&point));
    sample_valid = true; mode = HPET_COUNTER_READ_NONE;
    assert(!irq_latency_reference_point(&point));
    mode = HPET_COUNTER_READ_EXTENDED32; ticks = UINT32_MAX-50;
    assert(irq_latency_reference_point(&point) && point.ticks > UINT32_MAX && point.counter_bits == 32);
    increment = 0; previous_reads = reads;
    assert(!irq_latency_reference_window(&windows[0]));
    assert(reads-previous_reads == IRQ_LATENCY_REFERENCE_POLL_LIMIT + 1);
    increment=100;mode=HPET_COUNTER_READ_SPLIT64_STABLE;
    assert(irq_latency_reference_acquire(&windows[0])==1);
    assert(windows[0].begin.acquired && windows[0].end.acquired && enabled);
    migrate=true;
    assert(irq_latency_reference_acquire(&windows[0])==2);
    assert(windows[0].begin.acquired && windows[0].end.acquired && !windows[0].begin.valid && !windows[0].end.valid);
    migrate=false;increment=0;
    assert(irq_latency_reference_acquire(&windows[0])==4 && windows[0].end.acquired);
    puts("IRQ latency actual-C capability and HPET acquisition: PASS clocks=HOSTED");
    return 0;
}
