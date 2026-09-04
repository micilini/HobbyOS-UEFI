# TMV1-CL-08-FIX6 — certificação final

Data: 2026-08-01
Branch: `feat/taskman`
Base completa: `8b10f2c1b5b23dfbbee6ffe63338512300b415ec`

## Defeitos auditados

O FIX5 imprimia as duas sentinelas negativas apenas pela presença da macro,
mantinha os canários adjacentes depois do array per-CPU e havia executado a
regressão CL-07 final essencialmente em SMP=8. O soak CL-08 também dependia do
valor residual de `n`, e os artefatos ainda usavam nomes de FIX anteriores.

## Classificador compartilhado

`clock_classify_sample()` é puro: não usa globais, lock, MMIO ou serial. O caminho
real de `clock_monotonic_ns()` chama esse helper e consome integralmente sua
decisão para atualizar histórico, global max, telemetria e ring de eventos. O
selftest chama o mesmo helper em dez casos: first, increasing, cross-lag, dois
backwards locais, retry exhausted, invalid, igualdade local/global, igualdade
local abaixo do global e fronteira `UINT64_MAX`.

O build global-only muda o helper para classificar `sample < global` como local
regression. O teste `80/100/90` observou essa mutação e somente então emitiu
`GLOBAL_ONLY_FALSE_REGRESSION_DETECTED`. O build local-backward muda o helper
para tratar `sample < local` como cross-lag não fatal; o teste `100/150/90`
detectou a mutação e emitiu `LOCAL_SOURCE_REGRESSION_DETECTED`. O build normal
emitiu `[ACCOUNT][CLOCK_CLASSIFIER] PASS cases=10`.

## Estado e canários

Todo o estado mutável reside em uma `clock_state_t`: canary begin, ready/global,
stats, estados per-CPU, sequência/ring de eventos e canary end. `_Static_assert`
fixa o canary inicial no offset 0 e exige o final depois do ring. O layout medido
foi begin=0, end=4016 e 4008 bytes protegidos. A validação ocorre sob o clock
lock. Reset de stats limpa apenas stats/eventos/sequência, preservando canários,
global last e histórico per-CPU.

## Ambientes do clock

Os três `launch.env` foram validados antes dos logs:

- TCG multi: `ACCEL=tcg`, `TCG_THREAD=multi`, `SMP=8`;
- TCG single: `ACCEL=tcg`, `TCG_THREAD=single`, `SMP=8`;
- KVM: `ACCEL=kvm`, `SMP=8`.

Todos registraram QEMU 8.2.2 (Debian/Ubuntu package). Cada ambiente executou
classifier, clock-smp 32/1000000, concurrent churn 1000/1000/1000, stats,
events e account check. Multi observou 20460 migrações, single 4428 e KVM
3364. Em todos: API=0, local=0, cross-lag=0, retry exhaustion=0, saturation=0 e
state corruption=0; o ring terminou vazio.

## Regressão CL-07

`scripts/test-cl07-matrix.sh` executou a matriz completa depois de
`task_handle_t`: SMP=1 com timeout-race 500, SMP=2 com 2000, SMP=4 com 5000,
smpstress 8, kill sweep e UI/TASKMAN, e SMP=8 com timeout races acumuladas,
smpstress 16 e soak contínuo de pelo menos 120000 ms. Checks kill/scheduler/sync/
account passaram em todos os SMPs. Os negatives non-killable e exiting-normal
foram detectados em QEMUs isolados. Sentinela final:
`[CL07][MATRIX] PASS smp=1,2,4,8`.

## Certificação CL-08

`scripts/test-cl08.sh all` passou a matriz SMP=1/2/4/8, todos os positivos do
reaper, normal/killed 1000, concurrent churn, create-handle-race 100000, ZOMBIE
MEM~, quatro negatives e soak SMP=8. O modo `scripts/test-cl08.sh soak` também
passou isoladamente: 9 ciclos em 180000 ms, dez ciclos TASKMAN, handle-race e
clock-smp finais. Zero panic, exception, structural/finish fault, invalid handle,
duplicate ID, heap drift ou anomalia scheduler/sync/accounting.

## Warnings

Os blocos CL-owned indicados em clock, lifecycle, metrics e scheduler foram
reformatados. O build final possui zero `-Wmisleading-indentation` nesses
arquivos. Permanecem apenas warnings legados/fora do escopo, principalmente XHCI,
unused, duas linhas preexistentes de `hpet_usleep` e linker executable-stack/RWX.

## Build e hashes

`make stack-check` passou com frame máximo de 1920 bytes. Os builds `JOBS=2` e
paralelo passaram, foram byte a byte idênticos e produziram:

```text
kernel.elf 657e5526a68482c8eced76a9b193d05bc26cbf02b4894ef62b8b94b4e1411084
hobbyos.img 619539e3de568f5c038a24eaedfe8f9711d3318b542a555cea8d1d22f1cec514
```

`deps-check`, `make image` e `nm -u kernel.elf` passaram; a lista de símbolos
indefinidos ficou vazia.

## Status

**APROVADO.** Todos os gates FIX6 passaram. CL-09+ não foi iniciada.
