#include "pvclock.h"
#include "irq_latency_tsc.h"

_Static_assert(PVCLOCK_RATE_PPM && 1000000u % PVCLOCK_RATE_PPM == 0,
               "reference reserve must have an exact integer divisor");

bool pvclock_features(uint32_t maximum, uint32_t ebx, uint32_t ecx,
                      uint32_t edx, uint32_t features)
{
    return maximum >= PVCLOCK_CPUID_FEATURES && maximum < UINT32_C(0x40000100) &&
        ebx == UINT32_C(0x4b4d564b) && ecx == UINT32_C(0x564b4d56) &&
        edx == UINT32_C(0x0000004d) &&
        (features & (PVCLOCK_FEATURE_SYSTEM_TIME | PVCLOCK_FEATURE_STABLE)) ==
        (PVCLOCK_FEATURE_SYSTEM_TIME | PVCLOCK_FEATURE_STABLE);
}

pvclock_status_t pvclock_interpolate(const pvclock_page_t *page,
                                     uint64_t tsc, uint64_t *ns)
{
    if (!page || !ns || !page->version || (page->version & 1) ||
        !page->multiplier || !(page->flags & PVCLOCK_FLAG_STABLE) ||
        (page->flags & ~(PVCLOCK_FLAG_STABLE | PVCLOCK_FLAG_STOPPED)) ||
        tsc < page->tsc_anchor || page->shift <= -64 || page->shift >= 64)
        return PVCLOCK_INVALID;
    if (page->flags & PVCLOCK_FLAG_STOPPED) return PVCLOCK_STOPPED;
    uint64_t cycles = tsc - page->tsc_anchor;
    if (page->shift >= 0) {
        if (cycles > (UINT64_MAX >> page->shift)) return PVCLOCK_OVERFLOW;
        cycles <<= page->shift;
    } else cycles >>= -page->shift;
    __uint128_t product = (__uint128_t)cycles * page->multiplier;
    product >>= 32;
    if (product > UINT64_MAX || (uint64_t)product > UINT64_MAX - page->ns_anchor)
        return PVCLOCK_OVERFLOW;
    *ns = page->ns_anchor + (uint64_t)product;
    return PVCLOCK_OK;
}

pvclock_status_t pvclock_snapshot(const volatile pvclock_page_t *page,
                                  pvclock_sample_t *out)
{
    if (!page || !out) return PVCLOCK_INVALID;
    for (unsigned attempt = 0; attempt < PVCLOCK_SNAPSHOT_ATTEMPTS; ++attempt) {
        uint32_t first = __atomic_load_n(&page->version, __ATOMIC_ACQUIRE);
        if (!first || (first & 1)) continue;
        pvclock_page_t copy = {.version = first};
        copy.tsc_anchor = __atomic_load_n(&page->tsc_anchor, __ATOMIC_RELAXED);
        copy.ns_anchor = __atomic_load_n(&page->ns_anchor, __ATOMIC_RELAXED);
        copy.multiplier = __atomic_load_n(&page->multiplier, __ATOMIC_RELAXED);
        copy.shift = __atomic_load_n(&page->shift, __ATOMIC_RELAXED);
        copy.flags = __atomic_load_n(&page->flags, __ATOMIC_RELAXED);
        uint64_t tsc = irq_latency_read_tsc();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&page->version, __ATOMIC_ACQUIRE) != first) continue;
        uint64_t ns;
        pvclock_status_t status = pvclock_interpolate(&copy, tsc, &ns);
        if (status != PVCLOCK_OK) return status;
        *out = (pvclock_sample_t){.tsc=tsc,.ns=ns,.version=first,
            .multiplier=copy.multiplier,.shift=copy.shift,.flags=copy.flags,
            .retries=attempt};
        return PVCLOCK_OK;
    }
    return PVCLOCK_BUSY;
}

bool pvclock_apply_offset(uint64_t raw, int64_t offset, uint64_t *out)
{
    if (!out) return false;
    if (offset >= 0) {
        if (raw > UINT64_MAX - (uint64_t)offset) return false;
        *out = raw + (uint64_t)offset;
    } else {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
        if (raw < magnitude) return false;
        *out = raw - magnitude;
    }
    return true;
}

