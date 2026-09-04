# TMV1-CL-11-FIX8 — preblock e cancelamento modal latched

## Continuação e certificação posterior

A FIX8 implementou o core do cancelamento preblock, migrou semáforo, timer e input
queue, adicionou o invariant runtime e corrigiu o latch do runtime modal. A execução
foi interrompida antes dos loops, do negative direcionado, dos hooks modais e da
certificação integral. A continuação está em
`TMV1-CL-11-FIX9-final-certification.md`.

## Base

- Branch `feat/taskman`.
- Base `88645901d9393f4796af84cecd9e13d3d6671c70`.
- Worktree FIX1–FIX7 preservado.

## Diagnóstico

O log FIX7 possuía workers 53, 59 e 61 com exit NORMAL e kill tardio
`ALREADY_ZOMBIE`, embora o teste imprimisse PASS. O teste validava o caller e o
cleanup, mas não o exit reason exato do worker. Também existia a janela permit-first:
completion pronta fazia `sem_wait_interruptible` retornar OK sem consumir o
`kill_pending`.

## Implementação realizada

- O prepare interruptível e suas migrações foram preservados.
- O validator do scheduler agora rejeita task cancelável com kill pendente, wait
  ativo e wake NONE, expondo `pending_cancel_wait_violations`.
- `inputtest preblock-cancel` usa worker killable e queue vazia; exige CANCELLED,
  incremento do contador, zero evento/phantom e exit KILLED.
- O runtime modal latcheia cancelamento por `wait == CANCELLED ||
  task_cancel_requested()`, conclui teardown/context lifetime e somente então chama
  `task_cancel_point`.
- O worker modal classifica caller bloqueado, cancelado, exiting ou ausente. Caller
  cancelado antes da entry causa recovery e self-kill do worker, sem aguardar timeout.

## Evidência

- `make kernel-check JOBS=2`: PASS.
- `inputtest preblock-cancel`: PASS.
- `synctest preblock-sem`: PASS.
- `synctest preblock-timer`: PASS, zero arm/ref.
- `modaltest kill-caller`: 25/25 PASS em SMP=8.
- `schedtest check`: PASS, violations=0.
- No lote focado: zero `reason=NORMAL` e zero `ALREADY_ZOMBIE`.
- Artefato: `artifacts/build/cl11fix8-focused-smp8.log`.

## Status

REJECTED. Faltam hooks e comandos separados BLOCKED/PREWAIT/COMPLETION_READY, negative
one-shot direcionado, loops 3×1000, cinco heavy runs, regressões integrais, matriz
CL-11, soak e build/hash finais. Nenhum commit foi criado.
