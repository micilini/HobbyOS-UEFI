# TMV1-CL-07-FIX2 — certificação final

Data: 2026-07-31. Branch `feat/taskman`. Base `aaa6a7b9c5e893c97a4df7645f9e8061570f3177`.

## Correções

Stats e counters de task usam exclusivamente o scheduler lock. `cancellation_points` é reclamado uma vez no exit KILLED; callback é contabilizado na conclusão sob lock. O harness usa acquire/release em flags, counters atômicos relaxed, paginação com retry por geração, validação sem underflow e quarantine em falha de lifetime. Hold de reap é capturado na publicação e rejeita ID inexistente.

Timeout-race usa buckets ao redor de 4 ms, cancellation point pós-timeout e logging lifecycle silencioso durante stress. Esta última correção eliminou `ACCEPTED+NORMAL` encontrado pela primeira execução SMP=1.

## Evidência

- SMP1/2/4/8 core: PASS.
- timeout-race: 500 `125/375`; 2000 PASS; 5000 `1246/3754`; 10000 `2487/7513` (NORMAL/KILLED), zero accepted-normal/not-found/stuck.
- negatives: `NONKILLABLE_ACCEPTANCE_DETECTED` e `EXITING_ACCEPTED_NORMAL_DETECTED`.
- smpstress: SMP4 8/8; SMP8 16/16 em três sweeps; killed=cleaned=reaped.
- CLI HMP: argc, parse, invalid, NOT_FOUND e protected observáveis em `cl07final-cli.log`.
- ps/TASKMAN: telemetria opt-in, session end e retorno ao shell em `cl07final-ui-status.log`.
- soak SMP8: duas janelas de 60 s, core repetida, workers e sweeps; scheduler started=finished=92776; sync/account/kill checks PASS; zero faults.

Artefatos: `artifacts/build/cl07final-smp{1,2,4,8}.log`, `cl07final-negative-*.log`, `cl07final-smpstress-smp4.log`, `cl07final-cli.log`, `cl07final-ui-status.log`, `cl07final-soak-smp8.log`.

## Nota de aceite

A infraestrutura `ui-setup/ui-cleanup` para manter simultaneamente PROT/NO/-/PEND/EXIT/ZOMB não foi implementada; a UI foi homologada com tasks reais disponíveis, predominantemente PROT. O sweep CLI de `-1/+1` não foi automatizado devido à limitação do typer HMP. Status: `REJECTED` até esses dois gates formais serem fechados. TMV1-CL-08 não iniciada.

## Fechamento dos gates finais

A entrega FIX2 permaneceu rejeitada por matriz UI incompleta, sweep CLI parcial e soak não contínuo.
A certificação final está em:
TMV1-CL-07-FIX3-final-gates.md
