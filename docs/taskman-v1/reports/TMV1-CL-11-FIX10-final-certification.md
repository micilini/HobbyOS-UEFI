# TMV1-CL-11-FIX10 — canário exato e boundaries modais

## Diagnóstico SMP=1

O failure FIX9 ocorreu em sem=46/timer=45, com 92 requests/accepted e 92 exits
KILLED. O período de 2000 ms do reaper atravessou o teste e alterou `armed` e
`task_refs_acquired` globais. Esses contadores não provavam ownership do target.

## Correção exata

- `timers_test_task_wake_snapshot` percorre pending/claimed sob `g_timer_lock`, por
  ID+lifecycle, copiando nodes, refs, handles e wrong-lifecycle.
- O worker preblock nasce com reap hold exato. Snapshots inicial e ZOMBIE KILLED
  validam wait generation e contadores de ref da própria task.
- O PASS exige zero nodes/refs do target; stats globais não participam mais.
- O hold só é removido depois da validação; o workspace só é reutilizado depois de
  handle ausente e `free_inflight=0`.
- `task_handle_t` foi movido para `task.h`, justificadamente, para compartilhar a
  identidade exata com timers sem dependência circular de scheduler.

## Resultados preblock

- SMP=1: 3 boots, sem=1000, timer=1000 e input=1000 PASS.
- SMP=8: 3 boots, mesmos volumes PASS.
- Zero NORMAL, nodes/refs/residual e violations.

## Runtime modal

- Snapshot persistente da última run e reap hold one-shot do worker.
- Workspace estático para os novos modos.
- BLOCKED focado: caller KILLED, worker KILLED, cleanup=1.
- PREWAIT focado: caller KILLED, worker KILLED, cleanup=1.
- COMPLETION_READY focado: wait permit-first/latch, caller KILLED, worker NORMAL,
  cleanup=1.
- Loop SMP=8 reduzido: 100/100 por modo PASS, zero NORMAL inesperado.

## Status

REJECTED. Permanecem loops modais 3×1000 SMP1/8, timer-noise dedicado, heavy runs,
regressões integrais, matriz/negatives/soak CL-11 e builds/hashes finais. Nenhum
commit foi criado.

## Continuação e certificação posterior

A FIX10 corrigiu o canário preblock por target exato e implementou os três modos
modais. Os testes focados e os loops reduzidos passaram, mas os harnesses não
foram integrados, o canário timer-noise não foi implementado, os loops modais
3×1000 e as regressões/soak não foram executados.

A auditoria também endureceu cleanup, residual, test controls e o snapshot
mode-specific.

A certificação final está em:
TMV1-CL-11-FIX11-final-certification.md
