# TMV1-CL-07-FIX3 — gates finais

Data: 2026-07-31. Branch `feat/taskman`. Base `1293cce3c92e5d988b13f9d1fadf85a4ace1df2d`.

## Escopo

Fechamento exclusivo dos gates de keymap/CLI, matriz UI simultânea e soak contínuo. O contrato cooperativo de kill, cancellation points, cleanup e reaper permaneceu inalterado. O único ajuste em scheduler amplia o hook test-only de reap hold para 16 IDs, indispensável para preservar os cinco workers da matriz até a validação de lifetime.

## Keymap e CLI

`python3 scripts/qemu_hmp.py --selftest-keymap` passou com `- -> minus` e `+ -> shift-equal`. No guest, `echo -1` e `echo +1` confirmaram os caracteres.

O sweep HMP produziu casos individuais:

- no-args e extra-args: `ARGS/INVALID`;
- alpha, minus, plus e overflow: `INVALID/INVALID`;
- zero: `OK/INVALID`;
- UINT64_MAX: `OK/NOT_FOUND`.

IDs reais cobriram `PROTECTED`, `NOT_KILLABLE`, `ACCEPTED`, `ALREADY_PENDING`, `ALREADY_EXITING`, `ALREADY_ZOMBIE` e `NOT_FOUND`.

## Matriz UI

Setup SMP=4 homologado: `prot=1 no=15 killable=16 pend=17 exit=18 zomb=19`. `ui-status` observou simultaneamente `PROT=1 NO=1 FREE=1 PEND=1 EXIT=1 ZOMB=1`.

`ps` registrou os mesmos IDs e estados. TASKMAN registrou os seis no mesmo frame, com `session_begin` e `session_end`; a shell respondeu a `help` após ESC.

O cleanup validado em execução limpa terminou com:

`[KILLTEST][UI_CLEANUP] PASS contexts=0 quarantine=0 holds=0`

## Soak contínuo

- START monotonic_ms=40765602, duration_ms=120000.
- END monotonic_ms=41078462, elapsed_ms=312860.
- mesmo QEMU SMP=8 durante toda a janela;
- `killtest all`: 10;
- timeout-race: 10000 rounds;
- smpstress sweep 16/16: 10;
- TASKMAN begin/end: 10;
- checks completos: 10.

Todos os checks finais passaram; não houve panic, #PF, #GP ou FATAL no serial do soak. Pending, cleanup missing/duplicate, accepted+normal, contexts, quarantine, holds, scheduler violations, lost wakes e metric anomalies permaneceram zero.

## Builds, matriz e negatives

Os builds canônicos JOBS=2 e JOBS=nproc foram comparados byte a byte: SHA-256 b949618fe89f499327fac70690064627dc995a78d4ad77afccb03706fd7fdb7c. `make deps-check`, `make image` e `nm -u kernel.elf` passaram. Imagem final SHA-256 cedcee2a3cde75576f14fc045735a5ba68308156e096c0b3c1589508cb7ae617. A matriz final cobriu SMP=1/2/4/8 com core, timeout-race, checks, UI/CLI em SMP=4 e soak/smpstress em SMP=8.

Os negativos reconstruídos detectaram `NONKILLABLE_ACCEPTANCE_DETECTED` e `EXITING_ACCEPTED_NORMAL_DETECTED`; o rebuild normal posterior passou.

## Artefatos

- `artifacts/build/cl07gate-cli-complete.log`
- `artifacts/build/cl07gate-ui-matrix.log`

## Correção posterior do fault de scheduler

A execução combinada CLI/UI revelou `SCHED: invalid switch finish state`.
A investigação e certificação estão documentadas em:
`TMV1-CL-07-FIX4-stack-safety.md`.
