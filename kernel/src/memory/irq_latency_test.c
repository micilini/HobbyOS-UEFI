#include "../core/irq_latency_interior.h"
#include "../core/irq_latency_heap.h"
#include "irq_latency_test.h"
#include "../core/clocksource.h"
#include "irq_latency_stress.h"
#include "tlb_fixture.h"
#include "stack_report.h"
#include "warning_report.h"
#include "guard_probe.h"
#include "../core/irq_latency.h"
#include "../core/irq_latency_calibration.h"
#include "../core/irq_latency_pairs.h"
#include "../core/irq_latency_tsc.h"
#include "../core/idt.h"
#include "../core/scheduler.h"
#include "../drivers/timer.h"
#include "../smp/smp_topology.h"
#include "../smp/cpu_request.h"
#include "paging.h"
#include "../libc/memory.h"
#include "../libc/string.h"

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
#define latency_report stack_test_emit_checked
#else
#define latency_report stack_test_emit
#endif

#define CAL_WINDOWS_PER_CPU 64u
#define CAL_PROBES_PER_WINDOW 4u
#define CAL_MIN_PROBES 32u
#define CAL_WORKERS HOBBYOS_MAX_CPUS
#define CAL_WINDOWS (CAL_WINDOWS_PER_CPU * HOBBYOS_MAX_CPUS)
#define CAL_DEADLINE_MS UINT64_C(120000)
enum {
    CAL_CLOCK_CAPS = 1u, CAL_CLOCK_WINDOWS = 2u, CAL_CLOCK_SELECTION = 4u,
    CAL_CLOCK_INTERVAL = 8u, CAL_CLOCK_DRIFT = 16u,
    CAL_OBSERVER_COVERAGE = 1u, CAL_OBSERVER_RANGE = 2u, CAL_OBSERVER_DRIFT = 4u
};
#define U64(value) ((unsigned long long)(value))

typedef struct {
    irq_latency_calibration_window_t window;
    irq_latency_irq_probe_t probe[CAL_PROBES_PER_WINDOW];
    uint32_t worker, valid, complete, acquisition;
} calibration_attempt_t;
typedef struct {
    calibration_attempt_t attempt[CAL_WINDOWS];
    irq_latency_capabilities_t caps[HOBBYOS_MAX_CPUS];
    uint32_t caps_seen[HOBBYOS_MAX_CPUS];
    irq_latency_timebase_t timebase[HOBBYOS_MAX_CPUS];
    uint32_t selected[HOBBYOS_MAX_CPUS][IRQ_LATENCY_CALIBRATION_WINDOWS];
    uint32_t valid_windows[HOBBYOS_MAX_CPUS], valid_probes[HOBBYOS_MAX_CPUS];
    uint64_t overhead[HOBBYOS_MAX_CPUS];
    uint32_t clock_errors[HOBBYOS_MAX_CPUS], observer_errors[HOBBYOS_MAX_CPUS];
    irq_latency_point_ref_t points[2 * CAL_WINDOWS];
    irq_latency_point_disposition_t disposition[2 * CAL_WINDOWS];
    irq_latency_pair_t pairs[CAL_WINDOWS];
    uint32_t order[2 * CAL_WINDOWS], pair_count, pairing_complete;
    uint32_t next, stop, failed, workers, limit, joined;
} calibration_series_t;
typedef struct { task_handle_t handle; uint32_t series, index, done; } calibration_worker_t;
static calibration_series_t g_calibration[2];
static calibration_worker_t g_calibration_workers[2][CAL_WORKERS];
static uint32_t g_latency_test_state;
static bool g_terminal_complete, g_audit_reported;

bool irq_latency_test_reporting(void)
{
    return __atomic_load_n(&g_latency_test_state, __ATOMIC_ACQUIRE) == 3;
}

static bool service_cutoff(const char *kind)
{
    uint64_t admitted, completed;
    if (!cpu_request_audit_snapshot(&admitted, &completed)) return false;
    return latency_report("[IRQ_LATENCY][%s] epoch=1 admitted=%llu completed=%llu cpus=%u joined=%u\n",
        kind, U64(admitted), U64(completed), g_cpu_count, g_calibration[1].joined);
}

static void calibration_worker(void *argument)
{
    calibration_worker_t *worker = argument;
    calibration_series_t *series = &g_calibration[worker->series];
    while (!__atomic_load_n(&series->stop, __ATOMIC_ACQUIRE)) {
        uint32_t index = __atomic_fetch_add(&series->next, 1, __ATOMIC_RELAXED);
        if (index >= series->limit) break;
        calibration_attempt_t *attempt = &series->attempt[index];
        attempt->worker = worker->index;
        uint64_t flags = irq_save(), switches;
        uint32_t cpu;
        irq_latency_capabilities_t caps;
        bool valid = irq_latency_identity_sample(&cpu, &switches) && cpu < g_cpu_count &&
            irq_latency_read_capabilities(&caps);
        if (valid) {
            if (series->caps_seen[cpu] && memcmp(&series->caps[cpu], &caps, sizeof(caps)))
                __atomic_store_n(&series->failed, 1, __ATOMIC_RELEASE);
            series->caps[cpu] = caps;
            series->caps_seen[cpu] = 1;
        }
        irq_restore(flags);
        if (!valid) __atomic_store_n(&series->failed, 1, __ATOMIC_RELEASE);
        attempt->acquisition = irq_latency_reference_acquire(&attempt->window);
        irq_latency_frequency_interval_t interval;
        attempt->valid = irq_latency_calibration_interval(&attempt->window, &interval);
        for (unsigned probe = 0; probe < CAL_PROBES_PER_WINDOW; ++probe)
            (void)irq_latency_irq_probe(&attempt->probe[probe]);
        __atomic_store_n(&attempt->complete, 1, __ATOMIC_RELEASE);
        timer_sleep(1);
    }
    __atomic_store_n(&worker->done, 1, __ATOMIC_RELEASE);
}

static bool calibration_collect(unsigned role)
{
    calibration_series_t *series = &g_calibration[role];
    series->limit = CAL_WINDOWS_PER_CPU * g_cpu_count;
    for (unsigned i = 0; i < g_cpu_count; ++i) {
        calibration_worker_t *worker = &g_calibration_workers[role][i];
        worker->series = role; worker->index = i;
        task_create_options_t options = {.name = "irq-clock", .task_class = TASK_CLASS_NORMAL,
                                         .flags = TASK_FLAG_SYSTEM};
        if (!thread_create_ex_handle(calibration_worker, worker, &options, &worker->handle)) {
            __atomic_store_n(&series->stop, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&series->failed, 1, __ATOMIC_RELEASE); break;
        }
        ++series->workers;
    }
    uint64_t deadline = timer_get_uptime_ms() + CAL_DEADLINE_MS;
    for (;;) {
        unsigned gone = 0;
        task_snapshot_t snapshot;
        for (unsigned i = 0; i < series->workers; ++i) {
            calibration_worker_t *worker = &g_calibration_workers[role][i];
            if (__atomic_load_n(&worker->done, __ATOMIC_ACQUIRE) &&
                !scheduler_snapshot_task_by_handle(worker->handle, &snapshot)) ++gone;
        }
        if (gone == series->workers) break;
        if (timer_get_uptime_ms() >= deadline) {
            __atomic_store_n(&series->stop, 1, __ATOMIC_RELEASE);
            series->failed = 1;
            return false;
        }
        timer_sleep(10);
    }
    series->joined = 1;
    return !series->failed && series->workers == g_cpu_count;
}

