#include "scheduler.h"
#include "task.h"
#include "spinlock.h"
#include "../memory/heap.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "timers.h"
#include "../apic/lapic.h"

#include "panic.h"
#include "../drivers/serial.h"
#include "interrupts.h"

#define STACK_SIZE (16 * 1024)
#define MAX_CPUS 256

extern void thread_wrapper(void);


/* Duas filas: interativos rodam primeiro, depois normais */
static struct list_head g_interactive_queue;
static struct list_head g_normal_queue;
static spinlock_t g_scheduler_lock;


static task_t *g_current_task_map[MAX_CPUS] = {0};


static task_t *g_idle_task_map[MAX_CPUS] = {0};

static int g_next_tid = 1;


static task_t* create_idle_task(void) {
    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    if (!idle) return NULL;

    memset(idle, 0, sizeof(task_t));

    idle->id = 0;
    strcpy(idle->name, "Idle");
    idle->state = TASK_RUNNING;
    idle->stack_base = NULL;
    idle->rsp = 0;
    idle->quantum = DEFAULT_QUANTUM;
    idle->quantum_default = DEFAULT_QUANTUM;
    idle->task_class = TASK_CLASS_NORMAL;

    list_init(&idle->list);
    return idle;
}

/* Helper: retorna a fila correta para a classe da task */
static struct list_head *queue_for_class(int task_class)
{
    if (task_class == TASK_CLASS_INTERACTIVE)
        return &g_interactive_queue;
    return &g_normal_queue;
}

/* Helper: pega a próxima task priorizando interativos */
static task_t *pick_next_task(void)
{
    /* Interativos primeiro */
    if (!list_empty(&g_interactive_queue))
    {
        struct list_head *node = g_interactive_queue.next;
        task_t *t = list_entry(node, task_t, list);
        list_del(&t->list);
        return t;
    }

    /* Depois normais */
    if (!list_empty(&g_normal_queue))
    {
        struct list_head *node = g_normal_queue.next;
        task_t *t = list_entry(node, task_t, list);
        list_del(&t->list);
        return t;
    }

    return NULL;
}

/* Helper: re-enfileira task na fila correta */
static void enqueue_task(task_t *t)
{
    struct list_head *q = queue_for_class(t->task_class);
    list_add_tail(&t->list, q);
}


void scheduler_init(void)
{
    spinlock_init(&g_scheduler_lock);
    list_init(&g_interactive_queue);
    list_init(&g_normal_queue);


    uint32_t bsp_id = lapic_get_id();

    task_t *idle = create_idle_task();
    if (!idle) {
        kpanic("SCHED: Failed to create BSP Idle Task");
    }

    g_idle_task_map[bsp_id] = idle;
    g_current_task_map[bsp_id] = idle;

    console_write_debug("[SCHED] SMP Scheduler initialized (Priority Queues).\n");
}


void scheduler_init_ap(void)
{
    uint32_t id = lapic_get_id();

    task_t *idle = create_idle_task();
    if (!idle) {
        while(1) __asm__ volatile("cli; hlt");
    }

    idle->state = TASK_RUNNING;
    g_idle_task_map[id] = idle;
    g_current_task_map[id] = idle;
}

task_t *get_current_task(void)
{
    uint32_t id = lapic_get_id();
    return g_current_task_map[id];
}

task_t *thread_create_with_class(void (*entry_point)(void *), void *arg, int task_class)
{
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(task_t));

    t->stack_base = kmalloc_aligned(STACK_SIZE, 16);
    if (!t->stack_base)
    {
        kfree(t);
        return NULL;
    }
    memset(t->stack_base, 0, STACK_SIZE);


    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    t->id = g_next_tid++;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    t->state = TASK_READY;
    t->cr3 = 0;
    t->task_class = task_class;

    if (task_class == TASK_CLASS_INTERACTIVE)
    {
        t->quantum = INTERACTIVE_QUANTUM;
        t->quantum_default = INTERACTIVE_QUANTUM;
    }
    else
    {
        t->quantum = DEFAULT_QUANTUM;
        t->quantum_default = DEFAULT_QUANTUM;
    }

    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack_base + STACK_SIZE);

    sp--;
    *sp = (uint64_t)thread_wrapper;
    sp--;
    *sp = 0;
    sp--;
    *sp = 0;
    sp--;
    *sp = (uint64_t)entry_point;
    sp--;
    *sp = (uint64_t)arg;
    sp--;
    *sp = 0;
    sp--;
    *sp = 0;

    t->rsp = (uint64_t)sp;

    list_init(&t->list);


    flags = spin_lock_irqsave(&g_scheduler_lock);
    enqueue_task(t);
    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    console_write_debug("[SCHED] Thread created. ID=");
    console_print_dec_debug(t->id);
    console_write_debug(task_class == TASK_CLASS_INTERACTIVE ? " [INTERACTIVE]\n" : "\n");

    return t;
}

