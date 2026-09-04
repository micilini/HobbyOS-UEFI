# TMV1-CL-09 — input router e filas sem vazamento

Data: 2026-08-01
Branch: `feat/taskman`
Base: `378a1ac77d7424b00d7b95235576dfa28375302f`

## Diagnóstico

As três filas de input mantinham ring e semáforo independentes. `try_pop` e
`drain` removiam eventos sem reconciliar permits; o input thread consumia um
permit e drenava um lote inteiro. Além disso, o router escolhia o destino sob
seu lock, liberava-o e somente depois enfileirava. Isso admitia wakes fantasmas
e enqueue numa geração de foco obsoleta. Keyboard, router e TASKMAN também
imprimiam trace por tecla por padrão.

## Arquitetura

`input_queue` usa storage externo e capacidade integral. `head`, `tail` e
`count` mudam sob o mesmo spinlock; `count` é a única fonte de verdade. Pushes
aceitos recebem sequence monotônica e metadata de geração/destino. Não existe
semaphore paralelo. `wait_pop` usa prepare/commit/abort com
`TASK_WAIT_INPUT_QUEUE`; o lock order é `queue -> scheduler`. Drain move o tail
para o head, zera count e avança a geração sem acordar waiters.

O router mantém estados DEFAULT/OPENING/MODAL/CLOSING e geração não nula. O
dispatch mantém `router -> destination queue -> scheduler` até que o evento
pertença à fila. Begin drena typed-ahead default e residual modal antes de
publicar MODAL; end drena o tail modal antes de publicar DEFAULT. Eventos após
DEFAULT ficam preservados até o resume da shell.

`modal_session` agora possui init explícito e estados
INACTIVE/OPENING/ACTIVE/CLOSING, sem token, task ID ou ownership da CL-10. O
teclado USB/PS2 publica `input_event_t` na mesma `input_queue` de 512 entradas;
o input thread faz um wait seguido por pops contabilizados individualmente.

## Tracing e diagnóstico

O boot usa `INPUT_TRACE_NONE`. Traces keyboard/router/queue/consumer dependem de
flags runtime; o caminho normal e queue-full não imprimem por evento. `inputtest
stats` e `inputtest check` expõem count, drops, wake stats, gerações e invariantes.

## Testes executados

- queue capacity 8, full, FIFO, wrap e sequence: PASS;
- try-pop 100 seguido de waiter real: PASS, phantom=0;
- drain 256/73/183 e full 10000: PASS;
- CHAR/SPECIAL intercalados: 1024/1024, ordem preservada;
- dois produtores/um consumidor: 100000 aceitos/consumidos, gaps=0, duplicates=0;
- boundary determinístico SMP=4: producer permaneceu bloqueado enquanto o hook
  segurava o router lock em begin/end; destino posterior correto;
- 10000 transições: PASS, filas vazias e shell retomada;
- keyboard FIFO real: 10000 injetados/roteados/consumidos, phantom=0;
- ESC + texto rápido HMP: marker anterior ao session_end ausente; marker posterior entregue;
- negatives isolados: PHANTOM_PERMIT_DETECTED e NONATOMIC_ROUTE_DETECTED;
- matriz TCG SMP=1/2/4/8: PASS, com checks scheduler/sync/account/kill/reap;
- regressão CL-07: `[CL07][MATRIX] PASS smp=1,2,4,8`;
- regressão CL-08: matriz, negatives e soak concluídos sem falha;
- soak CL-09 inicial: 180000 ms, 12 ciclos, 1.2 milhão de eventos sintéticos,
  12000 transições e 120000 eventos keyboard; checks finais sem falha.

## Negatives

`HOBBYOS_INPUT_NEGATIVE_PHANTOM_PERMIT` reproduz permits legados restantes após
try-pop. `HOBBYOS_INPUT_NEGATIVE_ROUTE_AFTER_UNLOCK` seleciona modal, muda a
rota e enfileira no destino antigo. As sentinelas só aparecem após a condição
incorreta ser observada. Cada QEMU é isolado e o build normal é restaurado.

## Arquivos e política typed-ahead

Foram adicionados `input_queue.[ch]`, `cmd_inputtest.[ch]` e
`scripts/test-cl09.sh`; router, modal session, keyboard, boot, registry, TASKMAN,
Makefile e documentação receberam integração estreita. Typed-ahead default no
begin e tail modal no end são descartados e contabilizados; eventos posteriores
ao end permanecem na default.

## Build, hashes e warnings

O build normal final `make kernel-check JOBS=2` passou e produziu
`e78f19eae9d5d91350eb6d5d6eb61212ef59fb1fe41c52ca5aa50dda3ae4f152` para
`kernel.elf`. Os gates jN/cmp, stack-check e hash final da imagem não foram
executados depois do blocker CL-08, pois a política proíbe certificar/commitir
após qualquer gate final falhar. Os blocos CL-owned tocados compilam sem
`-Wmisleading-indentation`; warnings legados XHCI/unused permanecem fora do
escopo.

## Blockers da recertificação final

A matriz CL-09 final SMP=1/2/4/8, leakage, negatives e a regressão CL-07 final
passaram. Um soak CL-09 de 180000 ms anterior ao último ajuste test-only passou
12 ciclos, mas não é usado como certificação do artefato final.

A recertificação CL-08 do artefato final falhou duas vezes, com QEMUs isolados:

```text
SMP=8: [REAPTEST][GRACE] FAIL
g0=1 g1_boundary=1 g1_after=1 g3000_before=1 g3000_after=0

SMP=1: [REAPTEST][CONCURRENT_CHURN] FAIL
normal=100 killed=100 normal_notified=100 killed_notified=100 reaped=200
snapshots=100 overlap=1 reaps_during_snapshots=202
generation_changes=0 restarts=0 duplicates=0 partial=0 contexts=0 holds=0
```

Uma execução positiva isolada entre as duas falhas passou, o que demonstra
intermitência, mas não satisfaz o gate obrigatório. Nenhuma alteração no reaper
ou scheduler foi feita para mascarar o resultado. O QEMU foi encerrado, o build
normal foi restaurado e nenhum commit foi criado.

## Reservado à CL-10

Não foram implementados owner token, owner task/generation, listener de morte,
auto-release, `modal_ui_run_sync`, task `taskman-ui`, ownership genérico ou UI
worker independente.

## Status

**REJEITADO.** A implementação CL-09 e seus testes positivos estão preservados
no worktree, mas a recertificação CL-08 obrigatória não é estável. Sem commit.
CL-10+ não foi iniciada.

## Correção e certificação posterior

A primeira tentativa foi rejeitada por dois gates flakey da regressão CL-08 e
por lacunas causais nos testes de input. A certificação final está em:
`TMV1-CL-09-FIX-final-certification.md`.