static bool calibration_select(unsigned role)
{
    calibration_series_t *series = &g_calibration[role];
    for (unsigned i = 0; i < series->limit; ++i) {
        calibration_attempt_t *attempt = &series->attempt[i];
        if (!__atomic_load_n(&attempt->complete, __ATOMIC_ACQUIRE)) return false;
        for (unsigned endpoint=0;endpoint<2;++endpoint) {
            unsigned id=2*i+endpoint;
            series->points[id]=(irq_latency_point_ref_t){
                .point=endpoint ? &attempt->window.end : &attempt->window.begin,
                .id=id,.epoch=1,.role=role,.complete=1};
        }
        for (unsigned j = 0; j < CAL_PROBES_PER_WINDOW; ++j) {
            const irq_latency_irq_probe_t *probe = &attempt->probe[j];
            if (!probe->valid || probe->cpu >= g_cpu_count) continue;
            uint64_t overhead = (probe->outer_end - probe->outer_begin) -
                                (probe->inner_end - probe->inner_begin);
            ++series->valid_probes[probe->cpu];
            if (overhead > series->overhead[probe->cpu]) series->overhead[probe->cpu] = overhead;
        }
    }
    series->pairing_complete=irq_latency_pairs_build(series->points,2*series->limit,
        g_cpu_count,1,role,series->order,series->disposition,series->pairs,CAL_WINDOWS,&series->pair_count);
    for (unsigned i=0;i<series->pair_count;++i)
        if (series->pairs[i].valid) ++series->valid_windows[series->pairs[i].cpu];
    bool valid = series->pairing_complete;
    for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu) {
        uint32_t clock_errors = series->caps_seen[cpu] ? 0 : CAL_CLOCK_CAPS;
        uint32_t observer_errors = series->valid_probes[cpu] < CAL_MIN_PROBES ? CAL_OBSERVER_COVERAGE : 0;
        if (!series->overhead[cpu] || series->overhead[cpu] > UINT64_MAX / 2)
            observer_errors |= CAL_OBSERVER_RANGE;
        if (role && series->overhead[cpu] > 2 * g_calibration[0].overhead[cpu])
            observer_errors |= CAL_OBSERVER_DRIFT;
        irq_latency_calibration_window_t selected[IRQ_LATENCY_CALIBRATION_WINDOWS];
        unsigned n=0;
        for (unsigned i=0;i<series->pair_count && n<IRQ_LATENCY_CALIBRATION_WINDOWS;++i) {
            if (!series->pairs[i].valid || series->pairs[i].cpu!=cpu) continue;
            series->selected[cpu][n]=i;
            irq_latency_pair_window(series->points,&series->pairs[i],&selected[n++]);
        }
        if (!series->pairing_complete || n<IRQ_LATENCY_CALIBRATION_WINDOWS) clock_errors |= CAL_CLOCK_WINDOWS;
        if (!clock_errors && !irq_latency_calibration_select(selected, &series->caps[cpu], &series->timebase[cpu]))
            clock_errors |= CAL_CLOCK_SELECTION;
        for (unsigned i = 0; i < series->pair_count; ++i) {
            const irq_latency_pair_t *pair=&series->pairs[i];
            if (clock_errors || !pair->valid || pair->cpu!=cpu) continue;
            irq_latency_calibration_window_t window;
            irq_latency_pair_window(series->points,pair,&window);
            irq_latency_frequency_interval_t interval;
            uint64_t delta, ppm;
            const irq_latency_timebase_t *base = &series->timebase[cpu];
            if (!irq_latency_calibration_interval(&window, &interval) ||
                interval.lower_hz < base->conversion_hz ||
                (base->declared_hz && (base->declared_hz < interval.lower_hz || base->declared_hz > interval.upper_hz)))
                { clock_errors |= CAL_CLOCK_INTERVAL; continue; }
            delta = interval.estimate_hz > base->estimate_hz ? interval.estimate_hz - base->estimate_hz :
                    base->estimate_hz - interval.estimate_hz;
            if (!irq_latency_muldiv(delta, UINT64_C(1000000), base->estimate_hz, true, &ppm) ||
                ppm > IRQ_LATENCY_CALIBRATION_PPM) clock_errors |= CAL_CLOCK_DRIFT;
        }
        if (role && !clock_errors &&
            !irq_latency_calibration_recheck(&g_calibration[0].timebase[cpu], &series->timebase[cpu]))
            clock_errors |= CAL_CLOCK_DRIFT;
        series->clock_errors[cpu] = clock_errors;
        series->observer_errors[cpu] = observer_errors;
        if (clock_errors || observer_errors) valid = false;
    }
    return valid;
}

static bool point_line(unsigned role, unsigned index, const char *endpoint,
                        const irq_latency_reference_point_t *point, uint64_t period)
{
    return latency_report("[IRQ_LATENCY][REFERENCE] role=%u index=%u endpoint=%s cpu=%u end_cpu=%u tsc_before=%llu hpet=%llu tsc_after=%llu switch_before=%llu switch_after=%llu mode=%u bits=%u retries=%u period_fs=%llu initialized=%u if_enabled=%u valid=%u acquired=%u sample_valid=%u identity_valid=%u if_after=%u period_after=%llu\n",
        role, index, endpoint, point->cpu, point->end_cpu, U64(point->tsc_before), U64(point->ticks),
        U64(point->tsc_after), U64(point->switch_sequence), U64(point->switch_after), point->mode,
        point->counter_bits, point->retries, U64(period), point->initialized, point->interrupts_enabled, point->valid,
        point->acquired, point->sample_valid, point->identity_valid, point->if_after, U64(point->period_after));
}

