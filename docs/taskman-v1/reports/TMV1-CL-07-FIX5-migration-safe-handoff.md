# TMV1-CL-07-FIX5 — Migration-safe CPU-local handoff

## 1. Base e entrada

- Branch: `feat/taskman`.
- Base/HEAD de entrada: `108164f08cc04f94a7f4c4a8d7c69f9097e4082c`.
- A entrada continha as alterações legítimas, não commitadas, da FIX4. Elas foram preservadas.
- `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` permaneceu preexistente, não rastreado e fora do commit.

## 2. Diagnóstico: 0x47 e timeline

O mask observado é `0x47 = 0x01 | 0x02 | 0x04 | 0x40`: handoff inativo, `prev == NULL`,
`next == NULL` e `cpu.current != prev`. Os guards da FIX4 registraram zero corrupção e zero RSP fora
dos limites, descartando overflow como causa única.

Timeline confirmada: uma task lia slot A com IF=1; o LAPIC a preemptava; ela era retomada/migrada em B;
a chamada antiga continuava usando A, instalava o handoff em `cpu[A]`, mas o switch físico ocorria em B.
O finish em B encontrava o estado vazio e produzia 0x47. O handoff abandonado em A permanecia ativo,
explicando diretamente o próximo `pending handoff overwrite` e a cascata de `invalid switch begin`.

## 3. Pinning e ordem de entrada

Foi introduzido `scheduler_cpu_pin_t` com flags de IRQ, slot, APIC, task e estado ativo, além de
`scheduler_cpu_pin`, `scheduler_cpu_pin_validate` e `scheduler_cpu_unpin`. O pin executa
`irq_save()` antes de resolver APIC/slot/current e o unpin restaura exatamente as flags originais.
`irq_is_enabled()` sustenta a assertion dos helpers internos pinned.

`schedule_impl` agora executa `irq_save -> clock -> slot/current/APIC -> scheduler lock`. Sob o lock
valida APIC, slot, initialized/online, current, on_cpu, current_cpu_slot, handoff e stack. No caminho sem
switch restaura flags diretamente. No caminho com switch, as flags ficam no frame suspenso; o finish
libera somente o scheduler lock e o frame retomado restaura IF uma única vez.

`thread_exit_with_reason` faz tanto o claim inicial quanto a conclusão pós-cleanup com IRQs desabilitadas
antes de slot/lock e exige que `cpu[slot].current` continue sendo a mesma task. O claim também resolve
`kill_pending + NORMAL` para `KILLED` sob o lock, fechando a janela ACCEPTED+NORMAL encontrada no
primeiro SMP=8 de 10.000 rounds; a repetição passou com `accepted_normal=0`.

`get_current_task()` passou a capturar slot/current dentro de `irq_save/irq_restore`. Para invariantes
task+CPU, os callers usam o token de pin, não apenas esse retorno. `timer_sleep_interruptible` deriva
ID e gerações exclusivamente de `token.task`.

## 4. Auditoria de callers

| Caller/classe | Classificação e ação |
|---|---|
| `thread_set_current_name`, cancel helpers | identidade lógica; `get_current_task` agora é pinned internamente |
| `scheduler_current_context_can_block`, block legado | invariantes CPU+task; slot/current pinned |
| `thread_exit_with_reason` | dois claims CPU+task com irq_save antes do slot e validação sob lock |
| `timer_sleep_interruptible` | identidade do block token; removido current prévio |
| accounttest migrate worker | telemetria via pin validado, sem dereference CPU após unpin |
| smpstress cleanup e synctest OOM | identidade lógica da própria task |
| request/consume reschedule e LAPIC accounting | contexto IRQ, IF=0 por hardware |
| boot/init callers de SMP slot | contexto boot, sem scheduler/preempção ativa |
| test telemetry | snapshot ou API pública de pin validado |

A busca por `get_current_task`, helper antigo e `smp_current_cpu_slot` foi revisada em todo
`kernel/src`; não restou captura slot→lock com IF habilitado em caminhos CPU-local fortes.

## 5. ABI e handoff exato

`switch_context(prev,next,cpu_state,sequence)` usa System V AMD64: RDI/RSi para tasks e RDX/RCX para
ponteiro/sequence. Após trocar a stack e restaurar callee-saved, assembly move RDX→RDI e RCX→RSI,
alinha a stack com `sub rsp,8`, chama `scheduler_finish_switch(cpu_state,sequence)` e retorna.
`objdump` confirmou a sequência tanto em `switch.o` quanto em `kernel.elf`; `thread_wrapper` foi
preservado.

O handoff guarda expected_slot, owner_apic_id, sequence, executing_task_id, prev e next. O finish valida
primeiro que o ponteiro identifica exatamente um elemento do array, depois slot físico, APIC, active e
sequence. Foram adicionados FF_CPU_POINTER, FF_WRONG_CPU, FF_WRONG_APIC, FF_SEQUENCE e
FF_LOCK_OWNERSHIP, além dos contadores de runtime correspondentes.

