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

/*
 * spin_trylock_irqsave — tenta adquirir o lock com IRQs desabilitadas.
 * Retorna 1 e salva flags em *out_flags se conseguiu.
 * Retorna 0 (e restaura flags) se o lock já estava ocupado.
 * Usado em contexto de IRQ onde não podemos ficar girando.
 */
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