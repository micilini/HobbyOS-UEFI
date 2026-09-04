# TMV1-CL-09-FIX — certificação final

## Base e worktree

- Branch: `feat/taskman`.
- Base: `378a1ac77d7424b00d7b95235576dfa28375302f`.
- O worktree CL-09 foi preservado; o roadmap permaneceu não rastreado.

## Correções causais

O grace de 3000 ms agora aceita corretamente o ator que efetivamente removeu a
task: o reaper de background ou a chamada manual. O boundary anterior ao limite
continua exigindo task presente e retorno zero. O concurrent churn usa handshake
request/ready/done/consumed entre a primeira página e um incremento real de
`reaper_stats.reaped`; somente a segunda página com generation distinta conta
`forced_generation_changes` e restart.

A pausa da shell usa loads acquire e stores release. Eventos retirados durante a
janela de pausa são descartados e contabilizados. Falha em
`input_router_end_modal()` restaura a sessão ACTIVE sem retomar a shell.

`input_queue_wait_pop_entry()` compartilha a lógica de wait com o wrapper normal.
O teste de producers valida a sequence real; drain bloqueia um waiter real; o
negative phantom executa permits legados numa fila test-only. Producers de
boundary chamam dispatch enquanto o router lock está preso. Route mismatch,
phantom wakes e invariantes do ring são calculados.

## Evidência intermediária

- SMP=1 churn 100/100/100: PASS, generation=1, forced=1, restarts=1.
- Grace: PASS, `g3000_after=1`, ator manual na amostra.
- SMP=4 producers: PASS, 100.000 consumidos, gaps/duplicatas/regressões zero.
- SMP=4 drain e boundaries begin/end/rollback/pause: PASS.
- Negatives phantom permit e rota não atômica: DETECTED; positivos normais: PASS.

## Certificação final

- Stack-check: PASS, maior frame 1920 bytes, violações zero.
- Builds `JOBS=2` e `JOBS=12`: PASS e byte a byte idênticos.
- Kernel SHA-256: `6a8b17ef60f9349c896fe512e4d2d200c7b908ad62a3dd247178b9d590f48553`.
- Imagem SHA-256: `889f9b0abd5e4a0bc786cc3c067917b728268d0a483c993591a08b5fe943594c`.
- `deps-check`, imagem e `nm -u`: PASS; nenhum símbolo indefinido.
- Matriz CL-09 SMP=1/2/4/8: PASS. Producers: 100.000 aceitos e
  consumidos, 50.000 CHAR/50.000 SPECIAL, gaps/duplicatas/regressões zero.
- Drain: 73 pops, 183 drenados, phantom zero e um wake real.
- Boundaries reais: 1.000 begin + 1.000 end, `causal=1`. O controller
  permaneceu executável enquanto a transição segurava o router lock, o producer
  chamou dispatch e cada handle foi aguardado até ZOMBIE/desaparecer.
- End rollback e shell pause boundary: PASS.
- Transições 10.000, keyboard 10.000 e leakage HMP: PASS.
- Negatives isolados phantom permit e rota não atômica: DETECTED; rebuild normal
  e positivos correspondentes: PASS.
- Estabilização CL-08: três churns SMP=1 com forced generation=1 e três
  grace SMP=8 com `g3000_after=1`.
- Regressão CL-07: `[CL07][MATRIX] PASS smp=1,2,4,8`.
- Regressão CL-08: matriz, ZOMBIE MEM~, quatro negatives e soak PASS; soak
  final com 9 ciclos/180000 ms.
- Soak CL-09 final: PASS, 180000 ms, 7 ciclos de carga, 1.000 boundaries
  causais, backlog observado até zero antes de 100 ciclos TASKMAN e checks
  scheduler/sync/account/kill/reap limpos.
- Warnings CL-owned de misleading indentation: zero. Permanecem apenas warnings
  legados de bootloader/XHCI/unused fora do escopo.

## Status

**APROVADO.** Nenhum QEMU ficou ativo, as macros negative foram removidas pelo
rebuild normal e CL-10+ não foi iniciada.