task_t *thread_create(void (*entry_point)(void *), void *arg)
{
    return thread_create_with_class(entry_point, arg, TASK_CLASS_NORMAL);
}

void thread_block(wait_queue_t *wq, task_state_t state)
{

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    task_t *current = get_current_task();


    if (current == g_idle_task_map[lapic_get_id()]) {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }

    current->state = state;


    if (current->list.next != NULL && current->list.prev != NULL) {
        list_del(&current->list);
    }

    if (wq)
    {
        list_add_tail(&current->list, &wq->head);
    }
    else
    {
        list_init(&current->list);
    }

    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    schedule_voluntary();
}

void thread_wake(task_t *t)
{
    if (!t) return;

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (t->state == TASK_READY || t->state == TASK_RUNNING)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }


    if (t->list.next != NULL && t->list.prev != NULL) {
        list_del(&t->list);
    }

    t->state = TASK_READY;
    enqueue_task(t);

    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

int thread_wake_one(wait_queue_t *wq)
{
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (list_empty(&wq->head))
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return 0;
    }

    struct list_head *node = wq->head.next;
    task_t *t = list_entry(node, task_t, list);

    list_del(&t->list);
    t->state = TASK_READY;
    enqueue_task(t);

    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    return 1;
}

/*
 * thread_exit — termina a thread atual de forma limpa.
 * Marca como ZOMBIE e faz schedule para nunca mais voltar.
 * A memória da stack/task fica "vazando" por ora (sem reaper thread),
 * mas o sistema não trava.
 */
void thread_exit(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    task_t *current = get_current_task();

    if (current && current != g_idle_task_map[lapic_get_id()])
    {
        current->state = TASK_ZOMBIE;

        if (current->list.next != NULL && current->list.prev != NULL)
        {
            list_del(&current->list);
        }
        list_init(&current->list);
    }

    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    schedule_voluntary();

    /* Nunca deveria chegar aqui, mas por segurança: */
    while (1) __asm__ volatile("cli; hlt");
}

/*
 * schedule_impl — implementacao interna do scheduler.
 *
 * voluntary=0: chamado pelo timer IRQ (preempcao). Respeita quantum.
 * voluntary=1: chamado por thread_block/timer_sleep/yield. Sempre troca.
 *
 * INVARIANTE CRITICO para SMP:
 *   IRQs ficam DESABILITADAS durante todo o switch_context.
 *   Usamos spin_unlock (sem irq_restore) antes do switch para liberar
 *   o lock sem habilitar IRQs. Apos switch_context retornar no novo
 *   contexto, fazemos irq_restore explicitamente.
 */
