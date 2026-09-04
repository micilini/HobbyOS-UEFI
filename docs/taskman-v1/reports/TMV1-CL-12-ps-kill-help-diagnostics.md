# TMV1-CL-12 — alinhamento de ps, kill, help e diagnóstico

Data: 2026-08-16
Branch: `feat/taskman`
Base aprovada: `ce321da092d109cf49243d34c4c8709c5cf2e3f1`
Status: `APPROVED WITH NON-BLOCKING NOTES`.

## 1. Auditoria inicial

O worktree começou na branch e base exigidas. O único item preexistente era
`ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md`, não rastreado e fora do stage.

`ps` possuía buffer total fixo de 128 entries, offset zero, ignorava argumentos,
usava título `Fase 5B`, formatters próprios de class/MEM/%CPU, padding manual,
colunas divergentes e truncamento sem navegação. `kill` já exigia `argc == 2`,
tinha parser uint64, aceitava `UINT64_MAX`, rejeitava sinal/overflow e separava
mensagens. Foram preservadas essas garantias; as lacunas eram mapping inline,
return codes colapsados, parser booleano, telemetria hardcoded e help incompleto.

`help` continha `Commmand`/`Descriptions`, aceitava argumentos extras e expunha
fases internas. Os aliases eram `ps: tasks`, `kill: terminate` e
`taskman: tm, top`, sem validador de colisões. Não existia `taskdiag` canônico.
A string `Phase 3` em `kernel_init.c` permanece registrada como debug de boot e
não é exibida por help, ps, kill ou TASKMAN.

## 2. Itens preservados

Foram preservados scheduler/context switch/runqueues, wait/cancel, timers,
reaper, input router, ownership modal, clock/LAPIC/HPET, layout/navegação CL-11,
`task_handle_t`, snapshot generation e todos os snapshots copy-only. Nenhuma
interface guarda `task_t *`.

## 3. Formatação comum

`kernel/src/core/task_format.{h,c}` é agora a fonte comum para PID, USER,
CPU/LCPU/AFF, state long/compact, class long/compact, quantum, Time+, %CPU,
kill status, MEM~ e nome bounded. Time+ delega para
`task_metrics_format_runtime`; %CPU recebe amostras de
`task_cpu_sampler_sample`; kill status delega para o scheduler. MEM~ usa
B/KiB/MiB/GiB e nomes são sanitizados e truncados com reticências.

A tabela única contém ordem, headers e widths. O heapsort por PID foi movido
para `task_snapshot_sort_by_pid`; `taskman_sort_by_pid` é wrapper compatível.
`taskman_format_name`, `taskman_format_mem`, `taskman_format_row` e
`taskman_build_header` continuam públicos. TASKMAN WIDE usa LONG, COMPACT usa
os nomes curtos. PS usa LONG. O status terminal ZOMBIE tem precedência visual
`ZOMB`, inclusive para fixtures não-killable.

## 4. Parser e resultados de kill

Foi adicionado `task_id_parse_decimal_ex` com `OK`, `ZERO`, `EMPTY`, `SIGN`,
`NON_DECIMAL`, `OVERFLOW` e `INVALID_ARGUMENT`. O wrapper legado continua true
para OK/ZERO. `scheduler_task_kill_result_to_string` centraliza ACCEPTED,
ALREADY_PENDING, ALREADY_ZOMBIE, ALREADY_EXITING, PROTECTED, NOT_KILLABLE,
NOT_FOUND e INVALID.

`cmd_kill` rejeita zero, preserva `UINT64_MAX` no parser, usa mensagens de
cancelamento cooperativo e retorna 0/2/3/4/64..69 conforme o contrato. A
telemetria opt-in deriva do parser e scheduler reais:
`argc`, `parse`, `pid`, `scheduler`, `status`. A extensão estreita de
`killtest` valida parser/result strings e permite cleanup correto depois de a
CLI cancelar o fixture killable.

## 5. PS, paginação e janela de CPU

