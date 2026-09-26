#include "spinlock.h"
#include "../smp/cpu_limits.h"

#define SPINLOCK_OWNER_MASK 0x0000FFFFu
#define SPINLOCK_NEXT_ONE   0x00010000u
#define SPINLOCK_TICKET_MAX 0xFFFFu

_Static_assert(HOBBYOS_MAX_CPUS * (IDT_ENTRIES + 1u) <= SPINLOCK_TICKET_MAX,
               "ticket width cannot represent maximum nested contenders");

static inline uint16_t spinlock_owner(uint32_t state)
{
    return (uint16_t)(state & SPINLOCK_OWNER_MASK);
}

static inline uint16_t spinlock_next(uint32_t state)
{
    return (uint16_t)(state >> 16);
}

#ifdef HOBBYOS_SELFTEST
typedef struct
{
    spinlock_t *target;
    volatile uint64_t observations;
    volatile uint64_t contended;
    volatile uint64_t violations;
    volatile uint32_t max_distance;
    uint32_t limit;
    volatile uint8_t active;
} spinlock_test_observer_state_t;

static spinlock_test_observer_state_t g_spinlock_test_observer;

static void spinlock_test_observe(spinlock_t *lock, uint16_t distance)
{
    if (__atomic_load_n(&g_spinlock_test_observer.active,
                        __ATOMIC_ACQUIRE) != 2 ||
        __atomic_load_n(&g_spinlock_test_observer.target,
                        __ATOMIC_RELAXED) != lock)
    {
        return;
    }

    __atomic_add_fetch(&g_spinlock_test_observer.observations, 1,
                       __ATOMIC_RELAXED);
    if (distance != 0)
    {
        __atomic_add_fetch(&g_spinlock_test_observer.contended, 1,
                           __ATOMIC_RELAXED);
    }
    if ((uint32_t)distance > g_spinlock_test_observer.limit)
    {
        __atomic_add_fetch(&g_spinlock_test_observer.violations, 1,
                           __ATOMIC_RELAXED);
    }

    uint32_t current = __atomic_load_n(
        &g_spinlock_test_observer.max_distance, __ATOMIC_RELAXED);
    while ((uint32_t)distance > current &&
           !__atomic_compare_exchange_n(
               &g_spinlock_test_observer.max_distance, &current,
               (uint32_t)distance, false, __ATOMIC_RELAXED,
               __ATOMIC_RELAXED))
    {
    }
}

bool spinlock_test_observer_begin(spinlock_t *lock, uint32_t limit)
{
    uint8_t expected = 0;
    if (!lock || limit > SPINLOCK_TICKET_MAX ||
        !__atomic_compare_exchange_n(&g_spinlock_test_observer.active,
                                     &expected, 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    {
        return false;
    }

    __atomic_store_n(&g_spinlock_test_observer.target, lock,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_spinlock_test_observer.observations, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_spinlock_test_observer.contended, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_spinlock_test_observer.violations, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_spinlock_test_observer.max_distance, 0,
                     __ATOMIC_RELAXED);
    g_spinlock_test_observer.limit = limit;
    __atomic_store_n(&g_spinlock_test_observer.active, 2,
                     __ATOMIC_RELEASE);
    return true;
}

bool spinlock_test_observer_end(spinlock_t *lock,
                                spinlock_test_observation_t *out)
{
    if (!lock || !out ||
        __atomic_load_n(&g_spinlock_test_observer.active,
                        __ATOMIC_ACQUIRE) != 2 ||
        __atomic_load_n(&g_spinlock_test_observer.target,
                        __ATOMIC_RELAXED) != lock)
    {
        return false;
    }

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    out->observations = __atomic_load_n(
        &g_spinlock_test_observer.observations, __ATOMIC_RELAXED);
    out->contended = __atomic_load_n(
        &g_spinlock_test_observer.contended, __ATOMIC_RELAXED);
    out->violations = __atomic_load_n(
        &g_spinlock_test_observer.violations, __ATOMIC_RELAXED);
    out->max_distance = __atomic_load_n(
        &g_spinlock_test_observer.max_distance, __ATOMIC_RELAXED);
    out->limit = g_spinlock_test_observer.limit;
    __atomic_store_n(&g_spinlock_test_observer.target, (spinlock_t *)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_spinlock_test_observer.active, 0,
                     __ATOMIC_RELEASE);
    return true;
}
#endif

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
    lock->tickets = 0;
}

int spin_trylock(spinlock_t *lock)
{
    uint32_t expected = __atomic_load_n(&lock->tickets, __ATOMIC_RELAXED);
    if (spinlock_owner(expected) != spinlock_next(expected))
    {
        return 0;
    }

    uint32_t desired = expected + SPINLOCK_NEXT_ONE;
    if (!__atomic_compare_exchange_n(&lock->tickets, &expected, desired,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
    {
        return 0;
    }
    __atomic_store_n(&lock->locked, 1, __ATOMIC_RELAXED);
    return 1;
}

void spin_lock(spinlock_t *lock)
{
    uint32_t state = __atomic_fetch_add(&lock->tickets, SPINLOCK_NEXT_ONE,
                                        __ATOMIC_RELAXED);
    uint16_t ticket = spinlock_next(state);
#ifdef HOBBYOS_SELFTEST
    uint16_t distance = (uint16_t)(ticket - spinlock_owner(state));
    spinlock_test_observe(lock, distance);
#endif
    while (spinlock_owner(__atomic_load_n(&lock->tickets,
                                          __ATOMIC_ACQUIRE)) != ticket)
    {
        spin_cpu_relax();
    }
    __atomic_store_n(&lock->locked, 1, __ATOMIC_RELAXED);
}

void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELAXED);

    uint32_t expected = __atomic_load_n(&lock->tickets, __ATOMIC_RELAXED);
    for (;;)
    {
        uint32_t desired = (expected & ~SPINLOCK_OWNER_MASK) |
                           (uint16_t)(spinlock_owner(expected) + 1u);
        if (__atomic_compare_exchange_n(&lock->tickets, &expected, desired,
                                        false, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED))
        {
            return;
        }
    }
}

irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    irq_flags_t flags = irq_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{
    spin_unlock(lock);
    irq_restore(flags);
}
