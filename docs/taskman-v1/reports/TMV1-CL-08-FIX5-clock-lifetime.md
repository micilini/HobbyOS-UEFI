# TMV1-CL-08-FIX5 — clock SMP e lifetime de criação

Data: 2026-08-01
Branch: `feat/taskman`
Base: `e9834561d66b1bf55a8aed39d7bcde75c16baf7a`

## Diagnóstico de entrada

O FIX4 observou 830 `clock_regressions` em 1.252.915 leituras após concurrent
churn SMP=8 MTTCG, com zero regressão runtime/métrica, zero amostras acima de 100%
e LAPIC rate PASS. O modelo antigo serializava todas as chamadas, comparava uma
leitura raw apenas com `g_last_ns`, fazia até 4096 releituras MMIO e retornava
`max(raw,g_last_ns)`. Portanto o contador não provava regressão da API.

## Nova classificação

O clock agora mantém histórico raw/returned por CPU mais bucket bootstrap, sob o
clock lock. A leitura HPET estável usa duas leituras e no máximo quatro tentativas.
Os contadores separam API regression, source-local regression, cross-CPU lag,
retry exhaustion, saturation, unknown CPU e state corruption, com máximos de
backward/lag/retry. Canários protegem o estado e um ring bounded de 32 eventos não
imprime no hot path. A API continua linearizável por `max(sample,g_last_ns)`.

Medição após concurrent churn 1000/1000/1000:

- TCG multi SMP=8: API=0, local=0, cross=0, retry exhaustion=0, max lag=0;
- TCG single SMP=8: API=0, local=0, cross=0, retry exhaustion=0, max lag=0;
- KVM SMP=8: API=0, local=0, cross=0, retry exhaustion=0, max lag=0.

`clock-smp 32 1000000` passou nos três ambientes. MTTCG observou 22.838
migrações; single 4.774; KVM 4.316. O ring ficou vazio em MTTCG. Assim os 830
eventos antigos eram artefatos do sample isolado contra o máximo global; a leitura
bounded seguinte alcançou o valor estável sem retry storm e sem clamp.

## Lifetime de criação

Antes, `thread_create_ex` publicava/enfileirava, soltava o lock e dereferenciava
`task_t` para log/retorno. Uma child imediata podia executar, sair e ser reaped
antes dessas leituras. A API canônica agora retorna `task_handle_t {id,
lifecycle_generation}` copiado sob o scheduler lock. Nome, flags e classe usados
no log também são cópias locais capturadas antes da publicação ficar acessível
fora do lock. Callers que precisam observar estado usam snapshots por ID/generation.

Foram migrados churn, reaptest, killtest, accounttest, schedtest e os caminhos
cancel/race/stale de synctest. O static gate não encontra assignment de retorno de
criação para `task_t *`. `create-handle-race 100000` passou em MTTCG SMP=8:
100000 handles únicos e válidos, 100000 reaps, zero duplicates/faults e heap sem
drift. Concurrent churn permaneceu PASS em TCG multi/single e KVM.

## Classificação sintética e negatives

O selftest puro emite `[CLOCK][SELFTEST] SMP_CLASSIFICATION_OK` e cobre raw
crescente local, lag entre CPUs, backward local, max global, canário, slot
desconhecido, saturação e retry exhaustion. Os builds negativos emitiram:

- `[CLOCK][NEGATIVE] GLOBAL_ONLY_FALSE_REGRESSION_DETECTED`;
- `[CLOCK][NEGATIVE] LOCAL_SOURCE_REGRESSION_DETECTED`.

Os quatro negatives do reaper emitiram apenas suas sentinelas esperadas (age,
timer ref, duplicate notification e on-CPU), sem `REAPTEST FAIL` posterior. Os
dois negatives CL-07 (non-killable e exiting-normal) também foram reexecutados e
detectados. Todos os builds negativos foram seguidos por rebuild normal.

## Matriz e soak

A matriz positiva CL-08 passou em SMP 1/2/4/8. Incluiu fixture, priority, all,
normal/killed, concurrent churn, checks, ZOMBIE MEM~, create-handle-race e
clock-smp. O soak MTTCG SMP=8 durou 180000 ms, completou oito ciclos de churn,
oito timer-ref, oito snapshots, dez ciclos TASKMAN, create-handle-race 100000 e
clock-smp 1000000. Os checks finais de reaper, kill, scheduler, sync e accounting
passaram sem fault estrutural, UAF, drift de heap ou anomalia de clock/métrica.

A regressão CL-07 final passou com `[CL07][REGRESSION] PASS` após soak contínuo
de 120000 ms, timeout races acumuladas, smpstress, CLI/TASKMAN e checks, todos
separados por `shell_sync`. O lock exclusivo impediu ownership simultâneo de
`.qemu`.

## Build e imagem

O stack-check passou com frame máximo de 1920 bytes. Os builds `JOBS=2` e
paralelo passaram, foram byte a byte idênticos e produziram
`d941dd94793b63326a21e0d757a9d8b8d89372eeced3204d339ce05f3f2abfb2`.
`deps-check`, image e `nm -u` também passaram; o hash da imagem final é
`9753630985b5949358c3a91c505561efd167b65092965902fdfff5a3efc7d36c`.
Os warnings observados na criação da imagem são os dois preexistentes do
bootloader (`serial_write_u32_all` e `EFI_STATUS status`); os blocos tocados de
clock, accounttest e reaptest não introduziram `-Wmisleading-indentation`.

## Status

**APROVADO.** Todos os gates obrigatórios passaram; resta apenas o commit único
prescrito. CL-09+ não foi iniciada.

## Auditoria posterior

A primeira certificação FIX5 possuía negatives do clock incondicionais,
canários adjacentes e regressão CL-07 somente em SMP=8. A certificação
definitiva está em `TMV1-CL-08-FIX6-final-certification.md`.
