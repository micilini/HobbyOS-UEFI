# TMV1-CL-11-FIX17 — fechamento retrospectivo

Data: 2026-08-16
Branch: `feat/taskman`
Base: `88645901d9393f4796af84cecd9e13d3d6671c70`

## Correção de compilação

O smoke FIX16 falhou por declaração implícita de `clock_monotonic_ns()`. A FIX17
adicionou somente `#include "../../core/clock.h"` a `cmd_taskman.c`. O smoke
seguinte passou com frame máximo 1984/2048 e `nm -u` vazio.

## Gates executados

O probe passou com 5 sessões, 50 full frames, fallback e scroll zero, duas
páginas, 142 tasks capturadas, gap máximo de 1094 ms e heap drift zero. A matriz
TASKMAN SMP=1/2/4/8 e os quatro negatives causais passaram.

O soak guest-side emitiu PASS após 3.367.550 ms, 100 sessões e 1.500 full
frames. Fallbacks, shortfalls e scroll modal ficaram em zero; o gap máximo foi
1748 ms e todos os workloads reduzidos foram concluídos.

## Rejeição host-side

O runner FIX17 rejeitou o soak porque seu regex exigia `refs=0`, embora o guest
tivesse emitido `[REAPTEST][CHECK] PASS ... refs=1 ...`. Esse valor é a timer
reference legítima do reaper periódico, não uma referência vazada.

## Fechamento posterior

A correção causal do verificador, a revalidação offline e o build final estão
documentados em `TMV1-CL-11-FIX18-final-closure.md`.
