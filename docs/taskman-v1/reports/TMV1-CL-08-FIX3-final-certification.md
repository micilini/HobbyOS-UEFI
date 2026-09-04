# TMV1-CL-08-FIX3 — certificação final

Data: 2026-07-31
Branch: `feat/taskman`
Base: `e9834561d66b1bf55a8aed39d7bcde75c16baf7a`
Status: **REJECTED**

## Diagnóstico de starvation

`shell-thread` é INTERACTIVE em `kernel_init.c`; o fixture antigo era NORMAL; e
`runqueue_take_next_locked()` examina a fila INTERACTIVE antes da NORMAL. O antigo
`wait_state_scheduled()` mantinha a shell runnable e apenas chamava
`schedule_voluntary()`. Yield não é primitive de espera e não garante progresso de
uma classe inferior sob prioridade estrita. Isso era defeito do harness, não uma
justificativa para alterar a política do scheduler.

## Fixture corrigido

O worker continua NORMAL e bloqueia em `semaphore_t release_gate`. O controller
INTERACTIVE usa deadline monotônico e `timer_sleep(1)`, observa
BLOCKED/WAIT_SEMAPHORE/WAIT queue, instala hold pelo ID copiado e libera o semáforo.
Só então observa ZOMBIE. Failure paths liberam gate/hold, aguardam reap e mantêm
quarantine quando lifetime não pode ser concluído.

O comando `reaptest fixture` passou em SMP 1/2/4/8. `reaptest priority` confirmou
controller INTERACTIVE, worker NORMAL e espera bloqueante.

## Waits e timer CLAIMED

O polling crítico baseado apenas em yield foi removido do fixture. Timer-ref passou
a segurar apenas a identidade alvo; o hook global anterior também retinha o timer
usado pela shell para dormir. O hold por identidade permite que a shell bloqueie
sem liberar a referência CLAIMED testada.

## Reaper FIX2 preservado

- `claimed == reaped + free_inflight`;
- CPU_SLOT isolado é estrutural; com ON_CPU/CURRENT é transitório;
- structural fault registra diagnóstico e causa panic controlado;
- grace continua policy-only;
- free e stats completion permanecem fora/depois do scheduler lock conforme contrato.

## Resultados positivos

- stack-check: PASS, frame máximo 1920/2048;
- kernel-check j2 e j12: PASS e binários idênticos;
- kernel SHA-256: `7121f4c91e127fecc711000d161b185ffba3a0cbd2c659d97368d7a1f9d51ebf`;
- image SHA-256: `1cd08fec8f43c9e5886ce8ccb6fc03ce52f25a784fee6e5ad86d6e46f170e7a0`;
- deps-check, image e `nm -u`: PASS;
- SMP 1/2/4/8: fixture, priority, all e checks sem fault;
- Normal 1000 e Killed 1000: PASS;
- grace 0/1/3000, batch 65, cleanup, notification/unregister, timer-ref,
  on-CPU, stale, heap drift zero e snapshot 1000: PASS;
- ZOMBIE MEM~: 16800 bytes idênticos em snapshot, ps e TASKMAN; removido após reap;
- negatives AGE_ONLY, TIMER_REF, DUPLICATE_NOTIFICATION e ON_CPU: PASS;
- soak CL-08 SMP=8: PASS, 180000 ms, quatro ciclos, structural zero.

Logs principais: `artifacts/build/cl08fix3-smp{1,2,4,8}.log`,
`cl08fix3-zombie-mem.log`, `cl08fix2-negative-*.log` e
`cl08fix3-soak-smp8.log`.

## Bloqueadores

1. `churn 1000 1000` agora contabiliza corretamente 2000 notifications/reaps e
   executa snapshots com workers vivos, mas seus creators NORMAL e KILLED ainda são
   fases sequenciais. A exigência de creators simultâneos não foi demonstrada.
2. `make test-cl07-final` não produziu certificação final. O harness CL-07 digitou
   comandos sobrepostos depois de iniciar o soak; o serial mostrou caracteres
   intercalados e nenhuma sentinela final. Não houve panic, mas o gate é inválido.

Por esses dois gates, nenhum commit foi criado. O worktree foi preservado, não
houve push e TMV1-CL-09 não foi iniciada.

## Fechamento posterior

A entrega FIX3 permaneceu rejeitada porque o churn era sequencial e a regressão
CL-07 não possuía barreira de conclusão da shell. A certificação final está em
`TMV1-CL-08-FIX4-final-certification.md`.
