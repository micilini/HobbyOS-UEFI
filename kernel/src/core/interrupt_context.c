#include "interrupt_context.h"

#include "../libc/memory.h"
#include "../memory/heap.h"

#define INTERRUPT_CONTEXT_NEST_LIMIT 16u
#define INTERRUPT_CONTEXT_FIRST_NONE 256u

typedef struct
{
    volatile uint64_t entered[256];
    volatile uint64_t returned[256];
    volatile uint64_t entered_total;
    volatile uint64_t returned_total;
    volatile uint64_t unexpected;
    volatile uint64_t underflow;
    volatile uint64_t mismatch;
    volatile uint32_t first_vector;
    volatile uint32_t last_vector;
    volatile uint32_t depth;
    volatile uint32_t max_depth;
    volatile uint8_t preempt_epilogue;
    volatile uint8_t active_vectors[INTERRUPT_CONTEXT_NEST_LIMIT];
} interrupt_cpu_journal_t;

static interrupt_cpu_journal_t *g_interrupt_journals;
static uint32_t g_interrupt_journal_count;

static void interrupt_journal_reset(interrupt_cpu_journal_t *journal)
{
    memset(journal, 0, sizeof(*journal));
    journal->first_vector = INTERRUPT_CONTEXT_FIRST_NONE;
    journal->last_vector = INTERRUPT_CONTEXT_FIRST_NONE;
}

static void interrupt_journal_enter(interrupt_cpu_journal_t *journal,
                                    uint8_t vector)
{
    __atomic_add_fetch(&journal->entered[vector], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&journal->entered_total, 1, __ATOMIC_RELAXED);
    uint32_t expected = INTERRUPT_CONTEXT_FIRST_NONE;
    (void)__atomic_compare_exchange_n(&journal->first_vector, &expected,
                                      vector, false, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED);
    __atomic_store_n(&journal->last_vector, vector, __ATOMIC_RELAXED);
    uint32_t prior = __atomic_fetch_add(&journal->depth, 1,
                                        __ATOMIC_ACQ_REL);
    uint32_t depth = prior + 1u;
    if (prior < INTERRUPT_CONTEXT_NEST_LIMIT)
        journal->active_vectors[prior] = vector;
    else
        __atomic_add_fetch(&journal->mismatch, 1, __ATOMIC_RELAXED);
    uint32_t maximum = __atomic_load_n(&journal->max_depth,
                                       __ATOMIC_RELAXED);
    while (maximum < depth && !__atomic_compare_exchange_n(
        &journal->max_depth, &maximum, depth, false, __ATOMIC_RELAXED,
        __ATOMIC_RELAXED))
        ;
}

static void interrupt_journal_exit(interrupt_cpu_journal_t *journal,
                                   uint8_t vector)
{
    uint32_t depth = __atomic_load_n(&journal->depth, __ATOMIC_ACQUIRE);
    if (!depth)
    {
        __atomic_add_fetch(&journal->underflow, 1, __ATOMIC_RELAXED);
        return;
    }
    if (depth <= INTERRUPT_CONTEXT_NEST_LIMIT &&
        journal->active_vectors[depth - 1u] != vector)
        __atomic_add_fetch(&journal->mismatch, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&journal->returned[vector], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&journal->returned_total, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&journal->depth, 1, __ATOMIC_RELEASE);
}

static interrupt_cpu_journal_t *interrupt_current_journal(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!g_interrupt_journals || !smp_current_cpu_slot(&slot) ||
        slot >= g_interrupt_journal_count)
        return NULL;
    return &g_interrupt_journals[slot];
}

bool interrupt_context_init(uint32_t cpu_count)
{
    if (!cpu_count || g_interrupt_journals)
        return false;
    g_interrupt_journals =
        kmalloc(sizeof(interrupt_cpu_journal_t) * cpu_count);
    if (!g_interrupt_journals)
        return false;
    g_interrupt_journal_count = cpu_count;
    for (uint32_t slot = 0; slot < cpu_count; slot++)
        interrupt_journal_reset(&g_interrupt_journals[slot]);
    return true;
}

void interrupt_context_enter(uint8_t vector)
{
    interrupt_cpu_journal_t *journal = interrupt_current_journal();
    if (journal)
        interrupt_journal_enter(journal, vector);
}

void interrupt_context_exit(uint8_t vector)
{
    interrupt_cpu_journal_t *journal = interrupt_current_journal();
    if (journal)
        interrupt_journal_exit(journal, vector);
}

