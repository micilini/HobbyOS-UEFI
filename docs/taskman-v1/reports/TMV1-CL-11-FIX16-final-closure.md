# TMV1-CL-11-FIX16 — fechamento retrospectivo

Data: 2026-08-16
Branch: `feat/taskman`
Base: `88645901d9393f4796af84cecd9e13d3d6671c70`

## Modelo de soak

A FIX16 substituiu a razão inválida entre wall-clock do host e refresh do guest
por sessões determinísticas: cada sessão reseta a âncora do console, solicita
full frames ao guest e encerra somente quando o alvo é atingido. Full frames e
fallbacks são contabilizados separadamente, o gap é monotônico guest-side e o
scroll modal termina antes da mensagem normal da shell.

## Diagnóstico do soak antigo

O soak anterior terminou com 100 sessões, 190 full frames, `pages=0`,
`captured=0` e 63 scrolls. A âncora degradava a cada `TASKMAN exited.`, levando
sessões posteriores a `TOO_SHORT`; o scroll medido incluía a saída normal da
shell; e o frame ratio misturava wall-clock do host com sleep mínimo do guest.

## Correção posterior de compilação

A implementação FIX16 utilizava `clock_monotonic_ns()` em `cmd_taskman.c`, mas o
translation unit não incluía `core/clock.h`. A correção de uma linha e o fechamento
final estão documentados em:
TMV1-CL-11-FIX17-final-closure.md

## Fechamento posterior

A reclassificação correta de `refs_current`, a revalidação offline do soak e o
build final estão documentados em `TMV1-CL-11-FIX18-final-closure.md`.
