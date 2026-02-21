#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task.h"


void scheduler_init(void);


void scheduler_init_ap(void);

task_t *thread_create(void (*entry_point)(void *), void *arg);

/* Cria thread com classe de prioridade específica */
task_t *thread_create_with_class(void (*entry_point)(void *), void *arg, int task_class);

void schedule(void);
void schedule_voluntary(void);

/* Chamado pelo stub assembly irq_timer_entry DENTRO do contexto de IRQ.
 * Checa need_resched + quantum e, se necessário, faz switch_context. */
void scheduler_preempt_from_irq(void);

/* Implementação interna — exposta para scheduler_preempt_from_irq */
void schedule_impl(int voluntary);

task_t *get_current_task(void);

void thread_block(wait_queue_t *wq, task_state_t state);

int thread_wake_one(wait_queue_t *wq);

void thread_wake(task_t *t);

/* Termina a thread atual de forma limpa */
void thread_exit(void);

#endif