Uso final: `ps`, `ps <page>` e `ps <page> <page_size>`; defaults 1/32, máximo
128. Cada captura faz probe, calcula page/pages/offset, captura uma única
página e exige generation, total, offset e written coerentes. Mudança reinicia
o probe até quatro vezes; instabilidade persistente não imprime tabela parcial.

A página é ordenada por PID e mostra a ordem canônica
`PID USER CPU LCPU AFF STATE CLASS Q TIME+ %CPU KILL MEM~ NAME`, com título
`HobbyOS TASK SNAPSHOT`, range e comandos Next/Previous. O buffer 128 é apenas
capacidade máxima de uma página, não limite do registry.

PS possui sampler próprio. A primeira invocação emite `baseline=0` e `--`; a
segunda usa o delta positivo desde o `ps` anterior. O sampler frame-a-frame do
TASKMAN permanece independente, compartilhando somente fórmula e formatter.

## 6. Comparação PS/TASKMAN

`taskdiag trace <pid>` ativa um PID exato; `trace off` desativa. PS e TASKMAN
emitem `[TASKVIEW][ROW]` somente para o alvo, usando
`task_format_snapshot_fields`. No fixture real de 16800 bytes, as linhas foram
idênticas: `ZOMBIE`, `Normal`, `00:00.000`, `0.0%`, `ZOMB`, `16.4KiB`, 16800 e
`reap-held`. Trace permaneceu off no encerramento.

## 7. Help, aliases e registry

`ShellCommand` ganhou `details`. `help` aceita zero ou um argumento, resolve
alias para o nome canônico, rejeita extras e imprime Command, Description,
Usage, Aliases e Details. Typos e textos internos foram removidos. PS é
explicitamente uma lista de kernel tasks, não processos POSIX; kill documenta
cancelamento cooperativo; TASKMAN documenta refresh, ESC, navegação e as
constantes V1.

Aliases finais: `ps: tasks, tasklist`; `kill: terminate, taskkill`;
`taskman: tm, top`; `taskdiag: td, tdiag`. `shell_registry_validate` verifica
nome, handler, usage e colisões canonical/alias sem alterar lookup.

## 8. taskdiag

`summary` percorre páginas copy-only de 64 entries e repete o scan quando
generation/total mudam. Ele apresenta states, classes READY, idle, runqueues,
wait membership, kill pending, protected e killable. `task <pid>` usa
`scheduler_snapshot_task_by_id` e mostra campos canônicos, flags, queues,
wait, kill timestamps/count, exit, timers, reap mask e schedule count.

`scheduler`, `reaper`, `modal`, `input` e `accounting` usam somente snapshots e
validadores existentes, sem reset ou trace implícito. `check` valida registry,
scheduler, modal/UI, input, equação `acquired == released + current`,
`free_inflight`, faults estruturais, clock, formatter/parser e trace. O baseline
observado `refs_current=1` é referência ativa, não leak. `selftest` executou 30
casos de formato, 13 de parser e 13 de registry.

## 9. Arquivos alterados

- `kernel/src/core/task_format.{h,c}`
- `kernel/src/core/scheduler.{h,c}`
- `kernel/src/shell/command.h`
- `kernel/src/shell/commands/cmd_{ps,kill,help,taskman,taskdiag}.{h,c}` conforme aplicável
- `kernel/src/shell/commands/cmd_killtest.c`
- `kernel/src/shell/commands/registry.{h,c}`
- `scripts/test-cl12.sh`
- `makefile`, `AGENTS.md`, `docs/development/build-and-qemu.md`
- este relatório

Não houve alteração de arquivo runtime fora da lista permitida.

## 10. Static e build smoke

`scripts/test-cl12.sh static` passou: zero texto UX de fase, zero formatter
duplicado em PS, uso comum em PS/TASKMAN, parser/result strings comuns, help sem
typo, taskdiag registrado, `git diff --check`, stack-check, kernel-check j2,
imagem e `nm -u` vazio. Maior frame: 1984 bytes (`render`, preexistente dentro
do limite); maior frame novo de taskdiag: 832 bytes. Zero warning em arquivo
CL-owned. Warnings históricos permanecem em kernel/bootloader/xHCI e são
non-blocking.

## 11. QEMU SMP=1 TCG

