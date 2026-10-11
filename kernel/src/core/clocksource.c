#include "clocksource.h"
#include "pvclock.h"
#include "irq_latency_tsc.h"
#include "spinlock.h"
#include "../cpu/cpu.h"
#include "../timer/hpet.h"
#include "../memory/physical_translation.h"
#include "../drivers/serial.h"
#include "../libc/string.h"

#define CLOCKSOURCE_POINTS 4u
#define CLOCKSOURCE_POLL_LIMIT 1000000u
#define CLOCKSOURCE_REFERENCE_NS UINT64_C(1000000000)
#define CLOCKSOURCE_ANCHOR_ERROR_NS UINT64_C(1000)

typedef enum {
    SOURCE_UNPREPARED, SOURCE_UNSUPPORTED, SOURCE_REGISTERING,
    SOURCE_QUALIFIED, SOURCE_REJECTED, SOURCE_QUARANTINED
} source_cpu_status_t;

typedef enum {
    SOURCE_REASON_NONE, SOURCE_REASON_FEATURES, SOURCE_REASON_CONTEXT,
    SOURCE_REASON_ADDRESS, SOURCE_REASON_MSR, SOURCE_REASON_SNAPSHOT,
    SOURCE_REASON_REFERENCE, SOURCE_REASON_RATE, SOURCE_REASON_OFFSET,
    SOURCE_REASON_IDENTITY, SOURCE_REASON_REENTRANT, SOURCE_REASON_COUNTER,
    SOURCE_REASON_SCALE, SOURCE_REASON_ANCHOR, SOURCE_REASON_OVERFLOW
} source_reason_t;

typedef struct {
    pvclock_page_t page;
    uint8_t reserved[64 - sizeof(pvclock_page_t)];
} __attribute__((aligned(64))) source_page_t;

typedef struct {
    source_cpu_status_t status;
    source_reason_t reason;
    uint32_t features, busy, registered, prepared_points;
    uint64_t physical, registration, reads, references, maximum_reference_ns;
    uint64_t last_tsc, last_ns, last_reference_ns;
    pvclock_reference_t points[CLOCKSOURCE_POINTS], reference, previous_reference;
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    pvclock_reference_t maximum_reference, maximum_reference_previous;
#endif
    pvclock_page_t anchor;
    int64_t offset_lower, offset_upper;
} source_cpu_t;

static source_page_t g_source_pages[HOBBYOS_MAX_CPUS] __attribute__((aligned(4096)));
static source_cpu_t g_source_cpu[HOBBYOS_MAX_CPUS];
static clocksource_id_t g_source = CLOCKSOURCE_HPET;
static uint64_t g_source_epoch = 1;
static uint32_t g_source_selected;
static int64_t g_source_offset;
static uint64_t g_source_origin_ns;
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
static clocksource_terminal_t g_source_terminal[HOBBYOS_MAX_CPUS];
static uint32_t g_source_terminal_ready[HOBBYOS_MAX_CPUS];
#endif

clocksource_id_t clocksource_current(void)
{
    return __atomic_load_n(&g_source, __ATOMIC_ACQUIRE);
}

uint64_t clocksource_epoch(void)
{
    return __atomic_load_n(&g_source_epoch, __ATOMIC_ACQUIRE);
}

void clocksource_invalidate(void)
{
    __atomic_store_n(&g_source,CLOCKSOURCE_FAILED,__ATOMIC_RELEASE);
    __atomic_store_n(&g_source_epoch,3,__ATOMIC_RELEASE);
}

static bool source_features(uint32_t *features)
{
    uint32_t a,b,c,d,maximum,vb,vc,vd;
    if (!cpu_get_cpuid_count(0,0,&a,&b,&c,&d) || a<1 || b!=0x756e6547 || c!=0x6c65746e || d!=0x49656e69 ||
        !cpu_get_cpuid_count(1,0,&a,&b,&c,&d) || !(c & (1u<<31)) ||
        (d & ((1u<<4)|(1u<<5)|(1u<<26))) != ((1u<<4)|(1u<<5)|(1u<<26)) ||
        !irq_latency_tsc_available() ||
        !cpu_get_cpuid_count(PVCLOCK_CPUID_BASE,0,&maximum,&vb,&vc,&vd) ||
        !cpu_get_cpuid_count(PVCLOCK_CPUID_FEATURES,0,&a,&b,&c,&d) ||
        !pvclock_features(maximum,vb,vc,vd,a)) return false;
    *features = a;
    return true;
}