static bool calibration_report(unsigned role, bool valid)
{
    const calibration_series_t *series = &g_calibration[role];
    if (!series->joined)
        return latency_report("[IRQ_LATENCY][CALIBRATION_INCOMPLETE] role=%u workers=%u joined=0 status=UNAVAILABLE_TIMEBASE\n",
                               role, series->workers) && false;
    bool ok = latency_report("[IRQ_LATENCY][PAIRING] role=%u epoch=1 method=cpu-local-reference-pairs version=1 attempts=%u points=%u probes=%u workers=%u target_ns=20000000 deadline_ms=120000 pairs=%u complete=%u\n",
        role,series->limit,2*series->limit,4*series->limit,series->workers,series->pair_count,series->pairing_complete);
    for (unsigned i=0;i<series->workers;++i) {
        const calibration_worker_t *worker=&g_calibration_workers[role][i];
        ok &= latency_report("[IRQ_LATENCY][CALIBRATION_WORKER] role=%u index=%u task=%llu lifecycle=%llu done=%u joined=%u\n",
            role,i,U64(worker->handle.id),U64(worker->handle.lifecycle_generation),worker->done,series->joined);
    }
    for (unsigned i = 0; i < series->limit; ++i) {
        const calibration_attempt_t *attempt = &series->attempt[i];
        ok &= latency_report("[IRQ_LATENCY][WINDOW] role=%u index=%u worker=%u complete=%u valid=%u acquisition=%u\n",
                              role, i, attempt->worker, attempt->complete, attempt->valid, attempt->acquisition);
        ok &= point_line(role, i, "begin", &attempt->window.begin, attempt->window.begin.period_fs);
        ok &= point_line(role, i, "end", &attempt->window.end, attempt->window.end.period_fs);
        for (unsigned j = 0; j < CAL_PROBES_PER_WINDOW; ++j) {
            const irq_latency_irq_probe_t *probe = &attempt->probe[j];
            ok &= latency_report("[IRQ_LATENCY][OVERHEAD] role=%u index=%u probe=%u cpu=%u end_cpu=%u outer_begin=%llu inner_begin=%llu inner_end=%llu outer_end=%llu switch_before=%llu switch_after=%llu sequence_before=%llu sequence_after=%llu irq_before=%llu irq_after=%llu valid=%u\n",
                role, i, j, probe->cpu, probe->end_cpu, U64(probe->outer_begin), U64(probe->inner_begin),
                U64(probe->inner_end), U64(probe->outer_end), U64(probe->switch_before), U64(probe->switch_after),
                U64(probe->sequence_before), U64(probe->sequence_after), U64(probe->irq_before), U64(probe->irq_after), probe->valid);
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
            ok &= latency_report("[IRQ_LATENCY][ENTRY_CHECKPOINT] role=%u epoch=1 index=%u probe=%u cpu=%u sequence=%llu method=entry-return-checkpoints version=2 tsc=%llu exit_tsc=%llu\n",
                role, i, j, probe->cpu, U64(probe->sequence_after), U64(probe->entry_checkpoint), U64(probe->exit_checkpoint));
#endif
        }
    }
    for (unsigned i=0;i<2*series->limit;++i) {
        const irq_latency_point_disposition_t *d=&series->disposition[i];
        ok &= latency_report("[IRQ_LATENCY][POINT_DISPOSITION] role=%u id=%u kind=%u reason=%u pair=%u\n",
                             role,i,d->kind,d->reason,d->pair);
    }
    for (unsigned i=0;i<series->pair_count;++i) {
        const irq_latency_pair_t *p=&series->pairs[i];
        ok &= latency_report("[IRQ_LATENCY][PAIR] role=%u id=%u begin=%u end=%u cpu=%u valid=%u\n",
                             role,i,p->begin,p->end,p->cpu,p->valid);
    }
    for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu) {
        const irq_latency_timebase_t *base = &series->timebase[cpu];
        const irq_latency_capabilities_t *caps = &series->caps[cpu];
        ok &= latency_report("[IRQ_LATENCY][CAPABILITY] role=%u cpu=%u seen=%u basic=%x extended=%x leaf1_edx=%x extended1_edx=%x extended7_edx=%x denominator=%u numerator=%u crystal_hz=%u vendor_b=%x vendor_d=%x vendor_c=%x\n",
            role, cpu, series->caps_seen[cpu], caps->maximum_basic, caps->maximum_extended,
            caps->leaf1_edx, caps->extended1_edx, caps->extended7_edx, caps->denominator, caps->numerator, caps->crystal_hz,
            caps->vendor_b,caps->vendor_d,caps->vendor_c);
        ok &= latency_report("[IRQ_LATENCY][TIMEBASE] role=%u cpu=%u windows=%u probes=%u selected0=%u selected1=%u selected2=%u lower_hz=%llu upper_hz=%llu estimate_hz=%llu conversion_hz=%llu declared_hz=%llu overhead_max=%llu clock_errors=%u observer_errors=%u series_valid=%u\n",
            role, cpu, series->valid_windows[cpu], series->valid_probes[cpu], series->selected[cpu][0],
            series->selected[cpu][1], series->selected[cpu][2], U64(base->lower_hz), U64(base->upper_hz),
            U64(base->estimate_hz), U64(base->conversion_hz), U64(base->declared_hz), U64(series->overhead[cpu]), series->clock_errors[cpu], series->observer_errors[cpu], valid);
    }
    return ok;
}

static bool witness_line(unsigned cpu, const char *kind, unsigned index,
                          const irq_latency_witness_t *witness)
{
    bool ok = true;
    for (unsigned first = 0; first < IL_FIELDS; first += 8) {
        char cells[192]; size_t used = 0;
        unsigned count = IL_FIELDS - first < 8 ? IL_FIELDS - first : 8;
        for (unsigned i = 0; i < count; ++i) {
            int n = ksnprintf(cells + used, sizeof(cells) - used, "%s%llu", i ? "," : "",
                              U64(witness->cells[first+i]));
            if (n < 1 || (size_t)n >= sizeof(cells) - used) return false;
            used += (size_t)n;
        }
        ok &= latency_report("[IRQ_LATENCY][WITNESS] cpu=%u kind=%s index=%u first=%u count=%u cells=%s\n",
                              cpu, kind, index, first, count, cells);
    }
    return ok;
}

static bool component_report(uint32_t cpu)
{
    bool ok = true;
    for (unsigned operation = 1; operation <= 4; ++operation) {
        irq_latency_component_t callback, request;
        if (!irq_latency_components(cpu, operation, &callback, &request)) return false;
        uint64_t ns = 0, hz = g_calibration[0].timebase[cpu].conversion_hz;
        bool converted = irq_latency_cycles_ns(callback.maximum, hz, &ns);
        ok &= latency_report("[IRQ_LATENCY][CALLBACK] cpu=%u operation=%u count=%llu invalid=%llu failures=%llu maximum_cycles=%llu maximum_ns=%llu frequency_hz=%llu ns_available=%u begin=%llu end=%llu generation=%llu root=%llu context=%llu first_generation=%llu last_generation=%llu\n",
            cpu, operation, U64(callback.count), U64(callback.invalid), U64(callback.failures),
            U64(callback.maximum), U64(ns), U64(hz), converted, U64(callback.begin), U64(callback.end),
            U64(callback.generation), U64(callback.root), U64(callback.context),
            U64(callback.first_generation), U64(callback.last_generation));
        ok &= latency_report("[IRQ_LATENCY][REQUEST_ELAPSED] cpu=%u operation=%u count=%llu invalid=%llu failures=%llu maximum_ns=%llu begin_ns=%llu end_ns=%llu generation=%llu root=%llu context=%llu first_generation=%llu last_generation=%llu\n",
            cpu, operation, U64(request.count), U64(request.invalid), U64(request.failures),
            U64(request.maximum), U64(request.begin), U64(request.end), U64(request.generation),
            U64(request.root), U64(request.context), U64(request.first_generation), U64(request.last_generation));
    }
    return ok;
}

