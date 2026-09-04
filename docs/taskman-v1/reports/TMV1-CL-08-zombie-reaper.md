# TMV1-CL-08 — ZOMBIE, cleanup e reaper seguro

## 1. Estado da tentativa

- Data: 2026-07-31.
- Branch: `feat/taskman`.
- Base completa: `e9834561d66b1bf55a8aed39d7bcde75c16baf7a`.
- Status: **REJECTED — gates funcionais obrigatórios ainda ausentes**.
- Commit: não criado, conforme a regra de falha da CL-08.
- Roadmap preexistente: preservado não rastreado e fora de stage.

## 2. Diagnóstico e arquitetura implementada

O reaper anterior aceitava ZOMBIE por estado, ausência de CPU e idade. A tentativa separou o bit de
política `GRACE` dos blockers estruturais, adicionou claim único sob o scheduler lock, remoção do registry
sob lock e liberação de stack/TCB fora dele. O batch passou a ser constante de file scope com limite 16.

O exit agora registra `exit_started_ns`, executa cleanup fora do scheduler lock, registra
`cleanup_completed_ns`, aloca `zombie_generation`, publica um evento copy-only fora do lock e somente
depois faz o commit de ZOMBIE com `zombie_entered_ns`. Falha de publish resulta em panic controlado.

Foi criada a API mínima de lifecycle com oito listeners, lock próprio, cópia local antes dos callbacks,
registro/unregister, detecção bounded de publicação duplicada e stats. O evento não contém `task_t *`.

Task-wake timers adquirem referência de geração ao arm, liberam no cancel PENDING ou após dispatch e
mantêm a referência enquanto CLAIMED. A ordem é `timer lock -> scheduler lock`; o reaper nunca toma o
timer lock. Um deadlock inicial no caminho prepare/arm foi encontrado em boot e corrigido com o helper
`scheduler_task_timer_ref_acquire_locked`.

O mask cobre estado, test hold, idle, exit, cleanup, reason, notification, CPU/current/slot, queue, wait,
object, deferred ready, timer refs, stack guard, RSP, claim, timestamp, clock e grace. Stats de scan,
backlog, claim, reap e deferrals foram adicionados. Snapshot expõe os novos metadados.

TASKMAN deixou de zerar artificialmente `kernel_mem_est_bytes` para ZOMBIE. `ps` já usava o valor real.

## 3. Arquivos alterados

- `AGENTS.md`
- `makefile`
- `kernel/src/core/kernel_init.c`
- `kernel/src/core/task.h`
- `kernel/src/core/scheduler.h`
- `kernel/src/core/scheduler.c`
- `kernel/src/core/timers.h`
- `kernel/src/core/timers.c`
- `kernel/src/core/task_lifecycle.h` (novo)
- `kernel/src/core/task_lifecycle.c` (novo)
- `kernel/src/shell/commands/cmd_taskman.c`
- `kernel/src/shell/commands/cmd_reaptest.h` (novo)
- `kernel/src/shell/commands/cmd_reaptest.c` (novo)
- `kernel/src/shell/commands/registry.c`
- este relatório.

## 4. Builds e hashes

- `make stack-check`: PASS; 75 arquivos, máximo 1920 bytes, limite 2048, zero violações.
- `make kernel-check JOBS=2`: PASS.
- `make kernel-check JOBS=$(nproc)`: PASS.
- Hash j2/jN, iguais por `cmp`: `58f46b9f4a1d090ad2690ac8016143552d043495dfcae92df678672cf77f9484`.
- `make deps-check`: PASS.
- `make image`: PASS.
- `nm -u kernel.elf`: vazio.
- Imagem desta tentativa: `d9d78c0c719cfc74c7cf6f94af87a364f8eab33a400cea7d9bc3d46afd5e76af`.

## 5. Resultados QEMU obtidos

- SMP=1 TCG: `reaptest all`, kill/scheduler/sync/account checks PASS.
- SMP=2 TCG: mesmos checks PASS.
- SMP=4 TCG: mesmos checks PASS.
- SMP=8 TCG: mesmos checks PASS.
- NORMAL 1000 em SMP=1: PASS, 1000 criadas/notificadas/reaped.
- KILLED 1000 em SMP=1: PASS, 1000 criadas/notificadas/reaped.
- Batch 65: PASS, cinco batches, máximo 16.
- Snapshot: 1000 iterações PASS no harness básico.
- Grace: somente a prova g0/test-hold passou; a medição temporal completa 0/1/3000 não foi feita.
- Logs: `artifacts/build/cl08-smp1.log` a `cl08-smp8.log`.

## 6. Gates ausentes/bloqueantes

Os comandos `reaptest cleanup`, `timer-ref`, `oncpu`, `stale` e `heap` ainda caem no check genérico;
portanto não certificam os cenários exigidos. Também não foram implementados/executados:

- hold de cleanup com watchdog e quarantine;
- timer real mantido CLAIMED durante ZOMBIE;
- avaliação controlada antes do finish físico;
- stale wake após reap;
- baseline/drift de heap;
- telemetria comparativa ps/TASKMAN para ZOMBIE;
- negatives AGE_ONLY, TIMER_REF_IGNORED, DUPLICATE_NOTIFICATION e ON_CPU;
- churn concorrente completo em SMP=4/8;
- soak contínuo SMP=8 de 180000 ms;
- recertificação integral `make test-cl07-final` após a alteração.

Essas ausências são bloqueantes pelos critérios fornecidos. Nenhum resultado foi inventado e nenhum
commit foi criado. Itens CL-09/CL-10 não foram iniciados.

## 7. Warnings e regressões

Os warnings de build observados são os preexistentes de `-Wall`; os gates estritos configurados passaram.
Os checks básicos CL-05/06/07 usados na matriz passaram, mas isso não substitui a matriz e o soak integrais.

## 8. Status final

**REJECTED.** Worktree preservado para continuação; CL-08 não está certificada e CL-09+ permanece bloqueada.

## Correção e certificação posterior

A primeira tentativa implementou a arquitetura, mas permaneceu rejeitada por testes placeholders e gates ausentes.
O resultado da tentativa de correção está em `TMV1-CL-08-FIX-final-certification.md`.
# Correção posterior

As tentativas de correção e certificação estão documentadas em
`TMV1-CL-08-FIX-final-certification.md` e
`TMV1-CL-08-FIX2-final-certification.md`.
