#include "spinlock.h"

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
}

int spin_trylock(spinlock_t *lock)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(
        &lock->locked,
        &expected,
        1,
        0,
        __ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED);
}

void spin_lock(spinlock_t *lock)
{
    while (!spin_trylock(lock))
    {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
        {
            spin_cpu_relax();
        }
    }
}

void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
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