#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
static bool interior_point_line(unsigned cpu, unsigned path, const char *side,
    const irq_latency_interior_point_t *p)
{
    return latency_report("[IRQ_LATENCY][INTERIOR_POINT] cpu=%u path=%u endpoint=%s tsc=%llu flags=%llu actual_cpu=%llu epoch=%llu segment=%llu switches=%llu entries=%llu faults=%llu owner=%llu generation=%llu depth=%llu lock=%llu queries=%llu recorder_sequence=%llu\n",
        cpu,path,side,U64(p->tsc),U64(p->flags),U64(p->cpu),U64(p->epoch),U64(p->segment),
        U64(p->switches),U64(p->entries),U64(p->faults),U64(p->owner),U64(p->generation),
        U64(p->depth),U64(p->lock),U64(p->queries),U64(p->recorder_sequence));
}

static bool interior_report(unsigned cpu)
{
    bool ok = true;
    for (unsigned path=0;path<IRQ_LATENCY_PATHS;++path) {
        const irq_latency_interior_record_t *r=irq_latency_interior_read(cpu,path);
        if (!r) return false;
        uint64_t upper=0, minimum=0;
        bool frequency=!g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
            irq_latency_interior_minimum(r->maximum_cycles,g_calibration[0].timebase[cpu].upper_hz,
                g_calibration[1].timebase[cpu].upper_hz,&upper,&minimum);
        ok &= latency_report("[IRQ_LATENCY][INTERIOR] cpu=%u path=%u method=continuous-mask-lock-body version=2 epoch=1 count=%llu invalid=%llu cycles=%llu paths=%llu frequency_upper=%llu minimum_ns=%llu frequency_valid=%u authority=DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY below_budget=NOT_ESTABLISHED\n",
            cpu,path,U64(r->count),U64(r->invalid),U64(r->maximum_cycles),U64(r->maximum.paths),U64(upper),U64(minimum),frequency);
        if (r->count) {
            ok &= interior_point_line(cpu,path,"begin",&r->maximum.begin);
            ok &= interior_point_line(cpu,path,"end",&r->maximum.end);
        }
    }
    return ok;
}

static bool heap_operation_report(unsigned cpu)
{
    const irq_latency_heap_operation_record_t *r=irq_latency_heap_operation_read(cpu);
    const irq_latency_heap_record_t *allocate=irq_latency_heap_read(cpu,IRQ_HEAP_ALLOCATE);
    const irq_latency_heap_record_t *recheck=irq_latency_heap_read(cpu,IRQ_HEAP_RECHECK);
    if (!r || !allocate || !recheck) return false;
    uint64_t expected=allocate->metadata_count+recheck->metadata_count;
    const irq_latency_heap_operation_t *w=&r->maximum;
    uint64_t upper=0,minimum=0;
    bool frequency=!g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
        irq_latency_interior_minimum(r->maximum_cycles,g_calibration[0].timebase[cpu].upper_hz,
            g_calibration[1].timebase[cpu].upper_hz,&upper,&minimum);
    bool ok=latency_report("[IRQ_LATENCY][HEAP_OPERATION] cpu=%u method=heap-metadata-callsite version=1 site=6 epoch=1 boundary=operation-plus-declared-clock-boundaries count=%llu expected_count=%llu invalid=%llu last_sequence=%llu sequence=%llu operation=%llu cycles=%llu prepared=%llu validated=%llu previous_mark=%llu next_mark=%llu header_bytes=%llu frequency_upper=%llu minimum_ns=%llu frequency_valid=%u authority=DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY below_budget=NOT_ESTABLISHED\n",
        cpu,U64(r->count),U64(expected),U64(r->invalid),U64(r->last_sequence),U64(w->sequence),U64(w->operation),
        U64(r->maximum_cycles),U64(w->prepared),U64(w->validated),U64(w->previous_mark),U64(w->next_mark),
        U64(w->header_bytes),U64(upper),U64(minimum),frequency);
    if (r->count) for (unsigned i=0;i<4;++i) {
        const irq_latency_interior_point_t *point=i==0 ? &w->parent.begin : i==1 ? &w->interval.begin : i==2 ? &w->interval.end : &w->parent.end;
        const char *side=i==0 ? "parent_begin" : i==1 ? "begin" : i==2 ? "end" : "parent_end";
        ok &= latency_report("[IRQ_LATENCY][HEAP_OPERATION_POINT] cpu=%u sequence=%llu endpoint=%s tsc=%llu flags=%llu actual_cpu=%llu epoch=%llu segment=%llu switches=%llu entries=%llu faults=%llu owner=%llu generation=%llu depth=%llu lock=%llu queries=%llu recorder_sequence=%llu\n",
            cpu,U64(w->sequence),side,U64(point->tsc),U64(point->flags),U64(point->cpu),U64(point->epoch),
            U64(point->segment),U64(point->switches),U64(point->entries),U64(point->faults),U64(point->owner),
            U64(point->generation),U64(point->depth),U64(point->lock),U64(point->queries),U64(point->recorder_sequence));
    }
    return ok && r->count==expected && !r->invalid;
}

static bool clock_body_report(unsigned cpu)
{
    const irq_latency_clock_record_t *r=irq_latency_clock_read(cpu);
    if (!r) return false;
    const irq_latency_clock_witness_t *w=&r->maximum;
    uint64_t upper=0,minimum=0;
    bool frequency=!g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
        irq_latency_interior_minimum(r->maximum_cycles,g_calibration[0].timebase[cpu].upper_hz,
            g_calibration[1].timebase[cpu].upper_hz,&upper,&minimum);
    bool ok=latency_report("[IRQ_LATENCY][CLOCK_BODY] cpu=%u method=masked-clock-segment version=3 epoch=1 count=%llu invalid=%llu started=%llu retired=%llu pending=%llu sequence=%llu cycles=%llu incoming_flags=%llu site=%llu paths=%llu phase=%llu frequency_upper=%llu minimum_ns=%llu frequency_valid=%u authority=DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY below_budget=NOT_ESTABLISHED\n",
        cpu,U64(r->count),U64(r->invalid),U64(r->started),U64(r->retired),U64(r->pending),U64(w->sequence),U64(r->maximum_cycles),U64(w->incoming_flags),U64(w->site),U64(w->paths),U64(w->phase),U64(upper),U64(minimum),frequency);
    if (r->count) for (unsigned i=0;i<2;++i) {
        const irq_latency_interior_point_t *p=i ? &w->end : &w->begin;
        ok &= latency_report("[IRQ_LATENCY][CLOCK_BODY_POINT] cpu=%u endpoint=%s tsc=%llu flags=%llu actual_cpu=%llu epoch=%llu segment=%llu switches=%llu entries=%llu faults=%llu owner=%llu generation=%llu depth=%llu lock=%llu queries=%llu recorder_sequence=%llu\n",
            cpu,i ? "end" : "begin",U64(p->tsc),U64(p->flags),U64(p->cpu),U64(p->epoch),U64(p->segment),U64(p->switches),U64(p->entries),U64(p->faults),U64(p->owner),U64(p->generation),U64(p->depth),U64(p->lock),U64(p->queries),U64(p->recorder_sequence));
    }
    return ok && !r->invalid && !r->pending && r->started==r->retired;
}

