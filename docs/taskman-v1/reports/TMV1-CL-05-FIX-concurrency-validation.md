# TMV1-CL-05-FIX — Homologação concorrente

- Data: 2026-07-30
- Branch: `feat/taskman`
- Base: `0296f663437059c03f221a66037d83a5c9075cce`
- Fase: correção e homologação da CL-05; CL-06 não iniciada.

## Diagnóstico e arquitetura final

Os comandos anteriores `sem`, `boundary`, `cancel` e `stale` não exerciam concorrência ou lifetime real. O harness foi refeito com controller e workers separados, atomics de conclusão, watchdog e nomes estáveis.

O fallback de sleep agora consulta explicitamente `scheduler_current_context_can_block()`. Contexto sem task, bootstrap ou Idle usa atraso monotônico: `hlt` somente com IF habilitado e `pause` com IF desabilitado. Idle nunca é publicada como waiter.

Wake usa `SCHED_WAKE_NO_MATCH/WON/FATAL`. Falha de enqueue após uma wake vencedora não vira permit: locks são liberados e ocorre panic. Rollback limpa state, membership, active, kind, reason, key e deferred-ready.

Timers mantêm listas PENDING e CLAIMED protegidas pelo timer lock. `timers_cancel()` distingue CANCELLED, NOT_FOUND e ALREADY_CLAIMED de forma alcançável. Dispatch remove da lista claimed antes do callback, executa fora do lock e libera uma vez. Estatísticas compartilhadas usam operações atômicas.

## Harness

- `sem N W S`: waiters bloqueiam por `sem_wait_interruptible`; signalers distribuem exatamente N sinais.
- `boundary N`: waiter publica prepare pelo hook e controller concorrente sinaliza a fronteira; handshake impede sobreposição de rodadas.
- build negativo: move prepare após unlock e força sinal no gap.
- `cancel`: tasks KILLABLE reais bloqueadas em sem e sleep recebem CANCELLED, inclusive request duplicado.
- `race N`: worker repete sleeps curtos enquanto killer disputa cada geração; exatamente TIMEOUT ou CANCELLED.
- `sleep`: 32 workers, 8 por duração 1/2/10/100 ms, 100 iterações.
- `stale`: usa task real, captura ID/lifecycle/wait, cancela, reap e injeta wake antiga.
- `timercancel`: cancel pending, duplicado e claimed; callback no máximo uma vez.
- `fallback` e `oom`: validam flags/Idle e limpeza completa.

## Evidência runtime

- SMP=1 TCG: fallback PASS; 3.200 sleeps PASS; checks scheduler/sync PASS.
- SMP=2 auto: sem 100.000 PASS; boundary 10.000 PASS; lost/stuck/errors zero.
- SMP=4 auto: sem 100.000; boundary 10.000; cancel sem/sleep; race 5.000 (22 timeout, 4.978 cancelled); sleeps; stale pós-reap; timer claimed; fallback; OOM; checks — todos PASS.
- SMP=8 auto: sem 200.000; boundary 20.000; race 10.000 (108 timeout, 9.892 cancelled); cancel, sleeps, stale, timer claimed e checks — todos PASS.
- Scheduler final das matrizes: started==finished, pending=0, violations=0.
- TASKMAN SMP=4: 20 pares `session_begin OK` / `session_end OK`; shell recebeu `help` após cada ESC.
- USB/XHCI: boot, enumeração HID, shell e input funcionais; timers genéricos continuam callback-kind, não task-wake.
- Soak SMP=8 >=90 s: rodadas de sem 200k, schedtest 32x5000, race 5000, ps e checks. Mais de 1,9 milhão de switches; violations zero; sem panic/#PF/#GP/FATAL.
- Negative: `[SYNC][NEGATIVE] OLD_SEM_GAP_DETECTED`, seguido de rebuild normal e boundary PASS.

Artefatos: `artifacts/build/cl05fix-smp{1,2,4,8}-serial.log`, `cl05fix-soak-smp8-serial.log`, `cl05fix-taskman-20cycles.log` e `cl05fix-negative-lost-wake.log`.

## Gates estáticos e regressões

Zero task pointer em timer wake; zero `thread_block` nos caminhos migrados; zero `hlt` associado a irq-save; estados claimed e wake status presentes; rollback explícito. Warnings apenas legados XHCI/unused, GNU-stack e RWX.

CL-01 build reproduzível, CL-02 slots SMP, CL-03 identidade/guards, CL-03-FIX IDs e CL-04 handoff permanecem verdes. Accounting, lifecycle geral, killability completa, reaper geral e input/UI permanecem fora do escopo.

## Status

APPROVED. Todos os gates bloqueadores foram executados e registrados.

## Builds finais

- kernel-check JOBS=2: PASS
- kernel-check JOBS=nproc: PASS
- ELF SHA-256 (ambos): `619fb94afdbbd35bd350c8c8e0e469d41f41afd49d8d7a38d9293c60977e24a0`
- `cmp`: 0
- deps-check: PASS
- imagem final SHA-256: `36cdaae0005f74119f3eac9d969d3a0fed95007fc2136aa29604d41440dbdf51`