bool interrupt_context_exit_to_preempt(uint8_t vector)
{
    interrupt_cpu_journal_t *journal = interrupt_current_journal();
    if (!journal)
        return false;
    bool eligible = __atomic_load_n(&journal->depth, __ATOMIC_ACQUIRE) == 1u &&
                    __atomic_load_n(&journal->underflow,
                                    __ATOMIC_RELAXED) == 0 &&
                    __atomic_load_n(&journal->mismatch,
                                    __ATOMIC_RELAXED) == 0 &&
                    journal->active_vectors[0] == vector;
    interrupt_journal_exit(journal, vector);
    if (!eligible)
        return false;
    uint8_t expected = 0;
    return __atomic_compare_exchange_n(&journal->preempt_epilogue, &expected,
                                       1, false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
}

void interrupt_context_mark_unexpected(uint8_t vector)
{
    interrupt_cpu_journal_t *journal = interrupt_current_journal();
    if (!journal)
        return;
    __atomic_store_n(&journal->last_vector, vector, __ATOMIC_RELAXED);
    __atomic_add_fetch(&journal->unexpected, 1, __ATOMIC_RELAXED);
}

bool interrupt_context_snapshot(cpu_slot_t slot,
                                interrupt_cpu_snapshot_t *out)
{
    if (!out || !g_interrupt_journals || slot >= g_interrupt_journal_count)
        return false;
    interrupt_cpu_journal_t *journal = &g_interrupt_journals[slot];
    *out = (interrupt_cpu_snapshot_t){0};
    out->entered_total = __atomic_load_n(&journal->entered_total,
                                         __ATOMIC_ACQUIRE);
    out->returned_total = __atomic_load_n(&journal->returned_total,
                                          __ATOMIC_ACQUIRE);
    out->imbalance = out->entered_total >= out->returned_total
        ? out->entered_total - out->returned_total
        : out->returned_total - out->entered_total;
    out->unexpected = __atomic_load_n(&journal->unexpected,
                                      __ATOMIC_RELAXED);
    out->underflow = __atomic_load_n(&journal->underflow,
                                     __ATOMIC_RELAXED);
    out->mismatch = __atomic_load_n(&journal->mismatch,
                                    __ATOMIC_RELAXED);
    out->first_vector = __atomic_load_n(&journal->first_vector,
                                        __ATOMIC_RELAXED);
    out->last_vector = __atomic_load_n(&journal->last_vector,
                                       __ATOMIC_RELAXED);
    out->depth = __atomic_load_n(&journal->depth, __ATOMIC_ACQUIRE);
    out->max_depth = __atomic_load_n(&journal->max_depth,
                                     __ATOMIC_RELAXED);
    out->preempt_epilogue = __atomic_load_n(&journal->preempt_epilogue,
                                            __ATOMIC_ACQUIRE);
    out->initialized = 1;
    return true;
}

bool interrupt_context_snapshot_stable(cpu_slot_t slot,
                                       interrupt_cpu_snapshot_t *out,
                                       uint32_t attempts)
{
    if (!out || !attempts)
        return false;
    while (attempts--)
    {
        interrupt_cpu_snapshot_t snapshot;
        if (!interrupt_context_snapshot(slot, &snapshot))
            return false;
        if (snapshot.depth == 0 &&
            snapshot.entered_total == snapshot.returned_total &&
            snapshot.imbalance == 0 && !snapshot.preempt_epilogue)
        {
            *out = snapshot;
            return true;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}

bool interrupt_context_vector_snapshot(cpu_slot_t slot, uint8_t vector,
                                       uint64_t *out_entered,
                                       uint64_t *out_returned)
{
    if (!out_entered || !out_returned || !g_interrupt_journals ||
        slot >= g_interrupt_journal_count)
        return false;
    *out_entered = __atomic_load_n(
        &g_interrupt_journals[slot].entered[vector], __ATOMIC_RELAXED);
    *out_returned = __atomic_load_n(
        &g_interrupt_journals[slot].returned[vector], __ATOMIC_RELAXED);
    return true;
}

bool interrupt_context_can_preempt(void)
{
    interrupt_cpu_journal_t *journal = interrupt_current_journal();
    return journal &&
           __atomic_load_n(&journal->depth, __ATOMIC_ACQUIRE) == 1u &&
           __atomic_load_n(&journal->underflow, __ATOMIC_RELAXED) == 0 &&
           __atomic_load_n(&journal->mismatch, __ATOMIC_RELAXED) == 0;
}

bool interrupt_context_consume_preempt_epilogue(void)
{
    interrupt_cpu_journal_t *journal = interrupt_current_journal();
    return journal &&
           __atomic_exchange_n(&journal->preempt_epilogue, 0,
                               __ATOMIC_ACQ_REL) != 0 &&
           __atomic_load_n(&journal->depth, __ATOMIC_ACQUIRE) == 0 &&
           __atomic_load_n(&journal->underflow, __ATOMIC_RELAXED) == 0 &&
           __atomic_load_n(&journal->mismatch, __ATOMIC_RELAXED) == 0;
}

bool interrupt_context_reset(void)
{
    if (!g_interrupt_journals)
        return false;
    for (uint32_t slot = 0; slot < g_interrupt_journal_count; slot++)
        if (__atomic_load_n(&g_interrupt_journals[slot].depth,
                            __ATOMIC_ACQUIRE) != 0)
            return false;
    for (uint32_t slot = 0; slot < g_interrupt_journal_count; slot++)
        interrupt_journal_reset(&g_interrupt_journals[slot]);
    return true;
}

bool interrupt_context_selftest(void)
{
    static interrupt_cpu_journal_t journal;
    interrupt_journal_reset(&journal);
    interrupt_journal_enter(&journal, 34);
    interrupt_journal_enter(&journal, 35);
    bool ok = journal.first_vector == 34u && journal.last_vector == 35u &&
              journal.depth == 2u && journal.max_depth == 2u;
    interrupt_journal_exit(&journal, 35);
    interrupt_journal_exit(&journal, 34);
    ok = ok && journal.depth == 0 && journal.entered[34] == 1 &&
         journal.returned[34] == 1 && journal.entered[35] == 1 &&
         journal.returned[35] == 1 && journal.entered_total == 2 &&
         journal.returned_total == 2 && !journal.underflow &&
         !journal.mismatch;
    interrupt_journal_exit(&journal, 34);
    return ok && journal.underflow == 1;
}
