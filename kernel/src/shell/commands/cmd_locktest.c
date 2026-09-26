#include "cmd_locktest.h"

#include "cmd_accounttest.h"
#include "../../core/idt.h"
#include "../../core/scheduler.h"
#include "../../core/semaphore.h"
#include "../../core/spinlock.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"
#include "../../smp/smp_topology.h"
#include "../../libc/string.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef HOBBYOS_SELFTEST

#define LOCKTEST_MAX_WORKERS HOBBYOS_MAX_CPUS
#define LOCKTEST_READY_TIMEOUT_MS 10000u
#define LOCKTEST_REAP_TIMEOUT_MS 30000u
#define LOCKTEST_MAX_ITERATIONS 100000u

typedef struct
{
    uint32_t index;
    uint32_t iterations;
    uint64_t acquisitions;
} locktest_worker_t;

typedef struct
{
    spinlock_t lock;
    semaphore_t completion;
    volatile uint32_t ready;
    volatile uint32_t release;
    volatile uint32_t abort;
    volatile uint32_t active;
    volatile uint32_t max_active;
    volatile uint32_t inside;
    volatile uint64_t shared_counter;
    volatile uint64_t exclusive_violations;
    volatile uint64_t if_violations;
    volatile uint64_t slot_acquisitions[HOBBYOS_MAX_CPUS];
} locktest_state_t;

static locktest_state_t g_locktest;
static locktest_worker_t g_locktest_workers[LOCKTEST_MAX_WORKERS];
static task_handle_t g_locktest_handles[LOCKTEST_MAX_WORKERS];
static spinlock_t g_locktest_bss_lock;