bool pvclock_reference_bounds(const pvclock_reference_t *p,
                               int64_t *lower, int64_t *upper)
{
    if (!p || !lower || !upper || !p->valid || !p->period_fs ||
        p->period_fs > UINT64_C(100000000) || !p->ticks || !p->ns ||
        p->ns > INT64_MAX || p->before.ns > INT64_MAX || p->after.ns > INT64_MAX ||
        (p->mode != 1 && p->mode != 2) || p->retries > 4 ||
        (p->mode == 2 && p->retries) ||
        !p->before.multiplier || p->before.multiplier != p->after.multiplier ||
        p->before.shift<=-64 || p->before.shift>=64 ||
        p->before.shift != p->after.shift ||
        p->before.retries >= PVCLOCK_SNAPSHOT_ATTEMPTS ||
        p->after.retries >= PVCLOCK_SNAPSHOT_ATTEMPTS ||
        p->before.flags != PVCLOCK_FLAG_STABLE || p->after.flags != PVCLOCK_FLAG_STABLE ||
        !p->before.version || !p->after.version ||
        (p->before.version & 1) || (p->after.version & 1) ||
        p->before.tsc >= p->after.tsc || !p->before.ns || p->before.ns >= p->after.ns)
        return false;
    int64_t uncertainty = (int64_t)((p->period_fs + 999999) / 1000000);
    int64_t lo = (int64_t)p->ns - (int64_t)p->after.ns;
    int64_t hi = (int64_t)p->ns - (int64_t)p->before.ns;
    if (lo < INT64_MIN + uncertainty || hi > INT64_MAX - uncertainty) return false;
    *lower = lo - uncertainty;
    *upper = hi + uncertainty;
    return true;
}

bool pvclock_reference_offset_at(const pvclock_reference_t *p,
                                  uint64_t reference_ns, int64_t *lower, int64_t *upper)
{
    int64_t lo,hi;
    if(!lower || !upper || !reference_ns || reference_ns>INT64_MAX ||
        !pvclock_reference_bounds(p,&lo,&hi))return false;
    uint64_t elapsed=p->ns>=reference_ns ? p->ns-reference_ns : reference_ns-p->ns;
    const uint64_t divisor=1000000u/PVCLOCK_RATE_PPM;
    uint64_t reserve=elapsed/divisor+(elapsed%divisor!=0);
    if(reserve>INT64_MAX || lo<INT64_MIN+(int64_t)reserve || hi>INT64_MAX-(int64_t)reserve)return false;
    *lower=lo-(int64_t)reserve;*upper=hi+(int64_t)reserve;
    return true;
}

bool pvclock_reference_drift(const pvclock_reference_t *a,
                              const pvclock_reference_t *b)
{
    int64_t lo, hi;
    if (!pvclock_reference_bounds(a, &lo, &hi) ||
        !pvclock_reference_bounds(b, &lo, &hi) || a->mode != b->mode ||
        a->period_fs != b->period_fs || b->ticks <= a->ticks ||
        b->ns <= a->ns || b->before.ns <= a->after.ns ||
        b->before.tsc <= a->after.tsc ||
        a->before.multiplier != b->before.multiplier || a->before.shift != b->before.shift)
        return false;
    uint64_t delta = b->ns - a->ns;
    uint64_t minimum = b->before.ns - a->after.ns;
    uint64_t maximum = b->after.ns - a->before.ns;
    uint64_t uncertainty=2*((a->period_fs+999999)/1000000);
    if(delta<=uncertainty)return false;
    const uint64_t divisor=1000000u/PVCLOCK_RATE_PPM;
    uint64_t high=delta+uncertainty,low=delta-uncertainty;
    return minimum>=high-high/divisor && maximum<=low+low/divisor;
}

bool pvclock_reference_rate(const pvclock_reference_t *a,
                             const pvclock_reference_t *b)
{
    return pvclock_reference_drift(a,b) && b->ns-a->ns>=UINT64_C(10000000) &&
        b->ns-a->ns<=UINT64_C(100000000) &&
        a->after.ns-a->before.ns<=UINT64_C(100000) &&
        b->after.ns-b->before.ns<=UINT64_C(100000);
}