void schedule_impl(int voluntary)
{
    uint32_t my_id = lapic_get_id();
    task_t *prev = g_current_task_map[my_id];
    task_t *next = NULL;
    task_t *my_idle = g_idle_task_map[my_id];

    if (my_idle == NULL)
    {
        serial_write_all("[SCHED] FATAL: my_idle == NULL. CPU=0x");
        serial_write_hex64_all((uint64_t)my_id);
        serial_write_all(" (scheduler_init_ap probably stored idle on wrong APIC id)\n");
        kpanic("SCHED: my_idle == NULL");
    }

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    // AP entrou no scheduler sem current inicializado (bug real de ordem de init/timer).
    if (prev == NULL)
    {
        serial_write_all("[SCHED] WARN: prev==NULL, anchoring to per-cpu idle. CPU=0x");
        serial_write_hex64_all((uint64_t)my_id);
        serial_write_all("\n");

        prev = my_idle;
        g_current_task_map[my_id] = prev;

        if (prev)
        {
            prev->state = TASK_RUNNING;
            if (prev->quantum <= 0)
                prev->quantum = prev->quantum_default;
        }
    }

    /* Contabilidade de quantum — apenas para preempcao por timer */
    if (!voluntary && prev && prev != my_idle && prev->state == TASK_RUNNING)
    {
        prev->quantum--;
        if (prev->quantum > 0)
        {
            /* Mas: se há uma task interativa esperando e prev é normal,
             * preempta imediatamente (interativo "fura a fila"). */
            if (prev->task_class != TASK_CLASS_INTERACTIVE &&
                !list_empty(&g_interactive_queue))
            {
                /* Força preempção — cai no código abaixo */
            }
            else
            {
                spin_unlock_irqrestore(&g_scheduler_lock, flags);
                return;
            }
        }
    }

    next = pick_next_task();

    if (next == NULL)
    {
        next = my_idle;
    }

    if (prev != next)
    {
        if (prev && prev->state == TASK_RUNNING && prev != my_idle)
        {
            prev->state = TASK_READY;
            prev->quantum = prev->quantum_default;
            enqueue_task(prev);
        }
        else if (prev == my_idle && prev)
        {
            prev->state = TASK_READY;
        }

        if (next)
        {
            next->state = TASK_RUNNING;
            if (next->quantum <= 0)
                next->quantum = next->quantum_default;
        }

        g_current_task_map[my_id] = next;
    }

    if (prev != next)
    {
        /*
         * Libera o lock MAS mantem IRQs desabilitadas.
         * spin_unlock so libera o lock, nao toca em RFLAGS.
         */
        spin_unlock(&g_scheduler_lock);

        if (!next)
        {
            serial_write_all("[SCHED] FATAL: next == NULL\n");
            kpanic("SCHED: next == NULL");
        }

        uint64_t nrsp = next->rsp;

        if ((nrsp < 0x100000ULL) ||
            (nrsp >= 0x0000800000000000ULL && nrsp < 0xFFFF800000000000ULL))
        {
            serial_write_all("\n[SCHED] FATAL: next->rsp looks CORRUPTED (range)\n");

            serial_write_all("[SCHED] CPU=0x");
            serial_write_hex64_all((uint64_t)lapic_get_id());

            serial_write_all(" prev=0x");
            serial_write_hex64_all((uint64_t)prev);

            serial_write_all(" next=0x");
            serial_write_hex64_all((uint64_t)next);

            serial_write_all("\n");

            if (prev)
            {
                serial_write_all("[SCHED] prev.id=0x");
                serial_write_hex64_all((uint64_t)prev->id);

                serial_write_all(" prev.rsp=0x");
                serial_write_hex64_all((uint64_t)prev->rsp);

                serial_write_all(" prev.stack=0x");
                serial_write_hex64_all((uint64_t)prev->stack_base);

                serial_write_all("\n");
            }

            serial_write_all("[SCHED] next.id=0x");
            serial_write_hex64_all((uint64_t)next->id);

            serial_write_all(" next.rsp=0x");
            serial_write_hex64_all((uint64_t)next->rsp);

            serial_write_all(" next.stack=0x");
            serial_write_hex64_all((uint64_t)next->stack_base);

            serial_write_all("\n");

            kpanic("SCHED: next->rsp corrupted (range) (see serial)");
        }

        /* switch_context com IRQs OFF */
        switch_context(prev, next);

        /*
         * Voltamos aqui quando 'prev' é re-escalonado por outro CPU.
         */
        irq_restore(flags);
    }
    else
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
    }
}

/* Chamado pelo loop BSP/AP (fallback) */
void schedule(void)
{
    schedule_impl(0);
}

/* Chamado por thread_block, timer_sleep, yield (voluntario) */
void schedule_voluntary(void)
{
    schedule_impl(1);
}

/*
 * scheduler_preempt_from_irq — chamado pelo stub assembly irq_timer_entry.
 */
void scheduler_preempt_from_irq(void)
{
    extern volatile int g_system_ready_for_scheduling;

    if (!g_system_ready_for_scheduling)
        return;

    uint32_t id = lapic_get_id();

    if (id < MAX_CPUS && interrupts_consume_reschedule())
    {
        schedule_impl(0);
    }
}