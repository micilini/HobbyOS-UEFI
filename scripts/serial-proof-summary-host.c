#include <assert.h>
#include <stdio.h>
#include "../kernel/src/memory/irq_latency_test.c"
uint32_t g_cpu_count=2;
static bool enabled=true,available=true;
static uint64_t invalid_boundaries;
static irq_latency_snapshot_t snapshots[2];
bool irq_is_enabled(void){return enabled;}
void irq_latency_boundary_counts(uint64_t *early,uint64_t *invalid){*early=0;*invalid=invalid_boundaries;}
bool irq_latency_terminal_read(uint32_t cpu,const irq_latency_snapshot_t **out,
    const irq_latency_witness_t **failures,uint64_t *cutoff,uint64_t *cost)
{assert(cpu<2);*out=&snapshots[cpu];*failures=0;*cutoff=100000;*cost=10;return available;}
int main(void)
{
    irq_latency_test_summary_t result;
    assert(!irq_latency_test_summary(0));
    assert(!irq_latency_test_summary(&result) && !result.cells);
    g_terminal_complete=g_audit_reported=true;
    for(unsigned role=0;role<2;++role)g_calibration[role].joined=1;
    for(unsigned cpu=0;cpu<2;++cpu){
        g_calibration[0].timebase[cpu].conversion_hz=1000000000;
        for(unsigned path=0;path<IRQ_LATENCY_PATHS;++path)
            snapshots[cpu].path[path]=(irq_latency_path_record_t){.count=1,.maximum_cycles=49999};
    }
    assert(irq_latency_test_summary(&result) && result.cells==22 && !result.missing &&
        !result.invalid && !result.unqualified && !result.over_budget && result.maximum_ns==49999);
    snapshots[1].path[9].maximum_cycles=50000;
    assert(irq_latency_test_summary(&result) && result.over_budget==1 && result.maximum_ns==50000 &&
        result.maximum_cpu==1 && result.maximum_path==9);
    snapshots[1].path[9].maximum_cycles=50001;
    assert(irq_latency_test_summary(&result) && result.over_budget==1 && result.maximum_ns==50001);
    snapshots[1].path[9].count=0;
    assert(irq_latency_test_summary(&result) && result.missing==1);
    snapshots[1].path[9].count=1;snapshots[1].path[9].invalid=1;
    assert(irq_latency_test_summary(&result) && result.missing==1);
    snapshots[1].path[9].invalid=0;snapshots[1].path[9].maximum_cycles=0;
    assert(irq_latency_test_summary(&result) && result.missing==1);
    snapshots[1].path[9].maximum_cycles=49999;
    g_calibration[1].observer_errors[1]=4;
    assert(irq_latency_test_summary(&result) && result.unqualified==11);
    g_calibration[1].observer_errors[1]=0;g_calibration[1].clock_errors[0]=1;
    assert(irq_latency_test_summary(&result) && result.unqualified==11);
    g_calibration[1].clock_errors[0]=0;
    snapshots[0].invalid=1;invalid_boundaries=1;
    assert(irq_latency_test_summary(&result) && result.invalid==2);
    snapshots[0].invalid=0;invalid_boundaries=0;
    g_calibration[0].timebase[1].conversion_hz=0;
    assert(irq_latency_test_summary(&result) && result.missing==11);
    g_calibration[0].timebase[1].conversion_hz=3;
    snapshots[1].path[9].maximum_cycles=UINT64_MAX;
    assert(irq_latency_test_summary(&result) && result.missing==1);
    enabled=false;assert(!irq_latency_test_summary(&result) && !enabled);enabled=true;
    available=false;assert(!irq_latency_test_summary(&result));available=true;
    g_cpu_count=0;assert(!irq_latency_test_summary(&result));
    g_cpu_count=HOBBYOS_MAX_CPUS+1;assert(!irq_latency_test_summary(&result));
    puts("Actual frozen measurement summary: PASS strict_budget missing invalid unqualified overflow context");
}