static bool tick_body_report(unsigned cpu)
{
    const irq_latency_tick_record_t *r=irq_latency_tick_read(cpu);
    if (!r) return false;
    const irq_latency_tick_witness_t *w=&r->maximum;
    uint64_t upper=0,minimum=0;
    bool frequency=!g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
        irq_latency_interior_minimum(r->maximum_cycles,g_calibration[0].timebase[cpu].upper_hz,
            g_calibration[1].timebase[cpu].upper_hz,&upper,&minimum);
    bool ok=latency_report("[IRQ_LATENCY][TICK_BODY] cpu=%u method=lapic-handler-body version=4 epoch=1 vector=34 count=%llu invalid=%llu started=%llu retired=%llu pending=%llu without_memory=%llu sequence=%llu outer_sequence=%llu cycles=%llu paths=%llu sample_begin=%llu sample_end=%llu after_clock=%llu after_work=%llu frequency_upper=%llu minimum_ns=%llu frequency_valid=%u authority=DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY below_budget=NOT_ESTABLISHED\n",
        cpu,U64(r->count),U64(r->invalid),U64(r->started),U64(r->retired),U64(r->pending),U64(r->without_memory),U64(w->sequence),U64(w->outer_sequence),U64(r->maximum_cycles),U64(w->paths),U64(w->sample_begin),U64(w->sample_end),U64(w->after_clock),U64(w->after_work),U64(upper),U64(minimum),frequency);
    ok &= latency_report("[IRQ_LATENCY][TICK_SAMPLE] cpu=%u epoch=1 sequence=%llu outer_sequence=%llu counter_end=%llu ticks=%llu mode=%llu retries=%llu valid=%llu source=%llu source_epoch=%llu\n",
        cpu,U64(w->sequence),U64(w->outer_sequence),U64(w->counter_end),U64(w->sample_ticks),U64(w->sample_mode),U64(w->sample_retries),U64(w->sample_valid),U64(w->sample_source),U64(w->source_epoch));
    if (r->count) for (unsigned i=0;i<2;++i) {
        const irq_latency_interior_point_t *p=i ? &w->end : &w->begin;
        ok &= latency_report("[IRQ_LATENCY][TICK_BODY_POINT] cpu=%u endpoint=%s tsc=%llu flags=%llu actual_cpu=%llu epoch=%llu segment=%llu switches=%llu entries=%llu faults=%llu owner=%llu generation=%llu depth=%llu lock=%llu queries=%llu recorder_sequence=%llu\n",
            cpu,i ? "end" : "begin",U64(p->tsc),U64(p->flags),U64(p->cpu),U64(p->epoch),U64(p->segment),U64(p->switches),U64(p->entries),U64(p->faults),U64(p->owner),U64(p->generation),U64(p->depth),U64(p->lock),U64(p->queries),U64(p->recorder_sequence));
    }
    return ok && !r->invalid && !r->pending && r->started==r->retired && r->retired==r->count+r->without_memory;
}

static bool heap_stage_report(unsigned cpu)
{
    bool ok=true;
    for (unsigned op=1;op<=IRQ_HEAP_OPERATIONS;++op) {
        const irq_latency_heap_record_t *r=irq_latency_heap_read(cpu,op);
        if (!r) return false;
        const irq_latency_heap_event_t *e=&r->maximum;
        uint64_t upper=0, minimum=0;
        bool frequency=!g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
            irq_latency_interior_minimum(r->maximum_cycles,g_calibration[0].timebase[cpu].upper_hz,
                g_calibration[1].timebase[cpu].upper_hz,&upper,&minimum);
        ok &= latency_report("[IRQ_LATENCY][HEAP_STAGE] cpu=%u operation=%u method=continuous-mask-heap-stages version=1 epoch=1 count=%llu invalid=%llu last_sequence=%llu sequence=%llu cycles=%llu marks=%llu metadata_count=%llu interpretation=OBSERVER_INCLUSIVE_STAGE wait_start=%llu acquired=%llu pending_begin=%llu pending_end=%llu requested_pages=%llu frequency_upper=%llu minimum_ns=%llu frequency_valid=%u authority=DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY below_budget=NOT_ESTABLISHED\n",
            cpu,op,U64(r->count),U64(r->invalid),U64(r->last_sequence),U64(e->sequence),U64(r->maximum_cycles),
            U64(e->marks),U64(r->metadata_count),U64(e->wait_start),U64(e->acquired),U64(e->pending_begin),U64(e->pending_end),
            U64(e->requested_pages),U64(upper),U64(minimum),frequency);
        if (!r->count) continue;
        for (unsigned end=0;end<2;++end) {
            const irq_latency_interior_point_t *p=end ? &e->body.end : &e->body.begin;
            ok &= latency_report("[IRQ_LATENCY][HEAP_POINT] cpu=%u operation=%u sequence=%llu endpoint=%s tsc=%llu flags=%llu actual_cpu=%llu epoch=%llu segment=%llu switches=%llu entries=%llu faults=%llu owner=%llu generation=%llu depth=%llu lock=%llu queries=%llu recorder_sequence=%llu\n",
                cpu,op,U64(e->sequence),end ? "end" : "begin",U64(p->tsc),U64(p->flags),U64(p->cpu),U64(p->epoch),
                U64(p->segment),U64(p->switches),U64(p->entries),U64(p->faults),U64(p->owner),U64(p->generation),
                U64(p->depth),U64(p->lock),U64(p->queries),U64(p->recorder_sequence));
        }
        for (unsigned i=0;i<e->marks && i<IRQ_HEAP_MARKS;++i) {
            const irq_latency_heap_mark_t *m=&e->mark[i];
            ok &= latency_report("[IRQ_LATENCY][HEAP_MARK] cpu=%u operation=%u sequence=%llu index=%u site=%llu tsc=%llu work=%llu bytes=%llu\n",
                cpu,op,U64(e->sequence),i,U64(m->site),U64(m->tsc),U64(m->work),U64(m->bytes));
        }
    }
    return ok;
}

static bool writeback_point_line(unsigned cpu, const char *kind, const char *side,
    const irq_latency_interior_point_t *p)
{
    return latency_report("[IRQ_LATENCY][WRITEBACK_POINT] cpu=%u kind=%s endpoint=%s tsc=%llu flags=%llu actual_cpu=%llu epoch=%llu segment=%llu switches=%llu entries=%llu faults=%llu owner=%llu generation=%llu depth=%llu lock=%llu queries=%llu recorder_sequence=%llu\n",
        cpu,kind,side,U64(p->tsc),U64(p->flags),U64(p->cpu),U64(p->epoch),U64(p->segment),
        U64(p->switches),U64(p->entries),U64(p->faults),U64(p->owner),U64(p->generation),
        U64(p->depth),U64(p->lock),U64(p->queries),U64(p->recorder_sequence));
}