Passaram selftest, summary, scheduler, reaper, modal, input, accounting, check,
matriz help/aliases, parser inválido, argumentos inválidos de PS e dois PS.
Primeiro PS: `baseline=0`; segundo: `baseline=1`, `delta_ns=6900058830` na
execução final registrada. Checks finais de scheduler, sync, accounting, kill,
reaper, input, modal, TASKMAN e taskdiag passaram. O boot foi repetido após a
correção estreita de precedência ZOMB e confirmou o binário final.

## 12. QEMU SMP=4 TCG

Passou com 137 tasks totais durante o fixture de 129 workers:

- página 1/5 size 32: offset 0, written 32;
- página 2/5: offset 32, written 32;
- página 5/5: offset 128, written 9;
- página 1/2 size 128: offset 0, written 128.

Todos os IDs foram únicos dentro da página, ranges/prev/next foram coerentes e
o cleanup removeu os 129 workers. A comparação ZOMBIE passou. A matriz CLI
passou para usage, EMPTY, ZERO, SIGN, NON_DECIMAL, OVERFLOW, UINT64_MAX/NOT_FOUND,
PROTECTED, NOT_KILLABLE, ACCEPTED, ALREADY_PENDING (livre e preexistente),
ALREADY_EXITING e ALREADY_ZOMBIE, com status distintos e mensagens exatas.

`taskdiag task`, `all`, trace-status off e check passaram. Todos os oito checks
finais passaram, com `free_inflight=0`, `refs_current=1`, balanço correto e zero
fault estrutural. QEMU foi encerrado e não deixou pid/socket.

## 13. Faults causais resolvidos

A primeira comparação revelou que o formatter herdado priorizava `NO` sobre
ZOMBIE para o fixture não-killable. A precedência visual terminal foi corrigida
e coberta por selftest; a repetição comparou `ZOMB` em ambas as interfaces. A
segunda tentativa parou porque o transportador HMP não mapeia aspas. O harness
CL-12 passou a enviar `kill ""` com duas teclas `shift-apostrophe`, sem alterar
o transporte global; a CLI real retornou `EMPTY`. Nenhuma regressão ampla ou
soak foi executado durante essas correções.

## 14. Build final, hashes e Git

O stack-check repetido passou com frame máximo de 1984 bytes. Os builds j2 e
jN passaram, produziram o mesmo SHA-256 e `cmp=0`:

`b79eef4b7e863ba9b55308ddca55b5fdf0dfbcca3cc212ac38210e7a14efa0b1`

`deps-check`, imagem e `nm -u` vazio passaram. Hash final da imagem:

`5a3d0c5a26f7bc3039d23f269e9e85215edade09f5c6bb07bca8c2b167ae3288`

A branch/base permaneceram `feat/taskman` e
`ce321da092d109cf49243d34c4c8709c5cf2e3f1`. O fechamento usa exatamente um
commit com a mensagem
`refactor(taskman): align ps, kill and task diagnostics with V1 semantics`.
Artefatos, `.qemu`, binários e o roadmap permanecem fora do stage. Nenhum push,
PR, merge, tag, rebase, stash ou amend foi feito.

## 15. Artefatos

- `artifacts/build/cl12-static.log`
- `artifacts/build/cl12-smp1-tcg.log`
- `artifacts/build/cl12-smp4-tcg.log`
- `artifacts/build/cl12-ps-pagination.log`
- `artifacts/build/cl12-ps-taskman-compare.log`
- `artifacts/build/cl12-kill-cli.log`
- `artifacts/build/cl12-help-aliases.log`
- `artifacts/build/cl12-taskdiag.log`
- `artifacts/build/cl12-final-build.log`
- `artifacts/build/cl12-build-hashes.txt`
- `artifacts/build/cl12-final-hashes.txt`

## 16. Reservado para CL-13

Não foram executados SMP=8 soak, matriz CL-01→CL-12, loops históricos, LAPIC
benchmark ou `scripts/test-cl11.sh all`. Selftests consolidados, matriz completa,
fault injection final e soak SMP permanecem exclusivamente na TMV1-CL-13.
