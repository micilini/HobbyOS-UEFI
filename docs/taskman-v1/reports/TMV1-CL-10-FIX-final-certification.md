# TMV1-CL-10-FIX — certificação final

Data: 2026-08-01
Branch: `feat/taskman`
Base: `76d9a35f045322246fd6997226fcf008d79ed0ea`

## Defeitos e correções

O runtime anterior publicava `active_context` tarde, iniciava a UI após um
sleep heurístico, permitia abandono por cancelamento do caller e misturava claim
e publicação da completion. O contexto agora é reservado antes de criar o
worker e percorre estados NEW/RESERVED/WORKER_CREATED/WAITING_CALLER/RUNNING/
COMPLETING/DONE. Segundo caller recebe BUSY no ponto da reserva.

O worker somente entra na UI depois de observar por snapshot copy-only o caller
BLOCKED em `TASK_WAIT_SEMAPHORE`. `sem_wait_interruptible` trata cancelamento do
caller: solicita kill do worker, restaura a sessão, aguarda ZOMBIE/ausência e só
então executa o cancellation point. Timeout nunca retorna com worker vivo; uma
falha de lifetime irresolvível mantém o contexto alcançável e causa panic
controlado.

Completion usa CAS separado em `completion_claimed`, publica status/result com
release e somente depois marca `completion_signaled` e sinaliza o semáforo.
Cleanup e listener usam acessos atômicos. O exit reason vem de snapshot por
handle, inclusive kill antes do begin. Canários são validados em worker,
cleanup, listener e caller.

`modal_session_snapshot` mantém a lock modal ao observar router e pause atômico
da shell. Close idempotente exige também o owner correto. Recovery não completa
enquanto a sessão ainda pertence ao worker.

## Testes de runtime e sessão

- Open/close: SMP 1/2/4/8 com 250/500/1000/1000 ciclos; cleanup exatamente uma
  vez por ciclo e `heap_drift=0`.
- Reservation gap: BUSY antes do detach, zero worker extra, geração preservada e
  chamada posterior bem-sucedida.
- Concurrent callers: 10000 rodadas em SMP 4/8, exatamente um accepted e um BUSY
  por rodada, `extra_workers=0`.
- Kill caller e kill-before-begin: teardown completo, completion única, sessão
  restaurada e contexto zero.
- Snapshot race: 100000 transições e 100000 snapshots em SMP 4/8, zero combinação
  impossível.
- Tokens: false/stale/wrong owner rejeitados; close encerrado é idempotente
  apenas para o owner original.
- Falhas: allocation, worker create, router begin/end, owner kill e lifecycle
  fallback terminaram em INACTIVE/DEFAULT/unpaused.
- TASKMAN: `taskman-ui` distinto da shell, caller realmente BLOCKED; saída normal
  e killed preservam o comportamento visual existente.

## Stress e soak

O stress interno atingiu `normal=5000`, `killed=1000`, `recovered=100`,
`busy=1000`, `token=1000` e `failures=100`. O soak SMP=8 durou 180000 ms,
executou 100 ciclos TASKMAN, 100 cancelamentos de caller, snapshot race de
100000 e terminou com `contexts=0`, `quarantine=0`, duplicates zero e todos os
checks de modal/input/scheduler/sync/account/kill/reaper em PASS.

## Matriz e regressões

A matriz CL-10 passou em SMP 1/2/4/8. As regressões integrais produziram
`[CL07][MATRIX] PASS smp=1,2,4,8`, certificação CL-08 completa e
`[CL09][CERTIFICATION] PASS`. A estabilização CL-08 inclui hold atribuido à
task exata e fixture repetida sem race. A CL-09 inclui producers com sequence
real, drain waiter causal, boundary loop de 1000 ciclos e 100 ciclos TASKMAN.

Os negatives causais de stale token aceito e owner recovery ausente foram
executados em QEMUs isolados e seguidos de rebuild normal. Nenhuma macro negative
permanece no artefato final.

## Build, imagem e warnings

`make stack-check`, builds JOBS=2/JOBS=nproc reproduzíveis, `make deps-check`,
`make image` e `nm -u kernel.elf` compõem o gate final. Warnings
`-Wmisleading-indentation` em código CL-owned são zero; warnings legados de
XHCI/bootloader permanecem fora do escopo. Os hashes finais são registrados após
a última imagem, imediatamente antes do commit.

## Arquivos e artefatos

Implementação: `modal_ui.[ch]`, `modal_session.[ch]`, helpers copy-only do
scheduler, TASKMAN, modaltest, harnesses CL-07/08/09/10 e estabilizações estreitas
de input/reaptest. Artefatos principais: `cl10fix-smp{1,2,4,8}.log`,
`cl10fix-taskman-{normal,killed}.log`, `cl10-negative-*.log`,
`cl10fix-soak-smp8.log` e logs novos das regressões CL-07/08/09.

## Falha do gate agregado final

O `scripts/test-cl10.sh all` repetiu com sucesso a matriz CL-10, TASKMAN,
negatives, CL-07, CL-08 e CL-09 integrais. No soak final CL-10, depois de atingir
100 ciclos TASKMAN, os volumes de stress, 100000 snapshots e 100 cancelamentos
de caller, o check final de accounting falhou:

```text
[ACCOUNT][STATS]
clock_reads=43840939
api_regressions=0
local_regressions=83862
cross_lag=664493
retry_exhaustions=0
max_cross_lag_ns=42949635930
clock_saturations=0
metric_regressions=0
over100=0
[ACCOUNT][CHECK] FAIL
```

O artefato exato foi preservado em
`artifacts/build/cl10fix-soak-failure-clock.log`. `source_local_regressions > 0`
é hard failure do baseline CL-08 e não pode ser reclassificado como simples
cross-CPU lag. O runtime modal estava limpo imediatamente antes do check
(`state=INACTIVE`, `contexts=0`, `quarantine=0`, `duplicates=0`) e input,
reaper, scheduler e sync estavam em PASS.

## Status

REJECTED. Nenhum commit foi criado. O worktree CL-10/FIX foi preservado e o
QEMU encerrado. A investigação/correção da regressão local da fonte de clock é
necessária antes de repetir o gate agregado e autorizar o commit. Nenhum item
CL-11 foi implementado.

## Correção posterior do clock

O soak final encontrou regressões locais da fonte com delta próximo de 2^32
ticks. A auditoria identificou leitura rasgada do HPET no QEMU 8.2.2, que
anuncia contador de 64 bits, mas expõe acessos MMIO de 32 bits. A correção e
certificação final estão documentadas em
`TMV1-CL-10-FIX2-hpet-rollover.md`.
