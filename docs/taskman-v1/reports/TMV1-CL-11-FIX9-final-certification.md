# TMV1-CL-11-FIX9 — continuação dos canários preblock

## Correção e certificação posterior

A FIX9 adicionou os loops preblock e o negative direcionado. O primeiro loop SMP=1
falhou no timer após 45 ciclos porque o canário comparava contadores globais de
timers; o reaper periódico podia armar seu próprio timer entre os snapshots. A
correção está em `TMV1-CL-11-FIX10-final-certification.md`.

## Inventário

O worktree continha o prepare interruptível, as migrações sem/timer/input, stats,
invariant runtime, canários focados e latch/observer modal FIX8. Loops, mutation
direcionada e hooks modais ainda estavam ausentes.

## Implementado

- O negative global foi substituído por estado sob scheduler lock direcionado por
  handle, lifecycle, wait kind e object key, consumido uma única vez.
- `synctest preblock-negative` observa BLOCKED + kill_pending + wait ativo + wake NONE,
  sinaliza o semáforo, limpa o hook e dirige o reaper.
- `synctest preblock-loop` e `inputtest preblock-loop` executam casos silenciosos e
  restauram lifecycle logging.
- Hooks one-shot caller-before-wait e allow-worker-before-wait foram adicionados ao
  runtime modal; os comandos modais separados ainda não foram implementados.

## Evidência

- Build normal: PASS.
- Loop reduzido SMP=8: sem=100, timer=100, input=100, residual=0, violations=0.
- Negative isolado: `PREBLOCK_CANCELLATION_LOST_DETECTED`; rebuild normal concluído.
- Artefatos: `cl11fix9-preblock-loop100-smp8.log` e
  `cl11fix9-negative-preblock.log`.

## Falha bloqueante

O primeiro loop obrigatório SMP=1 falhou em sem=46/timer=45. O contador de violations
impresso era lixo de stack porque o harness não inicializava a struct quando o
validator retornava false; essa impressão foi corrigida, mas o gate não foi repetido.
Log: `artifacts/build/cl11fix9-preblock-smp1-boot1-failure.log`.

## Status

REJECTED. Nenhum commit foi criado. Modos modais, loops restantes, heavy runs,
regressões, CL-11, soak e hashes finais permanecem pendentes.