`owns_handoff` exige ponteiro válido, active e sequence esperada. Somente nesse caso um finish inválido
libera `g_scheduler_lock` antes do panic; finish espúrio não toca em lock possivelmente pertencente a
outra CPU.

## 6. Testes novos e negatives

- Negative stale capture: build com `HOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE` detectou
  `[SCHED][NEGATIVE] STALE_CPU_SLOT_CAPTURE_DETECTED` sem executar switch corrompido; rebuild normal feito.
- `schedtest cpu-pin`: 50k/100k/200k conforme SMP, com migrações reais e zero stale/current/wrong/sequence.
- `schedtest entry-window 200000`: PASS em SMP=4 e SMP=8, migrações reais, mismatches zero.
- TCG SMP=4: `killtest ui-cli-repro 500` PASS no mesmo QEMU; timeout-race 5000 PASS; sweep de 8 workers PASS.
- Negatives isolados reexecutados: NONKILLABLE_ACCEPTANCE_DETECTED,
  EXITING_ACCEPTED_NORMAL_DETECTED, NEXT_READY (mask 0x200),
  STACK_GUARD_CORRUPTION_DETECTED e STALE_CPU_SLOT_CAPTURE_DETECTED.
- A suíte `make test-cl07-final` passou e recertificou o soak/matriz CL-07 legado.

## 7. Matriz e soak

- SMP=1 TCG: killtest all, timeout-race 500, cpu-pin 50000 e checks PASS.
- SMP=2 TCG: killtest all, timeout-race 2000, cpu-pin 100000 e checks PASS.
- SMP=4 TCG: killtest all, timeout-race 5000, UI/CLI 500, cpu-pin/entry 200000, smpstress e checks PASS.
- SMP=8 TCG: killtest all, timeout-race 10000, cpu-pin/entry 200000, sweep 16 e checks PASS.
- Soak limpo SMP=8: mais de 180000 ms na mesma VM, timeout-race aprovado antes da carga, 16 workers,
  cpu-pin e entry-window concorrentes, ps/TASKMAN, checks e stack runtime. Sweep final 16/16.
- Checks finais: stale=0, current mismatch=0, wrong finish CPU=0, sequence mismatch=0,
  finish without pending=0, pending overwrite=0, violations=0, lost wakes=0 e métricas válidas.
  Nenhum panic/#PF/#GP/FATAL ocorreu no artefato positivo.

Nota não bloqueante: uma execução diagnóstica inicial SMP=8 expôs um ACCEPTED+NORMAL e motivou o claim
atômico descrito acima. Uma tentativa de timeout-race simultânea ao smpstress produziu killed=2000 e
normal=0 (falha apenas do requisito de distribuição do harness); o soak foi repetido limpo e aprovado.

## 8. FIX4 e stack safety

FIX4 permaneceu integral: guard de 512 bytes, low-watermark, diagnóstico detalhado de finish,
snapshot copy-only, sampler sem banco automático e workspaces persistentes. `make stack-check` passou
com 73 arquivos, máximo estático de 1920 bytes, limite 2048, zero violações e zero dynamic-unbounded.
Nos positivos, guards e RSP bounds permaneceram em zero falhas.

## 9. Builds, hashes e imagem final

- `make kernel-check JOBS=2`: PASS.
- `make kernel-check JOBS=12`: PASS.
- Ambos: `62713337f1edce622d933f917e40f1152567447a78331048d2b48458bd68cfa0`; `cmp` igual.
- `make deps-check`: PASS.
- `make image`: PASS.
- `nm -u kernel.elf`: vazio.
- `kernel.elf`: `62713337f1edce622d933f917e40f1152567447a78331048d2b48458bd68cfa0`.
- `hobbyos.img`: `aa3d3ad0d0005a7b89964f0f8ed22df30bc006abd34ee23ee26beeea034765a7`.

## 10. Artefatos e regressões

Artefatos principais: `artifacts/build/stack-usage-report.txt`,
`cl07fix5-negative-stale-slot.log`, `cl07fix5-ui-cli-repro-smp4-tcg.log`,
`cl07fix5-smp1.log`, `cl07fix5-smp2.log`, `cl07fix5-soak-smp8.log` e
`cl07gate-soak-continuous-smp8.log`, além dos negatives FIX5 isolados.

CL-01 a CL-07 (incluindo FIXes anteriores), switch/runqueue, waits/timers, accounting, UI/CLI,
killability/cleanup, snapshots/sampler e stack safety foram recertificados pelos builds, checks,
matrizes e soak. Nenhuma funcionalidade CL-08+ foi iniciada.

## 11. Status

APPROVED WITH NON-BLOCKING NOTES. A causa raiz de migração foi corrigida, o handoff está ligado ao
switch físico exato, os invariantes ficaram zerados e o artefato final não contém macro negativa ativa.
