# TMV1-CL-11-FIX11 — certificação final

## 1. Base e worktree

- Branch: `feat/taskman`.
- Base: `88645901d9393f4796af84cecd9e13d3d6671c70`.
- Fonte de verdade: worktree FIX1–FIX10 não commitado fornecido para a missão.
- `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` permaneceu não rastreado e fora do
  stage.

## 2. Inventário herdado da FIX10

Na entrada, o canário preblock por handle/lifecycle exatos estava implementado;
os loops sem/timer/input 3×1000 haviam passado em SMP=1 e SMP=8; os três modos
modais focados e loops reduzidos 100× em SMP=8 também haviam passado. O core de
wait/cancelamento aprovado foi preservado sem reimplementação.

A auditoria confirmou as lacunas declaradas: timer-noise ausente; harnesses sem
os novos comandos; loops modais 3×1000, heavy runs, regressões e soak ausentes;
input sem reap hold exato; cleanup com retornos prematuros; métricas residuais
literais; controles modais sem reset geral; snapshot sem kill result e com
leituras não atômicas; telemetria BLOCKED falsa no modo worker-before-wait.

## 3. Canário timer-noise

`synctest preblock-timer-noise` executa um worker independente com sleeps de
timer enquanto cada target preblock é validado por handle/lifecycle. O gate
exige avanço global real e, simultaneamente, zero nodes, refs e NORMAL exits do
target. Stats globais não participam do gate de ownership.

## 4. Cleanup preblock

Cada caso retorna um resultado estruturado com estágio, primeiro failure,
snapshots inicial/final, wait/exit reason, canário timer exato, deltas globais e
estado do cleanup. Todo path após criação libera o gate, tenta kill, aguarda
ZOMBIE held, limpa o hook negativo, remove o hold, dirige o reaper e exige
handle ausente, `free_inflight=0` e zero timer nodes/refs antes de liberar o
workspace claim.

## 5. Input com hold exato

O target usa `thread_create_ex_handle()` com `test_reap_hold=1`. A validação
ocorre em ZOMBIE KILLED held e cobre wait inativo/NONE, membership NONE, fila e
waiters vazios, zero phantom; somente depois o hold é removido e o handle é
reaped. O loop agrega cancelled, killed, NORMAL, waiters, phantom, handles gone,
free-inflight e residual medidos.

## 6. Controles e snapshot modal

`modal_ui_test_reset_controls()` e
`modal_ui_test_controls_snapshot()` cobrem todos os one-shots, holds, releases e
telemetria de boundary. O last-run snapshot publica, sob `g_ui.lock`, caller e
worker exatos, resultado do wait, latch de cancelamento, três boundaries,
observação BLOCKED, resultado do kill do worker, exit reason, status, cleanup e
recuperação de sessão. Campos escritos atomicamente são lidos com atomic load.

## 7. Validação por modo e lifetime do contexto

O workspace modal estático tem canários, generation, mode e claim exclusivo.
Antes da reutilização, os handles anteriores devem estar ausentes e o runtime
sem contexto ativo. O cleanup libera gates, reseta controles, mata handles ainda
vivos, recupera owner, aguarda ZOMBIE held, captura reasons, remove holds, dirige
o reaper e mede runtime/session/router/shell/contexts/quarantine. O claim só é
liberado após ambos os handles ausentes e `free_inflight=0`.

- BLOCKED exige wait CANCELLED, latch, observação BLOCKED verdadeira, nenhum
  boundary prewait e caller/worker KILLED.
- PREWAIT exige boundary caller-before-wait, BLOCKED falso, wait CANCELLED,
  latch, delta preblock-cancelled exato e caller/worker KILLED.
- COMPLETION_READY exige completion publicada antes do wait, worker-before-wait,
  BLOCKED falso, wait OK com latch, caller KILLED e worker NORMAL.

## 8. Harnesses

CL-07, CL-09 e CL-10 passaram a executar os comandos FIX11. O harness CL-11
codifica a ordem focused, timer-noise, loops 3×1000, negative causal, cinco heavy
runs, regressões CL-07/08/09/10, transporte HMP, matriz/negatives TASKMAN, soak e
build/hash final. Um único harness possui `.qemu` por vez e todo comando usa
`shell_sync` via `send_complete`.

## 9. Loops exatos e negative preblock

Os artefatos desta execução registram:

- `cl11fix11-focused-smp8.log`: focados sem/timer/input e os três modos modais
  PASS; timer-noise PASS com `count=1000`, `global_armed_delta=3190`,
  `global_ref_delta=3182`, `target_nodes=0`, `target_refs=0` e
  `target_normal_exits=0`.
- `cl11fix11-preblock-{sem,timer,input}-smp{1,8}.log`: seis boots, cada
  workload com `count=1000`, `normal=0`, todos os handles gone,
  `free_inflight=0` e residual zero.
- `cl11fix11-modal-{blocked,prewait,completion-ready}-smp{1,8}.log`: seis
  boots. BLOCKED e PREWAIT tiveram caller/worker KILLED; COMPLETION_READY teve
  caller KILLED e worker NORMAL. Todos registraram `cleanup=1000`,
  `handles_gone=2000`, `unexpected_exit=0` e `residual=0`.
- `cl11fix11-negative-preblock.log`: o build negativo emitiu
  `PREBLOCK_CANCELLATION_LOST_DETECTED`.
- `cl11fix11-negative-reset.log`: rebuild normal e focados subsequentes PASS,
  incluindo `modaltest check` sem test controls residuais.