static bool source_reference(cpu_slot_t slot, pvclock_reference_t *out)
{
    pvclock_reference_t p = {0};
    hpet_clock_sample_t h = {0};
    *out=p;
    if (pvclock_snapshot(&g_source_pages[slot].page,&p.before) != PVCLOCK_OK)
        return false;
    bool valid = hpet_read_clock_sample(&h);
    bool after_valid=pvclock_snapshot(&g_source_pages[slot].page,&p.after)==PVCLOCK_OK;
    p.ticks=h.ticks;p.ns=h.ns;p.period_fs=hpet_period_fs();
    p.mode=h.mode;p.retries=h.retries;p.valid=valid && h.valid && after_valid;
    *out=p;
    int64_t lower,upper;
    return pvclock_reference_bounds(&p,&lower,&upper);
}

static void source_reject_local(cpu_slot_t slot, source_reason_t reason)
{
    source_cpu_t *cpu=&g_source_cpu[slot];
    cpu->reason=reason;
    source_cpu_status_t status=SOURCE_REJECTED;
    if (cpu->registered) {
        uint64_t disabled=UINT64_MAX;
        if (cpu_write_msr_safe(PVCLOCK_SYSTEM_TIME_MSR,0) &&
            cpu_read_msr_safe(PVCLOCK_SYSTEM_TIME_MSR,&disabled) && !(disabled & 1))
            cpu->registered=0;
        else status=SOURCE_QUARANTINED;
    }
    /* Even confirmed-disabled pages remain reserved. No page is reused. */
    __atomic_store_n(&cpu->status,status,__ATOMIC_RELEASE);
}