static void locktest_write_u64(uint64_t value)
{
    char buffer[21];
    uint32_t count = 0;
    do
    {
        buffer[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (count != 0)
        serial_putc_all(buffer[--count]);
}

static bool locktest_parse_u32(const char *text, uint32_t maximum,
                               uint32_t *out)
{
    uint64_t value = 0;
    if (!text || !*text || !out)
        return false;
    while (*text)
    {
        if (*text < '0' || *text > '9')
            return false;
        value = value * 10u + (uint32_t)(*text++ - '0');
        if (!value || value > maximum)
            return false;
    }
    *out = (uint32_t)value;
    return true;
}

static void locktest_update_max(volatile uint32_t *maximum, uint32_t value)
{
    uint32_t current = __atomic_load_n(maximum, __ATOMIC_RELAXED);
    while (value > current &&
           !__atomic_compare_exchange_n(maximum, &current, value, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
    {
    }
}

static void locktest_worker(void *arg)
{
    locktest_worker_t *worker = (locktest_worker_t *)arg;
    uint32_t active = __atomic_add_fetch(&g_locktest.active, 1,
                                         __ATOMIC_ACQ_REL);
    locktest_update_max(&g_locktest.max_active, active);
    __atomic_add_fetch(&g_locktest.ready, 1, __ATOMIC_RELEASE);

    while (!__atomic_load_n(&g_locktest.release, __ATOMIC_ACQUIRE))
        schedule_voluntary();

    if (!__atomic_load_n(&g_locktest.abort, __ATOMIC_ACQUIRE))
    {
        for (uint32_t iteration = 0; iteration < worker->iterations;
             iteration++)
        {
            bool before_if = irq_is_enabled();
            irq_flags_t flags = spin_lock_irqsave(&g_locktest.lock);
            if (irq_is_enabled())
                __atomic_add_fetch(&g_locktest.if_violations, 1,
                                   __ATOMIC_RELAXED);
            if (__atomic_fetch_add(&g_locktest.inside, 1,
                                   __ATOMIC_RELAXED) != 0)
                __atomic_add_fetch(&g_locktest.exclusive_violations, 1,
                                   __ATOMIC_RELAXED);

            cpu_slot_t slot = CPU_SLOT_INVALID;
            if (!smp_current_cpu_slot(&slot) || slot >= g_cpu_count ||
                slot >= HOBBYOS_MAX_CPUS)
            {
                __atomic_add_fetch(&g_locktest.exclusive_violations, 1,
                                   __ATOMIC_RELAXED);
            }
            else
            {
                __atomic_add_fetch(&g_locktest.slot_acquisitions[slot], 1,
                                   __ATOMIC_RELAXED);
            }
            g_locktest.shared_counter++;
            worker->acquisitions++;

            if (__atomic_fetch_sub(&g_locktest.inside, 1,
                                   __ATOMIC_RELAXED) != 1)
                __atomic_add_fetch(&g_locktest.exclusive_violations, 1,
                                   __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&g_locktest.lock, flags);
            if (irq_is_enabled() != before_if ||
                (((flags >> 9) & 1u) != (uint64_t)before_if))
                __atomic_add_fetch(&g_locktest.if_violations, 1,
                                   __ATOMIC_RELAXED);

            if ((iteration & 63u) == 63u)
                schedule_voluntary();
        }
    }

    __atomic_sub_fetch(&g_locktest.active, 1, __ATOMIC_ACQ_REL);
    (void)sem_signal(&g_locktest.completion);
}

static bool locktest_bss_zero_contract(void)
{
    if (__atomic_load_n(&g_locktest_bss_lock.locked, __ATOMIC_RELAXED) != 0 ||
        __atomic_load_n(&g_locktest_bss_lock.tickets, __ATOMIC_RELAXED) != 0 ||
        !spin_trylock(&g_locktest_bss_lock))
        return false;
    spin_unlock(&g_locktest_bss_lock);
    spinlock_init(&g_locktest_bss_lock);
    return true;
}

static bool locktest_trylock_no_ticket(void)
{
    spinlock_init(&g_locktest.lock);
    irq_flags_t flags = spin_lock_irqsave(&g_locktest.lock);
    uint32_t before = __atomic_load_n(&g_locktest.lock.tickets,
                                      __ATOMIC_RELAXED);
    bool rejected = !spin_trylock(&g_locktest.lock);
    uint32_t after = __atomic_load_n(&g_locktest.lock.tickets,
                                     __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_locktest.lock, flags);
    spinlock_init(&g_locktest.lock);
    return rejected && before == after;
}

static int locktest_progress(uint32_t cpus, uint32_t iterations)
{
    bool static_zero = locktest_bss_zero_contract();
    bool trylock_no_ticket = locktest_trylock_no_ticket();
    spinlock_init(&g_locktest.lock);
    sem_init(&g_locktest.completion, 0);
    g_locktest.ready = 0;
    g_locktest.release = 0;
    g_locktest.abort = 0;
    g_locktest.active = 0;
    g_locktest.max_active = 0;
    g_locktest.inside = 0;
    g_locktest.shared_counter = 0;
    g_locktest.exclusive_violations = 0;
    g_locktest.if_violations = 0;
    for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
        g_locktest.slot_acquisitions[slot] = 0;

    bool observer = spinlock_test_observer_begin(&g_locktest.lock, cpus - 1u);
    uint32_t made = 0;
    scheduler_test_set_lifecycle_log_quiet(true);
    for (uint32_t worker = 0; worker < cpus; worker++)
    {
        g_locktest_workers[worker] = (locktest_worker_t){
            .index = worker,
            .iterations = iterations,
            .acquisitions = 0,
        };
        if (!thread_create_named_handle(locktest_worker,
                                        &g_locktest_workers[worker],
                                        "lock-progress",
                                        &g_locktest_handles[worker]))
            break;
        made++;
    }

    uint64_t deadline = timer_get_uptime_ms() + LOCKTEST_READY_TIMEOUT_MS;
    while (__atomic_load_n(&g_locktest.ready, __ATOMIC_ACQUIRE) < made &&
           timer_get_uptime_ms() < deadline)
        timer_sleep(1);

    bool ready = made == cpus &&
                 __atomic_load_n(&g_locktest.ready, __ATOMIC_ACQUIRE) == cpus;
    if (!ready)
        __atomic_store_n(&g_locktest.abort, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_locktest.release, 1, __ATOMIC_RELEASE);
    for (uint32_t worker = 0; worker < made; worker++)
        sem_wait(&g_locktest.completion);

    spinlock_test_observation_t observation = {0};
    bool observed = observer &&
                    spinlock_test_observer_end(&g_locktest.lock,
                                               &observation);
    accounttest_worker_quiescence_t quiescence = {0};
    bool reaped = made != 0 && accounttest_wait_workers_reaped(
        g_locktest_handles, made, LOCKTEST_REAP_TIMEOUT_MS, &quiescence);
    scheduler_test_set_lifecycle_log_quiet(false);

    uint64_t acquisitions = 0;
    bool per_worker = made == cpus;
    for (uint32_t worker = 0; worker < made; worker++)
    {
        acquisitions += g_locktest_workers[worker].acquisitions;
        if (g_locktest_workers[worker].acquisitions != iterations)
            per_worker = false;
        serial_write_all("[LOCKTEST][WORKER] index=");
        locktest_write_u64(worker);
        serial_write_all(" acquisitions=");
        locktest_write_u64(g_locktest_workers[worker].acquisitions);
        serial_write_all("\n");
    }

    uint64_t slot_mask = 0;
    uint32_t slots = 0;
    for (uint32_t slot = 0; slot < cpus; slot++)
    {
        uint64_t count = __atomic_load_n(&g_locktest.slot_acquisitions[slot],
                                         __ATOMIC_RELAXED);
        if (count != 0)
        {
            slot_mask |= 1ULL << slot;
            slots++;
        }
        serial_write_all("[LOCKTEST][CPU] slot=");
        locktest_write_u64(slot);
        serial_write_all(" acquisitions=");
        locktest_write_u64(count);
        serial_write_all("\n");
    }

    uint64_t expected = (uint64_t)cpus * iterations;
    bool lock_drained =
        __atomic_load_n(&g_locktest.lock.locked, __ATOMIC_RELAXED) == 0 &&
        (uint16_t)__atomic_load_n(&g_locktest.lock.tickets,
                                  __ATOMIC_RELAXED) ==
            (uint16_t)(__atomic_load_n(&g_locktest.lock.tickets,
                                       __ATOMIC_RELAXED) >> 16);
    bool ok = static_zero && trylock_no_ticket && ready && observed && reaped &&
              quiescence.handles_gone == cpus && per_worker &&
              acquisitions == expected && g_locktest.shared_counter == expected &&
              g_locktest.exclusive_violations == 0 &&
              g_locktest.if_violations == 0 && g_locktest.inside == 0 &&
              g_locktest.active == 0 && g_locktest.max_active == cpus &&
              observation.observations == expected &&
              observation.contended != 0 && observation.violations == 0 &&
              observation.max_distance <= cpus - 1u && slots == cpus &&
              lock_drained;

    serial_write_all("[LOCKTEST][PROGRESS] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" cpus="); locktest_write_u64(cpus);
    serial_write_all(" iterations="); locktest_write_u64(iterations);
    serial_write_all(" acquisitions="); locktest_write_u64(acquisitions);
    serial_write_all(" exclusive_violations=");
    locktest_write_u64(g_locktest.exclusive_violations);
    serial_write_all(" if_violations=");
    locktest_write_u64(g_locktest.if_violations);
    serial_write_all(" max_queue_distance=");
    locktest_write_u64(observation.max_distance);
    serial_write_all(" limit="); locktest_write_u64(cpus - 1u);
    serial_write_all(" slots="); locktest_write_u64(slots);
    serial_write_all(" slot_mask="); locktest_write_u64(slot_mask);
    serial_write_all(" observer_samples=");
    locktest_write_u64(observation.observations);
    serial_write_all(" contended=");
    locktest_write_u64(observation.contended);
    serial_write_all(" trylock_no_ticket=");
    locktest_write_u64(trylock_no_ticket);
    serial_write_all(" static_zero="); locktest_write_u64(static_zero);
    serial_write_all(" reaped=");
    locktest_write_u64(quiescence.handles_gone);
    serial_write_all(" max_active=");
    locktest_write_u64(g_locktest.max_active);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

int cmd_locktest(int argc, char **argv)
{
    uint32_t cpus;
    uint32_t iterations;
    if (argc != 4 || strcmp(argv[1], "progress") != 0 ||
        !locktest_parse_u32(argv[2], HOBBYOS_MAX_CPUS, &cpus) ||
        !locktest_parse_u32(argv[3], LOCKTEST_MAX_ITERATIONS, &iterations) ||
        cpus != g_cpu_count)
    {
        serial_write_all(
            "[LOCKTEST][PROGRESS] FAIL reason=usage-or-cpu-count\n");
        return 1;
    }
    return locktest_progress(cpus, iterations);
}

#else

int cmd_locktest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

#endif
