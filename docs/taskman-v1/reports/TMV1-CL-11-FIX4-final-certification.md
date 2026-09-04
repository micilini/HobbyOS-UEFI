# TMV1-CL-11-FIX4 — certificação final

## Correção posterior do canary de generation

A execução FIX4 corrigiu a quiescência do clock-smp, mas o canary CL-08 usava
`reaper_stats.reaped` como proxy de remoção do registry. Como `reaped` é
incrementado depois do free fora do scheduler lock, ele podia avançar sem nova
mudança de registry entre as páginas. A correção e certificação final estão em
`TMV1-CL-11-FIX5-final-certification.md`.

## Status

**REJECTED.** A correção de quiescência passou o reprodutor exato 3/3, mas a
certificação integral parou depois em uma regressão CL-08 aninhada. Nenhum
commit foi criado e a CL-12 não foi iniciada.

## Base e worktree

- Branch: `feat/taskman`.
- Base: `88645901d9393f4796af84cecd9e13d3d6671c70`.
- O worktree CL-11/FIX1–FIX3 foi preservado e recebeu FIX4 sem reset ou stash.
- `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` permanece não rastreado e fora de
  qualquer stage.

## Diagnóstico FIX3

A shell roda como `TASK_CLASS_INTERACTIVE`, o reaper como
`TASK_CLASS_NORMAL`, e a runqueue escolhe a fila interactive antes da normal.
O helper antigo acordava a shell com `timer_sleep(1)` enquanto esperava
passivamente o reaper periódico. A ordem observada foi `CLOCK_SMP FAIL`, depois
32 linhas `REAPER FREE`, e finalmente o marker de transporte. O guest continuou
vivo e não houve regressão do clock: a espera interferia no progresso que
pretendia observar.

## Implementação de quiescência

`accounttest_wait_workers_reaped` valida os `task_handle_t` exatos, dirige
`scheduler_reap_zombies(0)`, consulta cada handle por snapshot copy-only e exige
três observações estáveis de todos ausentes com `free_inflight=0`. A espera
entre scans é de 10 ms. Timeout registra contagens por estado e, para cada
handle ainda presente, estado, CPU, queue, wait, exit, cleanup, notification e
reap claim.

`accounttest_wait_reaper_idle` é separado: dirige o scan e confirma
`current_zombies=0` e `free_inflight=0` pelo número pedido de observações.
O delta global de `reaped` é apenas telemetria.

A conclusão dos workers usa um semáforo compartilhado: cada worker sinaliza uma
completion e a shell espera 32 sinais. Isso eliminou inclusive a interferência
residual observada quando se tentou polling de 10 ms.

## Reprodutor pesado 3×

No mesmo QEMU SMP=8 TCG, cada rodada executou concurrent churn 1000/1000/1000,
create-handle-race 100000 e clock-smp 32/1000000. Resultado: 3/3 PASS.

- `worker_handles_gone=32` em todas as rodadas.
- `free_inflight=0` em todas as rodadas.
- `active_reap_calls=4` e `active_reaped=32` na execução final.
- API regressions, local regressions e cross lag: zero.
- Migrações finais: 19052, 18905 e 19241.

Artefatos: `artifacts/build/cl11fix4-clock-smp-run1.log`, `run2.log` e
`run3.log`. O harness emitiu `[CL08][CLOCK_SMP_QUIESCENCE] PASS runs=3`.

## Transporte, TASKMAN e build parcial

- HMP timing/keymap selftests: PASS; hold 30 ms; profiles 100/200/250 ms.
- Transporte SMP=8: PASS, 25 markers durante e 25 depois do stress, lost=0,
  duplicates=0.
- Fixture 3×1000 em SMP=1 e SMP=8: PASS nesta rodada.
- Quatro negatives TASKMAN: executados antes do positivo normal.
- Positivos TASKMAN SMP=1/2/4/8: PASS; o SMP=8 foi repetido depois da última
  alteração e passou.
- Formatação, clipping, builder truncation, stale cells e region clear: zero
  no positivo SMP=8.
- Heap 129-task: baseline/final 995280 bytes, drift=0.
- Stack-check: PASS, frame máximo 1984/2048 bytes.
- Kernel-check JOBS=2: PASS.

A telemetria global `over100` foi renomeada para `over100_marked` e permanece
visível, mas não é fault global: essas amostras já são explicitamente marcadas
com `!` pela UI. Regressões de runtime, drops, saturations e regressões de clock
continuam bloqueantes.

## Regressões e bloqueio final

- CL-07 integral: PASS, inclusive matriz SMP=1,2,4,8.
- CL-08 dedicado clock-smp: PASS 3/3.
- CL-08 integral externa: avançou pelos positivos, negatives e soak.
- CL-09 integral: avançou pelos gates próprios e pela CL-07 aninhada.
- CL-10 integral: não executada porque a cadeia fail-fast parou antes.
- Soak CL-11 final: não executado depois da última alteração.

O bloqueio ocorreu na CL-08 aninhada da CL-09:

```text
[REAPTEST][CONCURRENT_CHURN] FAIL
normal=1000 killed=1000
normal_notified=1000 killed_notified=1000 reaped=2000 snapshots=1000
overlap=1 reaps_during_snapshots=2002 generation_changes=125
forced_generation_changes=0 restarts=125 duplicates=0 partial=0
contexts=0 holds=0
```

O guest continuou vivo, mas essa é uma falha causal do canário CL-08, não uma
perda HMP: o comando iniciou e concluiu explicitamente com FAIL. Alterar o
reaper/scheduler ou enfraquecer o canário está fora do escopo FIX4. Por isso a
certificação foi encerrada, sem executar build/hash finais e sem commit.

## Imagem, warnings e conclusão

Os hashes finais não foram emitidos, pois dependem de toda a homologação passar.
Os warnings observados pertencem ao bootloader preexistente; warnings CL-owned:
zero. Nenhum QEMU permaneceu ativo depois da falha.

Status final: **REJECTED**. CL-12+ não implementadas.
