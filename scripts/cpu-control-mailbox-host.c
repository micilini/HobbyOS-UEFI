#define _POSIX_C_SOURCE 200809L
#define HOBBYOS_SELFTEST 1
#define HOBBYOS_CPU_CONTROL_HOST_TEST 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../kernel/src/cpu/cpu_control_snapshot.c"

/* Real request/publication code with explicitly substituted privileged reads,
 * interrupt flags, clocksource and IPI delivery. Pthreads overlap claimed slots. */
SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
uint32_t g_cpu_count, g_bsp_apic_id;
volatile int g_panic_in_progress;
static _Thread_local uint32_t local_slot;
static _Thread_local irq_flags_t local_flags = 0x202;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_t workers[2];
static unsigned spawned, entered, completed, reads, overlap;
static bool concurrent, delay_writer, release_writer, invalid_identity, send_failure;
static bool runtime_ready=true;

irq_flags_t irq_save(void) { irq_flags_t f = local_flags; local_flags &= ~0x200u; return f; }
void irq_restore(irq_flags_t f) { local_flags = f; }
bool irq_is_enabled(void) { return !!(local_flags & 0x200); }
bool runtime_ready_is_test_ready(void) { return runtime_ready; }
bool irq_bootstrap_services_are_ready(void) { return true; }
bool clock_monotonic_is_ready(void) { return true; }
uint64_t clock_monotonic_ns(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
bool lapic_is_x2apic(void) { return false; }
bool cpu_local_arch_id(uint32_t *id) { *id = local_slot * 2; return !invalid_identity; }
bool smp_cpu_slot_from_apic_id(uint32_t id, cpu_slot_t *slot)
{ *slot = id / 2; return id % 2 == 0 && *slot < g_cpu_count; }
const SmpCpuInfo *smp_cpu_by_slot_const(cpu_slot_t slot) { return &g_cpus[slot]; }

void cpu_control_read_local(cpu_control_sample_t *out)
{
    __atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED);
    assert(!(local_flags & 0x200));
    if (local_slot && (concurrent || delay_writer))
    {
        assert(!pthread_mutex_lock(&mutex));
        entered |= 1u << local_slot;
        pthread_cond_broadcast(&condition);
        if (concurrent)
        {
            while ((entered & 6) != 6) pthread_cond_wait(&condition, &mutex);
            overlap |= 1u << local_slot;
            if (local_slot == 1)
                while (!(completed & 4)) pthread_cond_wait(&condition, &mutex);
        }
        if (delay_writer)
            while (!release_writer) pthread_cond_wait(&condition, &mutex);
        pthread_mutex_unlock(&mutex);
    }
    *out = (cpu_control_sample_t){.slot = local_slot, .apic_id = local_slot * 2,
        .identity_valid = true, .is_bsp = local_slot == 0, .paging = {.efer_valid = true}};
}
static void *deliver(void *argument)
{
    local_slot = (uint32_t)(uintptr_t)argument;
    cpu_control_snapshot_irq();
    unsigned before = __atomic_load_n(&reads, __ATOMIC_RELAXED);
    cpu_control_snapshot_irq(); /* Duplicate cannot recapture a published slot. */
    assert(__atomic_load_n(&reads, __ATOMIC_RELAXED) >= before);
    assert(local_flags == 0x202);
    pthread_mutex_lock(&mutex); completed |= 1u << local_slot;
    pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex);
    return NULL;
}
lapic_delivery_result_t lapic_send_ipi(uint32_t id, uint32_t vector)
{
    assert(vector == INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE && spawned < 2);
    assert(!pthread_create(&workers[spawned++], NULL, deliver, (void *)(uintptr_t)(id / 2)));
    return send_failure ? LAPIC_DELIVERY_INDETERMINATE : LAPIC_DELIVERY_SUBMITTED;
}
static void reset(unsigned cpus)
{
    for (unsigned i = 0; i < spawned; i++) assert(!pthread_join(workers[i], NULL));
    spawned = entered = completed = reads = overlap = 0;
    concurrent = delay_writer = release_writer = invalid_identity = send_failure = false;
    runtime_ready=true;
    g_active_generation = g_next_generation = g_owner = 0; g_quarantined = false;
    memset(g_slot_publication, 0, sizeof(g_slot_publication));
    memset(&g_capture, 0, sizeof(g_capture)); g_cpu_count = cpus;
    for (unsigned i = 0; i < cpus; i++)
        g_cpus[i] = (SmpCpuInfo){.slot = i, .apic_id = i * 2, .state = CPU_STATE_ONLINE};
}
int main(void)
{
    reset(24);runtime_ready=false;
    const cpu_control_capture_t *unready=cpu_control_snapshot();
    assert(unready && !unready->complete && !unready->generation && !unready->target_mask && !reads && !spawned);
    assert(!strcmp(unready->reason,"startup_or_panic"));cpu_control_snapshot_release();
    reset(1); g_owner = 1; assert(!cpu_control_snapshot()); g_owner = 0;
    g_next_generation = UINT64_MAX >> 2;
    assert(!strcmp(cpu_control_snapshot()->reason, "generation_exhausted"));
    assert(!g_active_generation); cpu_control_snapshot_release();
    reset(1); g_active_generation = 2; g_slot_publication[0] = (1u << 2) | 1;
    cpu_control_snapshot_irq(); assert(reads == 0); /* Stale observation. */
    g_slot_publication[0] = (2u << 2) | 1; invalid_identity = true;
    cpu_control_snapshot_irq(); assert(reads == 0);
    invalid_identity = false; cpu_control_snapshot_irq(); cpu_control_snapshot_irq();
    assert(reads == 1 && g_slot_publication[0] == ((2u << 2) | 3));
    g_active_generation = 0; cpu_control_snapshot_irq(); assert(reads == 1);
    for (unsigned repeat = 0; repeat < 3; repeat++)
    {
        reset(3); concurrent = true;
        const cpu_control_capture_t *c = cpu_control_snapshot();
        assert(c && c->complete && c->complete_mask == 7 && c->error_mask == 0);
        assert(overlap == 6 && reads == 3 && !g_active_generation);
        assert(cpu_control_snapshot() == NULL); cpu_control_snapshot_release();
    }
    reset(2); send_failure = true;
    const cpu_control_capture_t *failed_send = cpu_control_snapshot();
    assert(failed_send && !failed_send->complete && failed_send->error_mask == 2);
    cpu_control_snapshot_release();
    reset(2); delay_writer = true;
    const cpu_control_capture_t *c = cpu_control_snapshot();
    assert(c && !c->complete && !strcmp(c->reason, "missing_cpu_response"));
    assert(entered == 2 && g_quarantined && !g_active_generation);
    uint64_t generation = c->generation;
    cpu_control_snapshot_release();
    assert(cpu_control_snapshot() == c && c->generation == generation);
    assert(!strcmp(c->reason, "previous_timeout_quarantined"));
    cpu_control_snapshot_release();
    pthread_mutex_lock(&mutex); release_writer = true;
    pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex);
    for (unsigned i = 0; i < spawned; i++) assert(!pthread_join(workers[i], NULL));
    assert(c->generation == generation && c->cpus[1].identity_valid && g_quarantined);
    puts("{\"status\":\"PASS\",\"scope\":\"real_C_mailbox_substituted_privileged_operations\",\"overlapping_publications\":3,\"timeout_late_storage\":\"quarantined\"}");
    return 0;
}