void clocksource_prepare_local(void)
{
    irq_flags_t flags=irq_save();
    cpu_slot_t slot;
    if (!smp_current_cpu_slot(&slot) || slot>=g_cpu_count || slot>=HOBBYOS_MAX_CPUS ||
        __atomic_load_n(&g_source_selected,__ATOMIC_ACQUIRE)) {
        irq_restore(flags);return;
    }
    source_cpu_t *cpu=&g_source_cpu[slot];
    if (__atomic_load_n(&cpu->status,__ATOMIC_ACQUIRE)!=SOURCE_UNPREPARED) {
        irq_restore(flags);return;
    }
    if (!source_features(&cpu->features)) {
        cpu->reason=SOURCE_REASON_FEATURES;
        __atomic_store_n(&cpu->status,SOURCE_UNSUPPORTED,__ATOMIC_RELEASE);
        irq_restore(flags);return;
    }
    if (flags & (UINT64_C(1)<<9)) {
        source_reject_local(slot,SOURCE_REASON_CONTEXT);irq_restore(flags);return;
    }
    uint64_t physical;
    if (!physical_image_address_range((uintptr_t)&g_source_pages[slot],
                                     sizeof(source_page_t),&physical) || (physical & 63)) {
        source_reject_local(slot,SOURCE_REASON_ADDRESS);irq_restore(flags);return;
    }
    cpu->physical=physical;
    cpu->status=SOURCE_REGISTERING;
    /* Conservatively retained even when WRMSR faults or readback fails. */
    cpu->registered=1;
    if (!cpu_write_msr_safe(PVCLOCK_SYSTEM_TIME_MSR,physical|1) ||
        !cpu_read_msr_safe(PVCLOCK_SYSTEM_TIME_MSR,&cpu->registration) ||
        cpu->registration!=(physical|1)) {
        source_reject_local(slot,SOURCE_REASON_MSR);irq_restore(flags);return;
    }
    int64_t lower=INT64_MIN,upper=INT64_MAX;
    for (unsigned point=0;point<CLOCKSOURCE_POINTS;++point) {
        if (point) {
            const pvclock_sample_t *previous=&cpu->points[point-1].after;
            bool reached=false;
            for (unsigned poll=0;poll<CLOCKSOURCE_POLL_LIMIT;++poll) {
                pvclock_sample_t now;
                if (pvclock_snapshot(&g_source_pages[slot].page,&now)!=PVCLOCK_OK ||
                    now.ns<previous->ns || now.tsc<previous->tsc) break;
                if (now.ns-previous->ns>=UINT64_C(20000000)) {reached=true;break;}
                spin_cpu_relax();
            }
            if (!reached) {
                source_reject_local(slot,SOURCE_REASON_COUNTER);irq_restore(flags);return;
            }
        }
        pvclock_reference_t *p=&cpu->points[point];
        bool valid=source_reference(slot,p);
        ++cpu->prepared_points;
        if (!valid || p->after.ns-p->before.ns>UINT64_C(100000)) {
            source_reject_local(slot,SOURCE_REASON_REFERENCE);irq_restore(flags);return;
        }
        if(!point && slot==smp_bsp_cpu_slot())g_source_origin_ns=p->ns;
        if (point && !pvclock_reference_rate(&cpu->points[point-1],p)) {
            source_reject_local(slot,SOURCE_REASON_RATE);irq_restore(flags);return;
        }
        int64_t lo,hi;
        if (!pvclock_reference_offset_at(p,g_source_origin_ns,&lo,&hi)) {
            source_reject_local(slot,SOURCE_REASON_REFERENCE);irq_restore(flags);return;
        }
        if(lo>lower)lower=lo;
        if(hi<upper)upper=hi;
        if(lower>upper) {
            source_reject_local(slot,SOURCE_REASON_OFFSET);irq_restore(flags);return;
        }
    }
    const pvclock_sample_t *last=&cpu->points[CLOCKSOURCE_POINTS-1].after;
    cpu->anchor=(pvclock_page_t){.version=last->version,.tsc_anchor=last->tsc,
        .ns_anchor=last->ns,.multiplier=last->multiplier,.shift=last->shift,.flags=last->flags};
    cpu->offset_lower=lower;cpu->offset_upper=upper;
    cpu->last_tsc=last->tsc;cpu->last_ns=last->ns;cpu->last_reference_ns=last->ns;
    cpu->previous_reference=cpu->points[CLOCKSOURCE_POINTS-1];
    __atomic_store_n(&cpu->status,SOURCE_QUALIFIED,__ATOMIC_RELEASE);
    irq_restore(flags);
}

void clocksource_select(void)
{
    irq_flags_t flags=irq_save();
    cpu_slot_t slot;
    if ((flags & (UINT64_C(1)<<9)) || !smp_current_cpu_slot(&slot) ||
        slot!=smp_bsp_cpu_slot() || !g_cpu_count || g_cpu_count>HOBBYOS_MAX_CPUS ||
        __atomic_exchange_n(&g_source_selected,1,__ATOMIC_ACQ_REL)) {
        irq_restore(flags);return;
    }
    int64_t lower=INT64_MIN,upper=INT64_MAX;
    bool valid=true;
    for(unsigned cpu=0;cpu<g_cpu_count;++cpu) {
        source_cpu_t *s=&g_source_cpu[cpu];
        if(__atomic_load_n(&s->status,__ATOMIC_ACQUIRE)!=SOURCE_QUALIFIED) {valid=false;continue;}
        if(s->offset_lower>lower)lower=s->offset_lower;
        if(s->offset_upper<upper)upper=s->offset_upper;
    }
    if(valid && lower<=upper) {
        g_source_offset=(int64_t)(((__int128)lower+upper)/2);
        __atomic_store_n(&g_source_epoch,2,__ATOMIC_RELEASE);
        __atomic_store_n(&g_source,CLOCKSOURCE_KVM,__ATOMIC_RELEASE);
    }
    irq_restore(flags);
}

static bool source_failed(source_cpu_t *cpu, source_reason_t reason)
{
    cpu->reason=reason;
    clocksource_invalidate();
    return false;
}