## 10. Heavy runs

As cinco runs no mesmo QEMU SMP=8 TCG concluíram snapshot-boundary 100,
concurrent-churn 1000/1000/1000, create-handle-race 100000, clock-smp 32/1M,
timer-ref-loop 100, preblock/noise/input 100 e os três modos modais 100. A
sentinela host foi gravada em `cl11fix11-heavy-runs-smp8.log`:

```text
[CL10][FIX11_EXACT_CERT] PASS runs=5
```

Não houve unexpected exit, timer/wait/input/modal residual ou clock anomaly.

## 11. Regressões integrais alcançadas

Antes do gate bloqueante, o harness agregado `scripts/test-cl11.sh all`
concluiu novamente:

- CL-07: matriz SMP=1/2/4/8 e regression soak, com sentinelas
  `[CL07][MATRIX] PASS smp=1,2,4,8` e `[CL07][REGRESSION] PASS`;
- CL-08: positivos, timer-ref determinístico, negatives e soak
  `[REAPTEST][SOAK] PASS cycles=5 duration_ms=180000`;
- CL-09: positivos, preblock-loop 1000 SMP=1/8, negatives, boundary-loop
  causal 1000 e soak `[CL09][SOAK] PASS duration_ms=180000 cycles=4`, seguido
  de `[CL09][CERTIFICATION] PASS`;
- fixture CL-11: três boots SMP=1 e três SMP=8 com fixture-loop 1000.

Uma execução integral independente de `scripts/test-cl10.sh all`, anterior ao
harness agregado mas produzida nesta mesma homologação FIX11, chegou a
`[CL10][CERTIFICATION] PASS`, incluindo o soak modal/HPET. Isso não substitui e
não apaga a falha da reexecução agregada descrita abaixo.

Declarações para o ledger de continuidade FIX15:

- CL-07 integral alcançada;
- CL-08 integral alcançada;
- CL-09 integral alcançada;
- CL-10 integral independente PASS.

## 12. Falha bloqueante da certificação agregada

O caminho abaixo terminou com status 1:

```text
scripts/test-cl11.sh all
  -> scripts/test-cl10.sh all
    -> scripts/test-lapic-rate.sh all
      -> KVM SMP=8, run 1
        -> accounttest lapic-rate 2000 5
```

No mesmo boot, configuração e liveness passaram:

```text
[ACCOUNT][LAPIC_CONFIG] PASS cpus=8 spread_x10=3
[ACCOUNT][LAPIC_LIVENESS] PASS cpus=8 window_ms=500 all_advanced=1
```

O gate de taxa falhou de forma explícita:

```text
[ACCOUNT][LAPIC_RATE] FAIL window_ms=2000 rounds=5
worst_median_x1000=690 min_round_x1000=629 max_round_x1000=905
```

Esse resultado não foi reclassificado como transporte e não foi ocultado por
retry. O serial exato foi preservado em
`artifacts/build/cl11fix11-blocking-lapic-kvm-run1.log`, SHA-256
`af8df7b32fe2861f5056bc909f8f2f781aa24e38feac4c7aa02c1c05b6ae999a`.
Não há panic, `#PF`, `#GP` ou `FATAL` no log. O workload não cria handles; no
boot bloqueante, os stats imediatamente anteriores registravam
`requests=0`, `contexts=0` e `holds=0`. O trap encerrou o QEMU; PID e socket
ficaram ausentes.

## 13. Gates CL-11 não alcançados

Por causa da regra de parada após qualquer gate falho, a execução agregada não
prosseguiu para:

- transporte HMP 25+25;
- matriz positiva TASKMAN SMP=1/2/4/8;
- quatro negatives TASKMAN;
- soak final CL-11 de 180 s e seus mínimos de frames/navigation/churn;
- build reproduzível j2/jN, imagem e hashes finais pós-homologação.

Esses itens permanecem **não certificados**. Resultados antigos não foram
promovidos para preencher a execução interrompida.

## 14. Stack, build, warnings e estáticos

No início do harness agregado:

- `make stack-check`: PASS, maior frame `1984`, limite `2048`;
- `make kernel-check JOBS=2`: PASS, SHA-256 do `kernel.elf`
  `c95a69a8a29f7a92b131d7de90a6fa93e78081c792aac459b7aff4ef798a7a09`;
- warnings CL-owned em `timers.c`, `cmd_synctest.c`, `cmd_inputtest.c`,
  `cmd_modaltest.c` e `modal_ui.c`: zero;
- static gates FIX11: timer-noise, exact reap hold, reset/snapshot de controls,
  worker kill result, telemetria BLOCKED atômica, alias sem corpo legado e
  integração dos loops nos scripts: presentes;
- `git diff --check`: PASS.

O build jN, `cmp`, `make deps-check`, imagem e hash final não foram executados
depois da falha, portanto não são declarados como certificados.

## 15. Git e escopo

- Nenhum commit foi criado, conforme a política para gate falho.
- Nenhum push, PR, merge, tag, rebase, stash ou amend foi executado.
- O worktree FIX1–FIX11 foi preservado.
- `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` permaneceu não rastreado e fora de
  qualquer stage.
- TMV1-CL-12+ não foi iniciada.

## 16. Status

**REJECTED** — a implementação e os gates exatos FIX11 passaram, mas a
certificação final obrigatória falhou no LAPIC rate KVM SMP=8. Pela regra da
missão, não há commit e os gates CL-11 posteriores permanecem pendentes.