static bool writeback_report(unsigned cpu)
{
    const irq_latency_writeback_record_t *r=irq_latency_writeback_read(cpu);
    if (!r) return false;
    bool ok=latency_report("[IRQ_LATENCY][WRITEBACK] cpu=%u method=continuous-mask-wbinvd version=1 epoch=1 count=%llu invalid=%llu maximum_cycles=%llu first_generation=%llu last_generation=%llu authority=DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY below_budget=NOT_ESTABLISHED\n",
        cpu,U64(r->count),U64(r->invalid),U64(r->maximum_cycles),U64(r->first_generation),U64(r->last_generation));
    if (!r->count) return ok;
    for (unsigned i=0;i<2;++i) {
        const char *kind=i ? "maximum" : "first";
        const irq_latency_writeback_witness_t *w=i ? &r->maximum : &r->first;
        uint64_t upper=0, minimum=0, cycles=w->end.tsc-w->begin.tsc;
        bool frequency=!g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
            irq_latency_interior_minimum(cycles,g_calibration[0].timebase[cpu].upper_hz,
                g_calibration[1].timebase[cpu].upper_hz,&upper,&minimum);
        ok &= latency_report("[IRQ_LATENCY][WRITEBACK_WITNESS] cpu=%u kind=%s operation=3 request_generation=%llu root=%llu cycles=%llu frequency_upper=%llu minimum_ns=%llu frequency_valid=%u\n",
            cpu,kind,U64(w->request_generation),U64(w->root),U64(cycles),U64(upper),U64(minimum),frequency);
        ok &= writeback_point_line(cpu,kind,"begin",&w->begin);
        ok &= writeback_point_line(cpu,kind,"end",&w->end);
    }
    return ok;
}


#endif

static unsigned terminal_freeze(void)
{
    if (!irq_latency_freeze_request()) return 0;
    uint64_t deadline = timer_get_uptime_ms() + UINT64_C(5000);
    unsigned frozen;
    do {
        frozen = 0;
        for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu) {
            const irq_latency_snapshot_t *snapshot;
            const irq_latency_witness_t *failures;
            uint64_t cutoff, cost;
            if (irq_latency_terminal_read(cpu, &snapshot, &failures, &cutoff, &cost)) ++frozen;
        }
        if (frozen == g_cpu_count) break;
        timer_sleep(1);
    } while (timer_get_uptime_ms() < deadline);
    return frozen;
}

static bool clocksource_terminal_report(unsigned cpu)
{
    const clocksource_terminal_t *s=clocksource_terminal_read(cpu);
    if(!s)return false;
    bool ok=latency_report("[IRQ_LATENCY][CLOCKSOURCE_TERMINAL] version=3 cpu=%u source=%u epoch=%llu reason=%u busy=%u registered=%u reads=%llu references=%llu maximum_reference_ns=%llu last_tsc=%llu last_ns=%llu\n",
        cpu,s->source,U64(s->epoch),s->reason,s->busy,s->registered,U64(s->reads),U64(s->references),
        U64(s->maximum_reference_ns),U64(s->last_tsc),U64(s->last_ns));
    for(unsigned point=0;s->references && point<2;++point) {
        const pvclock_reference_t *p=point ? &s->maximum : &s->previous;
        ok &= latency_report("[IRQ_LATENCY][CLOCKSOURCE_REFERENCE] version=3 cpu=%u epoch=%llu point=%u tsc0=%llu ns0=%llu ver0=%u mul0=%u shift0=%d flags0=%u retry0=%u tsc1=%llu ns1=%llu ver1=%u mul1=%u shift1=%d flags1=%u retry1=%u ticks=%llu ns=%llu period=%llu mode=%u retries=%u valid=%u\n",
            cpu,U64(s->epoch),point,U64(p->before.tsc),U64(p->before.ns),p->before.version,p->before.multiplier,p->before.shift,p->before.flags,p->before.retries,
            U64(p->after.tsc),U64(p->after.ns),p->after.version,p->after.multiplier,p->after.shift,p->after.flags,p->after.retries,
            U64(p->ticks),U64(p->ns),U64(p->period_fs),p->mode,p->retries,p->valid);
    }
    return ok && !s->busy && s->source!=CLOCKSOURCE_FAILED;
}