bool clocksource_read_local(cpu_slot_t slot, clocksource_sample_t *out)
{
    if (!out || irq_is_enabled() || slot>=g_cpu_count || slot>=HOBBYOS_MAX_CPUS ||
        clocksource_current()!=CLOCKSOURCE_KVM) return false;
    source_cpu_t *cpu=&g_source_cpu[slot];
    if(__atomic_load_n(&cpu->status,__ATOMIC_ACQUIRE)!=SOURCE_QUALIFIED)
        return source_failed(cpu,SOURCE_REASON_IDENTITY);
    if(__atomic_exchange_n(&cpu->busy,1,__ATOMIC_ACQUIRE))
        return source_failed(cpu,SOURCE_REASON_REENTRANT);
    pvclock_sample_t raw;
    source_reason_t failure=SOURCE_REASON_NONE;
    if(cpu->reads==UINT64_MAX)failure=SOURCE_REASON_OVERFLOW;
    else if(pvclock_snapshot(&g_source_pages[slot].page,&raw)!=PVCLOCK_OK)
        failure=SOURCE_REASON_SNAPSHOT;
    else if(raw.multiplier!=cpu->anchor.multiplier || raw.shift!=cpu->anchor.shift)
        failure=SOURCE_REASON_SCALE;
    else if(raw.tsc<=cpu->last_tsc || raw.ns<=cpu->last_ns)
        failure=SOURCE_REASON_COUNTER;
    uint64_t ns=0,predicted=0;
    if(!failure) {
        if(pvclock_interpolate(&cpu->anchor,raw.tsc,&predicted)!=PVCLOCK_OK)
            failure=SOURCE_REASON_ANCHOR;
        else {
            uint64_t error=raw.ns>=predicted ? raw.ns-predicted : predicted-raw.ns;
            uint64_t delta=raw.tsc-cpu->anchor.tsc_anchor;
            if(cpu->anchor.shift>=0) {
                if(delta>(UINT64_MAX>>cpu->anchor.shift))failure=SOURCE_REASON_OVERFLOW;
                else delta<<=cpu->anchor.shift;
            } else delta>>=-cpu->anchor.shift;
            if(!failure && error>CLOCKSOURCE_ANCHOR_ERROR_NS+(delta>>32)+1)
                failure=SOURCE_REASON_ANCHOR;
        }
    }
    if(!failure && raw.ns-cpu->last_reference_ns>=CLOCKSOURCE_REFERENCE_NS && cpu->references==UINT64_MAX)
        failure=SOURCE_REASON_OVERFLOW;
    if(!failure && raw.ns-cpu->last_reference_ns>=CLOCKSOURCE_REFERENCE_NS) {
        ++cpu->references;
        bool valid=source_reference(slot,&cpu->reference);
        const pvclock_reference_t *r=&cpu->reference;
        if(r->after.ns>=r->before.ns && r->after.ns-r->before.ns>cpu->maximum_reference_ns) {
            cpu->maximum_reference_ns=r->after.ns-r->before.ns;
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
            cpu->maximum_reference=*r;
            cpu->maximum_reference_previous=cpu->previous_reference;
#endif
        }
        if(!valid || !pvclock_reference_drift(&cpu->previous_reference,r) ||
            r->after.multiplier!=raw.multiplier || r->after.shift!=raw.shift)
            failure=SOURCE_REASON_REFERENCE;
        else {cpu->last_reference_ns=r->after.ns;cpu->previous_reference=*r;}
    }
    if(!failure && !pvclock_apply_offset(raw.ns,g_source_offset,&ns))failure=SOURCE_REASON_OVERFLOW;
    if(failure)source_failed(cpu,failure);
    bool valid=!failure && clocksource_current()==CLOCKSOURCE_KVM;
    if(valid) {
        cpu->last_tsc=raw.tsc;cpu->last_ns=raw.ns;++cpu->reads;
        *out=(clocksource_sample_t){.counter=raw.tsc,.ns=ns,.epoch=2,.cpu=slot,
            .retries=raw.retries,.source=CLOCKSOURCE_KVM};
    }
    __atomic_store_n(&cpu->busy,0,__ATOMIC_RELEASE);
    return valid;
}

