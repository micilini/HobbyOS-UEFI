#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../kernel/src/core/spinlock.h"

#define HOST_THREADS 8u
#define HOST_ITERATIONS 20000u

static _Thread_local bool g_irq_enabled = true;
static spinlock_t g_static_zero;
static spinlock_t g_lock;
static pthread_barrier_t g_barrier;
static uint64_t g_counter;
static volatile uint32_t g_inside;
static volatile uint64_t g_exclusion_failures;
static uint64_t g_counts[HOST_THREADS];

irq_flags_t irq_save(void)
{
    irq_flags_t flags = g_irq_enabled ? 1ULL << 9 : 0;
    g_irq_enabled = false;
    return flags;
}

void irq_restore(irq_flags_t flags)
{
    g_irq_enabled = (flags & (1ULL << 9)) != 0;
}

bool irq_is_enabled(void)
{
    return g_irq_enabled;
}

void init_idt(void) {}
void idt_load(void) {}
void enable_interrupts(void) { g_irq_enabled = true; }
void disable_interrupts(void) { g_irq_enabled = false; }

static void *worker(void *opaque)
{
    uintptr_t index = (uintptr_t)opaque;
    (void)pthread_barrier_wait(&g_barrier);
    for (uint32_t iteration = 0; iteration < HOST_ITERATIONS; iteration++)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_lock);
        if (irq_is_enabled() ||
            __atomic_fetch_add(&g_inside, 1, __ATOMIC_RELAXED) != 0)
            __atomic_add_fetch(&g_exclusion_failures, 1, __ATOMIC_RELAXED);
        g_counter++;
        g_counts[index]++;
        if (__atomic_fetch_sub(&g_inside, 1, __ATOMIC_RELAXED) != 1)
            __atomic_add_fetch(&g_exclusion_failures, 1, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&g_lock, flags);
        if (!irq_is_enabled())
            __atomic_add_fetch(&g_exclusion_failures, 1, __ATOMIC_RELAXED);
        if ((iteration & 255u) == 255u)
            sched_yield();
    }
    return NULL;
}

static bool test_static_zero(void)
{
    if (g_static_zero.locked != 0 || g_static_zero.tickets != 0 ||
        !spin_trylock(&g_static_zero))
        return false;
    spin_unlock(&g_static_zero);
    return g_static_zero.locked == 0 &&
           (uint16_t)g_static_zero.tickets ==
               (uint16_t)(g_static_zero.tickets >> 16);
}

static bool test_wrap(void)
{
    spinlock_t lock = {.locked = 0, .tickets = 0xFFFEFFFEu};
    for (uint32_t index = 0; index < 4; index++)
    {
        if (!spin_trylock(&lock))
            return false;
        spin_unlock(&lock);
    }
    return lock.locked == 0 && (uint16_t)lock.tickets == 2u &&
           (uint16_t)(lock.tickets >> 16) == 2u;
}

static bool test_trylock_no_ticket(void)
{
    spinlock_t lock = {0};
    spin_lock(&lock);
    uint32_t before = lock.tickets;
    bool rejected = !spin_trylock(&lock);
    uint32_t after = lock.tickets;
    spin_unlock(&lock);
    return rejected && before == after;
}

static bool test_irq_restore(void)
{
    spinlock_t lock = {0};
    g_irq_enabled = false;
    irq_flags_t disabled = spin_lock_irqsave(&lock);
    spin_unlock_irqrestore(&lock, disabled);
    if (g_irq_enabled)
        return false;
    g_irq_enabled = true;
    irq_flags_t enabled = spin_lock_irqsave(&lock);
    spin_unlock_irqrestore(&lock, enabled);
    return g_irq_enabled;
}

int main(void)
{
    pthread_t threads[HOST_THREADS];
    spinlock_test_observation_t observation = {0};
    bool static_zero = test_static_zero();
    bool wrap = test_wrap();
    bool trylock = test_trylock_no_ticket();
    bool irq_restore_ok = test_irq_restore();
    spinlock_init(&g_lock);
    bool observer_started = spinlock_test_observer_begin(&g_lock,
                                                          HOST_THREADS - 1u);
    if (pthread_barrier_init(&g_barrier, NULL, HOST_THREADS) != 0)
        return 2;
    for (uintptr_t index = 0; index < HOST_THREADS; index++)
        if (pthread_create(&threads[index], NULL, worker,
                           (void *)index) != 0)
            return 2;
    for (uint32_t index = 0; index < HOST_THREADS; index++)
        if (pthread_join(threads[index], NULL) != 0)
            return 2;
    (void)pthread_barrier_destroy(&g_barrier);
    bool observer_ended = spinlock_test_observer_end(&g_lock, &observation);

    bool counts = true;
    for (uint32_t index = 0; index < HOST_THREADS; index++)
        counts = counts && g_counts[index] == HOST_ITERATIONS;
    bool exclusive = g_exclusion_failures == 0 && g_inside == 0 &&
                     g_counter == (uint64_t)HOST_THREADS * HOST_ITERATIONS &&
                     counts;
    bool progress = observer_started && observer_ended &&
                    observation.observations == g_counter &&
                    observation.contended != 0 &&
                    observation.violations == 0 &&
                    observation.max_distance <= HOST_THREADS - 1u;
    bool pass = static_zero && wrap && trylock && irq_restore_ok && exclusive &&
                progress;
    printf("[LOCKTEST][HOST] %s static_zero=%u wrap=%u "
           "trylock_no_ticket=%u irq_restore=%u exclusion=%u progress=%u "
           "threads=%u iterations=%u acquisitions=%llu max_distance=%u\n",
           pass ? "PASS" : "FAIL", static_zero, wrap, trylock,
           irq_restore_ok, exclusive, progress, HOST_THREADS, HOST_ITERATIONS,
           (unsigned long long)g_counter, observation.max_distance);
    return pass ? 0 : 1;
}
