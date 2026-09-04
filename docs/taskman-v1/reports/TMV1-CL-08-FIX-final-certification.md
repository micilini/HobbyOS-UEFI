# TMV1-CL-08-FIX — tentativa de certificação final

## Estado inicial

- Data: 2026-07-31.
- Branch: `feat/taskman`.
- Base/HEAD: `e9834561d66b1bf55a8aed39d7bcde75c16baf7a`.
- Worktree CL-08 rejeitado foi preservado integralmente.
- Roadmap preexistente permaneceu não rastreado.

## Correções implementadas

- Lifecycle stats passaram a usar exclusivamente o lifecycle lock.
- Listener slots agora possuem generation, active, retiring e in-flight; unregister BUSY não autoriza liberar ctx.
- Event validation cobre identidade, reason, timestamps e terminação do nome.
- Reaper stats deixaram de misturar incremento atômico fora do lock com snapshot comum.
- Deferrals esperados, policy grace e falhas estruturais foram separados.
- Mask adicionou notify-started, zombie generation, timestamp order e coerência de timer refs.
- Timer node ganhou `task_ref_held`; cancel/dispatch usam um único release helper e falha impossível é fatal.
- Hold CLAIMED passou a ser não bloqueante para o BSP.
- Claimed ZOMBIE recebe validação completa antes do free.
- `reaptest check` passou a paginar com registry generation/retry e validar os novos invariantes.
- Placeholders foram removidos; cleanup, notification/unregister, timer-ref, oncpu, stale, heap e snapshot possuem cenários próprios.

## Resultados positivos obtidos

- Core suite SMP=2: NORMAL/KILLED, grace real, batch, snapshot 1000, cleanup hold, notification,
  unregister quiescente, timer CLAIMED ref, on-CPU real, stale e heap PASS.
- Grace: g0 medido, g1 before/after e g3000 before/after PASS.
- Timer ref: CLAIMED=1, defer=1, release=1 e reap=1 PASS.
- On-CPU SMP=2: ON_CPU e CURRENT observados/deferred; reap após switch PASS.
- Heap: warmup + 1000 NORMAL + 1000 KILLED, `used_bytes` drift zero.
- Check: structural=0, violations=0 na execução positiva.
- Build intermediário `make kernel-check JOBS=2`: PASS.

## Bloqueios encontrados

1. O fluxo `reaptest zombie-mem setup` não conclui: a task `reap-held` é criada, mas a transição esperada
   não retorna ao controller antes do timeout de 60 s. Assim ps/TASKMAN MEM~ não foi certificado.
2. O primeiro negative isolado (`HOBBYOS_REAPER_NEGATIVE_AGE_ONLY`) não atingiu a sentinela pelo mesmo
   bloqueio no helper held-ZOMBIE. O script abortou corretamente e os outros três negatives não rodaram.
3. Por consequência, matriz final, `make test-cl07-final` e soak de 180 s não foram executados.

Esses itens são gates bloqueantes explícitos. O build normal foi restaurado após a tentativa negativa;
nenhum commit foi criado e nenhum resultado ausente foi declarado como PASS.

## Status

**REJECTED.** A implementação permanece no worktree para diagnóstico posterior. CL-09+ não foi iniciada.
# Correção posterior

A tentativa FIX2 e seu gate bloqueante estão documentados em
`TMV1-CL-08-FIX2-final-certification.md`.
O FIX3 está documentado em `TMV1-CL-08-FIX3-final-certification.md`.
