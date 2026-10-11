#ifndef HOBBYOS_PVCLOCK_H
#define HOBBYOS_PVCLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* KVM public system-time ABI; this is not the wall-clock or steal-time page. */
#define PVCLOCK_SYSTEM_TIME_MSR UINT32_C(0x4b564d01)
#define PVCLOCK_CPUID_BASE UINT32_C(0x40000000)
#define PVCLOCK_CPUID_FEATURES UINT32_C(0x40000001)
#define PVCLOCK_FEATURE_SYSTEM_TIME (UINT32_C(1) << 3)
#define PVCLOCK_FEATURE_STABLE (UINT32_C(1) << 24)
#define PVCLOCK_FLAG_STABLE 1u
#define PVCLOCK_FLAG_STOPPED 2u
#define PVCLOCK_SNAPSHOT_ATTEMPTS 4u
#define PVCLOCK_RATE_PPM 10000u

typedef struct {
    uint32_t version, reserved;
    uint64_t tsc_anchor, ns_anchor;
    uint32_t multiplier;
    int8_t shift;
    uint8_t flags, padding[2];
} pvclock_page_t;

typedef struct {
    uint64_t tsc, ns;
    uint32_t version, multiplier, retries;
    int8_t shift;
    uint8_t flags;
} pvclock_sample_t;

typedef struct {
    pvclock_sample_t before, after;
    uint64_t ticks, ns, period_fs;
    uint32_t mode, retries;
    bool valid;
} pvclock_reference_t;

typedef enum {
    PVCLOCK_OK = 0,
    PVCLOCK_UNAVAILABLE,
    PVCLOCK_BUSY,
    PVCLOCK_STOPPED,
    PVCLOCK_INVALID,
    PVCLOCK_OVERFLOW
} pvclock_status_t;

_Static_assert(sizeof(pvclock_page_t) == 32, "KVM system-time ABI size");
_Static_assert(offsetof(pvclock_page_t, tsc_anchor) == 8, "KVM TSC offset");
_Static_assert(offsetof(pvclock_page_t, ns_anchor) == 16, "KVM time offset");
_Static_assert(offsetof(pvclock_page_t, multiplier) == 24, "KVM scale offset");
_Static_assert(offsetof(pvclock_page_t, shift) == 28, "KVM shift offset");
_Static_assert(offsetof(pvclock_page_t, flags) == 29, "KVM flags offset");

bool pvclock_features(uint32_t maximum, uint32_t ebx, uint32_t ecx,
                      uint32_t edx, uint32_t features);
pvclock_status_t pvclock_interpolate(const pvclock_page_t *page,
                                     uint64_t tsc, uint64_t *ns);
pvclock_status_t pvclock_snapshot(const volatile pvclock_page_t *page,
                                  pvclock_sample_t *out);
bool pvclock_reference_bounds(const pvclock_reference_t *point,
                               int64_t *lower, int64_t *upper);
bool pvclock_reference_rate(const pvclock_reference_t *first,
                             const pvclock_reference_t *last);
bool pvclock_reference_drift(const pvclock_reference_t *first,
                              const pvclock_reference_t *last);
bool pvclock_reference_offset_at(const pvclock_reference_t *point,
                                  uint64_t reference_ns, int64_t *lower, int64_t *upper);
bool pvclock_apply_offset(uint64_t raw, int64_t offset, uint64_t *out);

#endif
