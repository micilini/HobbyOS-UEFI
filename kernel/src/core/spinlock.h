#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "idt.h"

typedef struct
{
    /* Compatibility/diagnostic state: zero while free, one while owned. */
    volatile uint32_t locked;
    /* Low 16 bits: owner ticket. High 16 bits: next ticket. */
    volatile uint32_t tickets;
} spinlock_t;

/*
 * FIFO ticket lock. A zero-filled object is an initialized, unlocked lock.
 * Recursive acquisition is unsupported. In particular, callers must never
 * acquire the same lock recursively from NMI context.
 */

void spinlock_init(spinlock_t *lock);

int spin_trylock(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

irq_flags_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags);

#ifdef HOBBYOS_SELFTEST
typedef struct
{
    uint64_t observations;
    uint64_t contended;
    uint64_t violations;
    uint32_t max_distance;
    uint32_t limit;
} spinlock_test_observation_t;

bool spinlock_test_observer_begin(spinlock_t *lock, uint32_t limit);
bool spinlock_test_observer_end(spinlock_t *lock,
                                spinlock_test_observation_t *out);
#endif

static inline void spin_cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline int spin_trylock_irqsave(spinlock_t *lock, irq_flags_t *out_flags)
{
    irq_flags_t f = irq_save();
    if (spin_trylock(lock))
    {
        *out_flags = f;
        return 1;
    }
    irq_restore(f);
    return 0;
}

#endif
