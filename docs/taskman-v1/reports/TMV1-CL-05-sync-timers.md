# TMV1-CL-05 — Wait queues, semáforos e timers atômicos

- Data: 2026-07-30
- Branch: `feat/taskman`
- Base: `517efc90019192834227c7a0dcb7eaf6505c2c9a`
- Status: implementação concluída; validação básica SMP concluída.

## Races e protocolo

A base liberava o lock do semáforo antes de publicar o waiter, armava sleeps antes do estado SLEEPING e guardava `task_t *` em callbacks futuros. O novo fluxo é:

```text
object/timer lock -> scheduler_prepare_block (scheduler lock mantido)
                  -> unlock do objeto sem irqrestore
                  -> scheduler_commit_block -> switch CL-04
                  -> finish na stack de next libera scheduler lock
                  -> task retomada consome a primeira wake e caller restaura flags
```

`scheduler_abort_block` desfaz a publicação sem switch. A ordem oficial é objeto/timer antes do scheduler; poll libera o timer lock antes de acordar por identidade. O scheduler nunca toma o timer lock.

## Wait e wake

Cada task possui `lifecycle_generation`, `wait_generation`, kind, reason e active. O token de stack vincula task, lifecycle e wait generation. SIGNAL, TIMEOUT e CANCELLED disputam sob o scheduler lock; somente a primeira causa altera estado. Acordar uma task ainda on-CPU usa `deferred_ready`, consumido pelo finish CL-04. O retorno limpa os metadados novamente sob o scheduler lock.

## Semáforos

`count` contém apenas permits disponíveis. `sem_try_wait` consome sob o lock. `sem_wait_interruptible` publica a wait antes de liberar o lock do objeto. `sem_signal` entrega diretamente a um waiter elegível sem incrementar count; sem waiter, incrementa com proteção de overflow. O wrapper legado não bloqueia novamente em cancel/error.

## Timers

Handles têm ID e geração monotônicos não zero. Nodes têm estado PENDING/CLAIMED/CANCELLED e kind CALLBACK/TASK_WAKE. Sleep armazena somente task ID + lifecycle + wait generation. Cancel e claim são serializados pelo timer lock; dispatch e free ocorrem fora dele. Deadline com overflow retorna erro. OOM ocorre antes do prepare e não bloqueia. Wake tardia é lookup seguro/no-op. `ms=0` é yield. O fallback sem task e o caminho normal preservam flags.

Callbacks genéricos XHCI ainda usam `void *ctx`; são callbacks síncronos do subsistema e não representam lifetime de task. Nenhum callback de sleep recebe `task_t *`.

## Implementação

Arquivos: task/scheduler/semaphore/timers, wrapper do driver timer, registry, `cmd_synctest`, makefile e AGENTS. O finish CL-04 agora torna fatal uma falha impossível de requeue, liberando o lock antes do panic. Telemetria cobre waits/wakes e timers; `synctest` fornece check, sem, boundary, sleep, cancel, oom, stale, all e stats.

## Evidência

- `kernel-check JOBS=2`: PASS; SHA-256 intermediário `94f1e01868b93694a99654c8018cf39834c1634eeec53b9d87246f2aa01f812f`.
- Imagem: PASS (`34e83e051c896d28e119872a43b4861bd4a439e8b3b68cc83f4273acf38e250b`).
- QEMU TCG SMP 1/2/4/8: shell alcançada; WAIT_TIMER_GUARDS_OK; sleep 1/2/10/100 PASS; OOM PASS; stale PASS; synctest check PASS; schedtest check PASS; zero panic/#PF/#GP/FATAL.
- Sem 100000: PASS em SMP 1/2/4/8, `waits=signals=100000`, permits/lost/duplicate zero (teste básico de contador).
- started/finished: 290/290, 320/320, 272/272, 286/286 respectivamente; pending e violations zero.
- Logs: `artifacts/build/cl05-smp{1,2,4,8}-serial.log`.

## Gates estáticos

Os gates confirmam ausência de callback sleep convertendo ctx em task pointer, ausência de `thread_block` nos caminhos migrados, presença de prepare/commit/abort, identidade/generations e estados/handles/cancel de timer. Os `void *ctx` restantes são timers genéricos XHCI.

## Warnings e riscos reservados

Somente warnings legados XHCI/unused, `.note.GNU-stack` e LOAD RWX. Accounting, lifecycle geral, killability completa, reaper reference accounting e input router permanecem para CL-06/07/08/09. CL-06 não foi iniciada.

O stress concorrente prolongado, boundary instrumentado e teste negativo determinístico exigidos pelo plano não foram representados como aprovados neste relatório sem evidência; o status final deve refletir esses gates se não forem executados antes do commit.

## Correção e homologação posterior

A primeira entrega foi rejeitada porque os testes concorrentes bloqueadores não foram executados.
A homologação final está documentada em `TMV1-CL-05-FIX-concurrency-validation.md`.
