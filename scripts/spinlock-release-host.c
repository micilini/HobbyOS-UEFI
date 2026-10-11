#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "../kernel/src/core/spinlock.h"

#define THREADS 8u
#define ITERATIONS 20000u
static _Thread_local irq_flags_t host_flags = 0x246;
static unsigned arrivals, compare_calls;

irq_flags_t irq_save(void)
{ irq_flags_t f=host_flags; host_flags &= ~UINT64_C(512); return f; }
void irq_restore(irq_flags_t f) { host_flags=f; }
bool irq_is_enabled(void) { return (host_flags & 512)!=0; }

/* Only the incoming control substitutes its release CAS. Each injected
 * contender atomically reserves a real next ticket before the actual CAS. */
static bool __attribute__((unused)) release_compare(volatile uint32_t *p,
    uint32_t *expected, uint32_t desired, bool weak, int success, int failure)
{
    ++compare_calls;
    if (arrivals) { --arrivals; __atomic_fetch_add(p,UINT32_C(0x10000),__ATOMIC_RELAXED); }
    return __atomic_compare_exchange_n(p,expected,desired,weak,success,failure);
}

#ifndef SPINLOCK_ACTUAL_SOURCE
#define SPINLOCK_ACTUAL_SOURCE "../kernel/src/core/spinlock.c"
#endif
#include SPINLOCK_ACTUAL_SOURCE

static void boundaries(bool incoming)
{
    spinlock_t lock={0};
    spin_lock(&lock);
    arrivals=32; compare_calls=0;
    spin_unlock(&lock);
    if (incoming) {
        assert(compare_calls==33 && arrivals==0 && lock.tickets==0x210001);
    } else {
        assert(compare_calls==0 && arrivals==32 && lock.tickets==0x10001);
    }
    arrivals=0;
    /* Owner wrap must not change reservations in the high half. */
    for (unsigned next=0;next<65536;next+=257) {
        lock=(spinlock_t){.locked=1,.tickets=(next<<16)|0xffff};
        spin_unlock(&lock);
        assert(lock.tickets==(next<<16) && !lock.locked);
    }
    lock=(spinlock_t){.tickets=0xfffefffe};
    for (unsigned i=0;i<4;++i) { assert(spin_trylock(&lock)); spin_unlock(&lock); }
    assert(lock.tickets==0x00020002);
    for (unsigned i=0;i<2;++i) {
        host_flags=i ? 0x246 : 0x46;
        irq_flags_t f=spin_lock_irqsave(&lock);
        assert(!irq_is_enabled()); uint32_t queued=lock.tickets;
        assert(!spin_trylock(&lock) && lock.tickets==queued);
        spin_unlock_irqrestore(&lock,f);
        assert(host_flags==(i ? 0x246u : 0x46u));
    }
}

static spinlock_t shared;
static pthread_barrier_t barrier;
static uint64_t total, payload=UINT64_C(0xf00d1234), per_thread[THREADS];
static unsigned inside;
static void *worker(void *arg)
{
    unsigned id=(unsigned)(uintptr_t)arg;
    (void)pthread_barrier_wait(&barrier);
    for (unsigned i=0;i<ITERATIONS;++i) {
        irq_flags_t f=spin_lock_irqsave(&shared);
        assert(!irq_is_enabled());
        assert(__atomic_fetch_add(&inside,1,__ATOMIC_RELAXED)==0);
        assert(payload==(total^UINT64_C(0xf00d1234)));
        ++total; payload=total^UINT64_C(0xf00d1234); ++per_thread[id];
        assert(__atomic_fetch_sub(&inside,1,__ATOMIC_RELAXED)==1);
        spin_unlock_irqrestore(&shared,f);
        assert(host_flags==0x246);
    }
    return NULL;
}

static unsigned fifo_position, fifo_order[THREADS];
static void *fifo_worker(void *arg)
{
    spin_lock(&shared);
    assert(fifo_position<THREADS); fifo_order[fifo_position++]=(unsigned)(uintptr_t)arg;
    spin_unlock(&shared); return NULL;
}

static uint64_t monotonic(void)
{
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}

int main(void)
{
#ifdef INCOMING_RELEASE_CONTROL
    boundaries(true);
    puts("Incoming ticket release: PASS forced_arrivals=32 release_attempts=33");
#else
    boundaries(false);
    pthread_t t[THREADS]; spinlock_init(&shared); spin_lock(&shared);
    for (unsigned i=0;i<THREADS;++i) {
        assert(pthread_create(&t[i],NULL,fifo_worker,(void *)(uintptr_t)i)==0);
        uint64_t deadline=monotonic()+UINT64_C(10000000000);
        while ((__atomic_load_n(&shared.tickets,__ATOMIC_ACQUIRE)>>16)!=i+2) {
            assert(monotonic()<deadline); sched_yield();
        }
    }
    spin_unlock(&shared);
    for (unsigned i=0;i<THREADS;++i) { assert(pthread_join(t[i],NULL)==0); assert(fifo_order[i]==i); }
    spinlock_init(&shared); assert(pthread_barrier_init(&barrier,NULL,THREADS)==0);
    assert(spinlock_test_observer_begin(&shared,THREADS-1));
    for (unsigned i=0;i<THREADS;++i) assert(pthread_create(&t[i],NULL,worker,(void *)(uintptr_t)i)==0);
    for (unsigned i=0;i<THREADS;++i) { assert(pthread_join(t[i],NULL)==0); assert(per_thread[i]==ITERATIONS); }
    spinlock_test_observation_t seen;
    assert(spinlock_test_observer_end(&shared,&seen));
    assert(seen.observations==THREADS*ITERATIONS && seen.contended && !seen.violations);
    assert(total==THREADS*ITERATIONS && !inside);
    assert((shared.tickets>>16)==(uint16_t)total && (uint16_t)shared.tickets==(uint16_t)total);
    assert(pthread_barrier_destroy(&barrier)==0);
    puts("Ticket release actual x86: PASS FIFO=8 acquisitions=160000 publication=CHECKED wrap=CHECKED flags=EXACT");
#endif
    return 0;
}
