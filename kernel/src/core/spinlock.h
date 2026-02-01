#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "idt.h"

typedef struct
{
    volatile uint32_t locked;
} spinlock_t;

void spinlock_init(spinlock_t *lock);

int spin_trylock(spinlock_t *lock);
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

irq_flags_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags);

static inline void spin_cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

#endif