static bool terminal_report(unsigned frozen)
{
    bool ok = frozen == g_cpu_count;
    for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu) {
        ok &= clocksource_terminal_report(cpu);
        const irq_latency_snapshot_t *snapshot;
        const irq_latency_witness_t *failures;
        uint64_t cutoff, cost;
        if (!irq_latency_terminal_read(cpu, &snapshot, &failures, &cutoff, &cost)) {
            uint64_t v[14] = {0};
            bool observed = irq_latency_freeze_diagnostic(cpu, v);
            (void)latency_report("[IRQ_LATENCY][UNFROZEN] cpu=%u observed=%u sequence=%llu tsc=%llu flags=%llu rsp=%llu scope_paths=%llu active=%llu paths=%llu pending=%llu lock_depth=%llu begin=%llu site=%llu begin_sequence=%llu context=%llu irq_sequence=%llu\n",
                cpu, observed, U64(v[0]), U64(v[1]), U64(v[2]), U64(v[3]), U64(v[4]),
                U64(v[5]), U64(v[6]), U64(v[7]), U64(v[8]), U64(v[9]), U64(v[10]),
                U64(v[11]), U64(v[12]), U64(v[13]));
            continue;
        }
        ok &= latency_report("[IRQ_LATENCY][CPU] cpu=%u epoch=%llu sequence=%llu cutoff=%llu snapshot_cycles=%llu active=%llu pending=%llu invalid=%llu exhausted=%llu recursion=%llu missing_begin=%llu unclosed=%llu early=%llu probes=%llu failures=%llu failure_overflow=%llu\n",
            cpu, U64(snapshot->epoch), U64(snapshot->sequence), U64(cutoff), U64(cost), U64(snapshot->active),
            U64(snapshot->pending), U64(snapshot->invalid), U64(snapshot->exhausted), U64(snapshot->recursion),
            U64(snapshot->missing_begin), U64(snapshot->unclosed), U64(snapshot->early), U64(snapshot->probes),
            U64(snapshot->failure_count), U64(snapshot->failure_overflow));
        uint64_t elided, mmio_reused, prechecked;
        if (irq_latency_nested_elisions(cpu, &elided) && irq_latency_mmio_reuses(cpu, &mmio_reused) &&
            irq_latency_nested_prechecks(cpu, &prechecked))
            ok &= latency_report("[IRQ_LATENCY][NESTED_OBSERVER] cpu=%u epoch=%llu elided=%llu mmio_reused=%llu prechecked=%llu\n",
                cpu, U64(snapshot->epoch), U64(elided), U64(mmio_reused), U64(prechecked));
        else ok = false;
        for (unsigned path = 0; path < IRQ_LATENCY_PATHS + 3; ++path) {
            const irq_latency_path_record_t *row = path < IRQ_LATENCY_PATHS ?
                &snapshot->path[path] : &snapshot->excluded[path - IRQ_LATENCY_PATHS];
            uint64_t ns = 0, hz = g_calibration[0].timebase[cpu].conversion_hz;
            bool converted = irq_latency_cycles_ns(row->maximum_cycles, hz, &ns);
            bool qualified = g_calibration[0].joined && g_calibration[1].joined &&
                !g_calibration[0].failed && !g_calibration[1].failed &&
                !g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
                !g_calibration[0].observer_errors[cpu] && !g_calibration[1].observer_errors[cpu];
            const char *status = !row->count || row->invalid || !row->maximum_cycles ? "INVALID_COVERAGE" :
                !converted || !qualified ? "UNQUALIFIED_OBSERVATION" : ns < IRQ_LATENCY_BUDGET_NS ? "BELOW_BUDGET_OBSERVATION" : "VIOLATION";
            if (path >= IRQ_LATENCY_PATHS) status = "EXCLUDED_CONTEXT";
            ok &= latency_report("[IRQ_LATENCY][MAXIMUM] cpu=%u path=%u count=%llu invalid=%llu violations=%llu maximum_cycles=%llu maximum_ns=%llu frequency_hz=%llu ns_available=%u measurement_qualified=%u status=%s\n",
                cpu, path, U64(row->count), U64(row->invalid), U64(row->violations), U64(row->maximum_cycles), U64(ns), U64(hz), converted, qualified, status);
            ok &= witness_line(cpu, "maximum", path, &row->maximum);
            if (path < IRQ_LATENCY_PATHS) {
                const irq_latency_contention_t *c = &snapshot->contention[path];
                ok &= latency_report("[IRQ_LATENCY][CONTENTION] cpu=%u path=%u observations=%llu queued=%llu maximum_distance=%llu lock=%llu begin_sequence=%llu paths=%llu\n",
                    cpu, path, U64(c->observations), U64(c->contended), U64(c->maximum_distance),
                    U64(c->lock), U64(c->begin_sequence), U64(c->paths));
            }
        }
        for (unsigned i = 0; i < snapshot->failure_count && i < IRQ_LATENCY_FAILURES; ++i)
            ok &= witness_line(cpu, "failure", i, &failures[i]);
        ok &= component_report(cpu);
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
        ok &= interior_report(cpu);
        ok &= writeback_report(cpu);
        ok &= heap_stage_report(cpu);
        ok &= heap_operation_report(cpu);
        ok &= clock_body_report(cpu);
        ok &= tick_body_report(cpu);
#endif
    }
    for (unsigned site = 1; site < 32; ++site) {
        uint64_t count = irq_latency_invalid_site(site);
        if (count) ok &= latency_report("[IRQ_LATENCY][INVALID_SITE] site=%u count=%llu\n", site, U64(count));
    }
    for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu) {
        uint64_t failure[4];
        if (irq_latency_owner_failure(cpu, failure))
            ok &= latency_report("[IRQ_LATENCY][OWNER_FAILURE] cpu=%u count=%llu rsp=%llu site=%llu owner_slot=%llu\n",
                cpu, U64(failure[0]), U64(failure[1]), U64(failure[2]), U64(failure[3]));
    }
    uint64_t early, invalid;
    irq_latency_boundary_counts(&early, &invalid);
    return latency_report("[IRQ_LATENCY][TERMINAL] cpus=%u frozen=%u early_boundaries=%llu invalid_boundaries=%llu transport=%s\n",
                           g_cpu_count, frozen, U64(early), U64(invalid), ok ? "COMPLETE" : "INCOMPLETE") && ok;
}

int irq_latency_test_proof(bool deferred, uint64_t session, unsigned invocations)
{
    if (__atomic_load_n(&g_latency_test_state,__ATOMIC_ACQUIRE)!=0) return 1;
    return tlb_fixture_proof_prepare(deferred,session,invocations) ? 0 : 1;
}

static uint64_t g_proof_terminal_service;

int irq_latency_test_export(void)
{
    if (!g_terminal_complete || !g_audit_reported || !g_proof_terminal_service) return 1;
    return tlb_fixture_proof_export(g_proof_terminal_service) ? 0 : 1;
}

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
bool irq_latency_test_summary(irq_latency_test_summary_t *out)
{
    if (!out) return false;
    *out=(irq_latency_test_summary_t){0};
    if (!irq_is_enabled() || !g_terminal_complete || !g_audit_reported ||
        !g_cpu_count || g_cpu_count>HOBBYOS_MAX_CPUS) return false;
    for (unsigned cpu=0;cpu<g_cpu_count;++cpu) {
        const irq_latency_snapshot_t *snapshot;
        const irq_latency_witness_t *failures;
        uint64_t cutoff,cost;
        if (!irq_latency_terminal_read(cpu,&snapshot,&failures,&cutoff,&cost)) return false;
        if (snapshot->active || snapshot->pending || snapshot->invalid || snapshot->exhausted ||
            snapshot->recursion || snapshot->missing_begin || snapshot->unclosed) ++out->invalid;
        bool qualified=g_calibration[0].joined && g_calibration[1].joined &&
            !g_calibration[0].failed && !g_calibration[1].failed &&
            !g_calibration[0].clock_errors[cpu] && !g_calibration[1].clock_errors[cpu] &&
            !g_calibration[0].observer_errors[cpu] && !g_calibration[1].observer_errors[cpu];
        for (unsigned path=0;path<IRQ_LATENCY_PATHS;++path) {
            const irq_latency_path_record_t *row=&snapshot->path[path];
            uint64_t ns=0;
            bool converted=irq_latency_cycles_ns(row->maximum_cycles,
                g_calibration[0].timebase[cpu].conversion_hz,&ns);
            ++out->cells;
            if (!row->count || row->invalid || !row->maximum_cycles || !converted) ++out->missing;
            if (!qualified) ++out->unqualified;
            if (converted && ns>=IRQ_LATENCY_BUDGET_NS) ++out->over_budget;
            if (converted && ns>out->maximum_ns) {
                out->maximum_ns=ns;out->maximum_cpu=cpu;out->maximum_path=path;
            }
        }
    }
    uint64_t early,invalid;
    irq_latency_boundary_counts(&early,&invalid);
    if (invalid) ++out->invalid;
    return true;
}
#endif