#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
bool clocksource_terminal_capture(cpu_slot_t slot)
{
    if(irq_is_enabled() || slot>=g_cpu_count || slot>=HOBBYOS_MAX_CPUS ||
        __atomic_load_n(&g_source_terminal_ready[slot],__ATOMIC_ACQUIRE))return false;
    const source_cpu_t *cpu=&g_source_cpu[slot];
    if(__atomic_load_n(&cpu->busy,__ATOMIC_ACQUIRE))return false;
    g_source_terminal[slot]=(clocksource_terminal_t){.reads=cpu->reads,.references=cpu->references,
        .maximum_reference_ns=cpu->maximum_reference_ns,.last_tsc=cpu->last_tsc,.last_ns=cpu->last_ns,
        .epoch=clocksource_epoch(),.source=clocksource_current(),.reason=cpu->reason,
        .busy=cpu->busy,.registered=cpu->registered,.maximum=cpu->maximum_reference,
        .previous=cpu->maximum_reference_previous};
    __atomic_store_n(&g_source_terminal_ready[slot],1,__ATOMIC_RELEASE);
    return true;
}

const clocksource_terminal_t *clocksource_terminal_read(cpu_slot_t slot)
{
    return slot<g_cpu_count && slot<HOBBYOS_MAX_CPUS &&
        __atomic_load_n(&g_source_terminal_ready[slot],__ATOMIC_ACQUIRE) ? &g_source_terminal[slot] : NULL;
}
#endif

void clocksource_report(void)
{
    char line[640];
    int n=ksnprintf(line,sizeof(line),"[CLOCKSOURCE] version=3 source=%u epoch=%llu cpus=%u offset=%lld reference_ns=%llu origin_ns=%llu rate_ppm=%u bsp=%u\n",
        (unsigned)clocksource_current(),(unsigned long long)clocksource_epoch(),g_cpu_count,
        (long long)g_source_offset,(unsigned long long)CLOCKSOURCE_REFERENCE_NS,
        (unsigned long long)g_source_origin_ns,PVCLOCK_RATE_PPM,smp_bsp_cpu_slot());
    if(n>0 && (size_t)n<sizeof(line))serial_write_all(line);
    for(unsigned slot=0;slot<g_cpu_count && slot<HOBBYOS_MAX_CPUS;++slot) {
        const source_cpu_t *cpu=&g_source_cpu[slot];
        n=ksnprintf(line,sizeof(line),"[CLOCKSOURCE_CPU] version=3 cpu=%u status=%u reason=%u features=%u physical=%llu registration=%llu registered=%u points=%u lower=%lld upper=%lld\n",
            slot,cpu->status,cpu->reason,cpu->features,(unsigned long long)cpu->physical,
            (unsigned long long)cpu->registration,cpu->registered,cpu->prepared_points,
            (long long)cpu->offset_lower,(long long)cpu->offset_upper);
        if(n>0 && (size_t)n<sizeof(line))serial_write_all(line);
        for(unsigned point=0;point<cpu->prepared_points;++point) {
            const pvclock_reference_t *p=&cpu->points[point];
            n=ksnprintf(line,sizeof(line),"[CLOCKSOURCE_POINT] version=3 cpu=%u index=%u tsc0=%llu ns0=%llu ver0=%u mul0=%u shift0=%d flags0=%u retry0=%u tsc1=%llu ns1=%llu ver1=%u mul1=%u shift1=%d flags1=%u retry1=%u ticks=%llu ns=%llu period=%llu mode=%u retries=%u valid=%u\n",
                slot,point,(unsigned long long)p->before.tsc,(unsigned long long)p->before.ns,
                p->before.version,p->before.multiplier,p->before.shift,p->before.flags,p->before.retries,
                (unsigned long long)p->after.tsc,(unsigned long long)p->after.ns,
                p->after.version,p->after.multiplier,p->after.shift,p->after.flags,p->after.retries,
                (unsigned long long)p->ticks,(unsigned long long)p->ns,
                (unsigned long long)p->period_fs,p->mode,p->retries,p->valid);
            if(n>0 && (size_t)n<sizeof(line))serial_write_all(line);
        }
    }
}
