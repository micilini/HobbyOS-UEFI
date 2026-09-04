# TMV1-CL-08-FIX4 — certificação final

Data: 2026-08-01
Branch: `feat/taskman`
Base: `e9834561d66b1bf55a8aed39d7bcde75c16baf7a`

## Base e diagnóstico

O worktree CL-08/FIX/FIX2/FIX3 foi preservado integralmente, incluindo o roadmap
preexistente e não rastreado. O churn antigo executava `workers(false,n)`, depois
`workers(true,k)` e somente então `snapshot_test()`. O snapshot mantinha 64 tasks
vivas por 1000 leituras e só enviava kill ao final. Portanto não havia creators
NORMAL/KILLED coexistentes nem snapshot simultâneo ao reap.

## Concurrent churn

`reaptest concurrent-churn N K S` usa contexto e tabelas de IDs no heap, quatro
tasks independentes e barreira release acquire/release. Os creators usam janelas
de 64 notificações para backpressure; o listener copy-only filtra os nomes do
cenário e exige reasons NORMAL/KILLED. O snapshot pagina em blocos de quatro,
valida offset/written/total/truncated e duplicatas, e reinicia quando a registry
generation muda. O driver aplica pressão pela API real `scheduler_reap_zombies(0)`.

O boundary determinístico publica `mid_snapshot` após a primeira página. Um reap
durante a fase publica `reap_between_pages`; a página seguinte observa generation
diferente e reinicia. O PASS exige ainda os quatro workers ativos simultaneamente,
reaps durante snapshots, contagens completas, listener quiescente e zero holds.

## Regressão CL-07 e harnesses

O defeito era host-side: a sentinela interna podia surgir antes do retorno do
handler, permitindo caracteres do comando seguinte na fila ou no modal. O helper
compartilhado mantém lock exclusivo `.qemu/harness.lock`, fail-fast e diagnóstico
com tail/status. `send_complete` aguarda o PASS e depois executa `killtest stats`;
o marcador novo prova que a shell voltou a consumir comandos. O ciclo TASKMAN
aguarda session begin, ESC, session end e termina em `shell_sync`.

## Evidência funcional inicial

- SMP=2 TCG, `concurrent-churn 20 20 20`: PASS; overlap 1; 38 reaps durante
  snapshots; 2 generation changes/restarts; reasons e contagens 20/20; zero
  duplicates, partial, contexts e holds.

## Matrizes, negatives, soak, builds e hashes

- stack-check: PASS, maior frame 1920/2048, zero violações;
- kernel-check j2/jN: PASS, binários idênticos;
- kernel SHA-256 j2/jN: `37aeed45ac93be6c034cedf2e96df24f01fcda1b1d94794a970bfeef6c24e924`;
- deps-check, image e `nm -u`: PASS;
- SMP=1 TCG, concurrent churn 100/100/100: PASS, overlap e generation retry;
- SMP=4 TCG, concurrent churn 1000/1000/1000: PASS após corrigir a captura
  stale de `now` anterior ao scheduler lock; 2002 reaps durante snapshots, 15
  generation changes, zero duplicates/partial/contexts/holds;
- regressão CL-07 SMP=8 serializada: PASS, soak >=120 s, 10000 timeout rounds,
  smpstress, TASKMAN e checks; log `cl08fix4-cl07-regression.log`;
- smoke `send_complete` + modal + `shell_sync`: PASS.

A matriz CL-08 progrediu por SMP 1/2/4 e chegou ao SMP=8 com positives e
concurrent churn PASS. O gate seguinte falhou exatamente em:

```text
[ACCOUNT][STATS] clock_regressions=830 metric_regressions=0 over100=0
[ACCOUNT][CHECK] FAIL
```

O harness abortou, salvou diagnóstico/tail/status e o trap parou o QEMU. Não
foram executados após essa falha os quatro negatives finais, a matriz CL-07
SMP=1/2/4, nem o soak CL-08 de 180 s. Nenhum desses resultados é inferido.

## Status

**REJECTED.** O gate `accounttest check` SMP=8 falhou com 830 regressões do
relógio sob a carga final. Nenhum commit foi criado. CL-09+ não foi iniciada.

## Fechamento posterior

A entrega FIX4 comprovou o concurrent churn, mas encontrou 830 eventos classificados
genericamente como clock regressions em MTTCG SMP=8. A auditoria também encontrou
lifetime inseguro no retorno raw de thread_create. A classificação, correção e
certificação final estão em `TMV1-CL-08-FIX5-clock-lifetime.md`.