static bool activation_report(bool valid)
{
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
#define IRQ_PROOF_OUTPUT_VERSION "3"
#else
#define IRQ_PROOF_OUTPUT_VERSION "2"
#endif
    return latency_report("[IRQ_LATENCY][BEGIN] cpus=%u budget_ns=50000 epoch=1 calibration=cpu-local-reference-pairs calibration_version=1 witness_cells=29 schema=2 primitive=" IRQ_LATENCY_PRIMITIVE " identity_method=stack-mask-identity-v2 entry_admission=entry-admission-cookie-v1 observer_updates=nested-observer-precheck-v2 mmio_identity=continuous-mask-mmio-identity-v1 writeback_method=%s request_timing=request-local-cycles-v1 proof_output=" IRQ_PROOF_OUTPUT_VERSION " heap_method=%s heap_operation_method=%s clock_body_method=%s tick_body_method=%s detail_policy=%s closure=snapshot-before-post-calibration-v1 status=%s\n",
                         g_cpu_count,
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
        "continuous-mask-wbinvd-v1","continuous-mask-heap-stages-v1","heap-metadata-callsite-v1","masked-clock-segment-v3","lapic-handler-body-v4",
#else
        "unavailable","unavailable","unavailable","unavailable","unavailable",
#endif
#if HOBBYOS_IRQ_LATENCY_DETAILS
        "bounded-details-v1",
#else
        "mandatory-maxima-only-v1",
#endif
        valid ? "ACTIVE" : "UNAVAILABLE_TIMEBASE") &&
        latency_report("[IRQ_LATENCY][STACK_COVERAGE_METHOD] version=1 cpus=%u epoch=1\n", g_cpu_count);
}

int irq_latency_test_begin(void)
{
    uint32_t expected = 0;
    if (!irq_is_enabled() || !tlb_fixture_proof_configured() || !__atomic_compare_exchange_n(&g_latency_test_state, &expected, 1,
            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 1;
    bool valid = irq_latency_identity_prepare() && calibration_collect(0);
    if (valid) valid = calibration_select(0);
    valid = calibration_report(0, valid) && valid;
    if (valid) for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu)
        if (!irq_latency_set_irq_overhead(cpu, 2 * g_calibration[0].overhead[cpu])) valid = false;
    if (valid) valid = irq_latency_activate(g_calibration[0].timebase, g_cpu_count);
    valid = activation_report(valid) && valid;
    __atomic_store_n(&g_latency_test_state, valid ? 2 : 5, __ATOMIC_RELEASE);
    return valid ? 0 : 1;
}

int irq_latency_test_stress(void)
{
    if (!irq_is_enabled() || __atomic_load_n(&g_latency_test_state, __ATOMIC_ACQUIRE) != 2) return 1;
    return irq_latency_stress_run();
}

int irq_latency_test_finish(void)
{
    uint32_t expected = 2;
    if (!irq_is_enabled() || !__atomic_compare_exchange_n(&g_latency_test_state, &expected, 3,
            false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 1;
    bool work = stack_report_flush() && stack_report_quiet() &&
        stack_report_checkpoint("work") && warning_report_flush() && warning_report_quiet() &&
        warning_report_checkpoint("work") &&
        service_cutoff("WORK_CUTOFF");
    unsigned frozen = work ? terminal_freeze() : 0;
    bool ready = frozen == g_cpu_count && latency_report(
        "[IRQ_LATENCY][SNAPSHOT_READY] epoch=1 cpus=%u frozen=%u closure=snapshot-before-post-calibration-v1\n",
        g_cpu_count, frozen);
    bool valid = ready && calibration_collect(1);
    if (valid) valid = calibration_select(1);
    valid = calibration_report(1, valid) && valid;
    bool terminal = ready && g_calibration[1].joined && stack_report_flush() &&
        stack_report_quiet() && warning_report_flush() && warning_report_quiet() && terminal_report(frozen);
    terminal = terminal && service_cutoff("SNAPSHOT_CUTOFF");
    g_terminal_complete = terminal;
    valid = valid && terminal;
    __atomic_store_n(&g_latency_test_state, valid ? 4 : 5, __ATOMIC_RELEASE);
    (void)latency_report("[IRQ_LATENCY][CALIBRATION_END] status=%s\n", valid ? "VALID" : !terminal ? "INCOMPLETE_TERMINAL" : "INVALID_CALIBRATION");
    return valid ? 0 : 1;
}

int irq_latency_test_audit(void)
{
    if (!irq_is_enabled() || !g_terminal_complete || g_audit_reported) return 1;
    g_audit_reported = true;
    uint64_t admitted, completed;
    uint64_t deadline = timer_get_uptime_ms() + UINT64_C(5000);
    do {
        if (!cpu_request_audit_snapshot(&admitted, &completed)) return 1;
        if (admitted == completed) break;
        timer_sleep(1);
    } while (timer_get_uptime_ms() < deadline);
    if (admitted != completed) return 1;
    bool ok = latency_report("[IRQ_LATENCY][AUDIT_BEGIN] epoch=1 admitted=%llu completed=%llu cpus=%u root=0x%llx\n",
        U64(admitted), U64(completed), g_cpu_count, U64(paging_kernel_root_physical()));
    for (uint64_t generation = 1; ok && generation <= completed; ++generation) {
        cpu_request_audit_t audit;
        if (!cpu_request_audit_copy(generation, &audit)) return 1;
        const cpu_request_spec_t *s = &audit.spec;
        const cpu_request_result_t *r = &audit.result;
        char identities[384]; size_t used = 0;
        for (unsigned slot = 0; slot < g_cpu_count; ++slot) if (s->targets & (1u << slot)) {
            int n = ksnprintf(identities + used, sizeof(identities) - used, "%s%u:%u", used ? "," : "", slot, s->apic[slot]);
            if (n < 1 || (size_t)n >= sizeof(identities) - used) return 1;
            used += (size_t)n;
        }
        if (!used) return 1;
        ok = latency_report("[IRQ_LATENCY][SERVICE] generation=%llu operation=%u mapping=%u root=0x%llx first=0x%llx last=0x%llx pages=%llu strategy=%u targets=0x%x ack=0x%x missing=0x%x failed=0x%x sent=0x%x send_failed=0x%x notifications=%u status=%u elapsed_ns=%llu identities=%s\n",
            U64(s->generation), (unsigned)s->operation, audit.mapping, U64(s->root), U64(s->interval.first),
            U64(s->interval.last), U64(s->interval.pages), (unsigned)s->interval.strategy,
            r->requested, r->acknowledged, r->missing, r->failed, r->sent, r->send_failed,
            r->notifications, (unsigned)r->status, U64(r->elapsed_ns), identities);
        for (unsigned slot=0;ok && slot<g_cpu_count;++slot) if (s->targets & (1u << slot)) {
            cpu_request_timing_t timing;
            char data[512];
            ok=cpu_request_timing_copy(generation,slot,&timing) &&
                cpu_request_timing_format(data,sizeof(data),generation,s->root,slot,&timing) &&
                latency_report("[IRQ_LATENCY][REQUEST_CYCLES] data=%s\n",data);
        }
    }
    uint64_t final_admitted = 0, final_completed = 0;
    ok = cpu_request_audit_snapshot(&final_admitted, &final_completed) &&
        final_admitted == admitted && final_completed == completed && ok;
    ok = stack_report_checkpoint("final") && warning_report_checkpoint("final") && ok;
    bool emitted = latency_report("[IRQ_LATENCY][AUDIT_END] epoch=1 admitted=%llu completed=%llu emitted=%llu status=%s\n",
        U64(final_admitted), U64(final_completed), U64(completed), ok ? "COMPLETE" : "INCOMPLETE");
    if (ok && emitted) g_proof_terminal_service=final_completed;
    return ok && emitted ? 0 : 1;
}
