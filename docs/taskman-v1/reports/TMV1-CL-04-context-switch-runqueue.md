# TMV1-CL-04 — Context switch e runqueue corretos em SMP

## Identificação

- Data: 2026-07-30
- Branch: `feat/taskman`
- Commit-base: `22ea989f1c04f47fbbebb6ab94e1bb777c7a8de6`
- Escopo: CL-04; CL-05+ não iniciadas

## Race e arquitetura

Antes, o scheduler publicava `prev=READY`, reenfileirava `prev`, publicava `current=next` e soltava o lock antes de trocar `rsp`. Outro CPU podia retirar `prev` e executá-la enquanto sua stack ainda estava fisicamente ativa.

```text
antes: lock -> enqueue(prev) -> current=next -> unlock -> troca rsp
agora: lock -> remove(next) -> handoff ativo -> troca rsp
                                            -> finish na stack next
                                            -> prev off-CPU/requeue
                                            -> next current/RUNNING -> unlock
```

O estado per-CPU contém `prev`, `next`, sequência, `active`, `started_count` e `finished_count`. `task_t` contém `on_cpu`, `current_cpu_slot` e `deferred_ready`. O finish valida o handoff sob o lock, retira fisicamente `prev` do CPU, consome wake deferida quando existente, reenfileira apenas outgoing runnable comum, publica `next` e libera com `spin_unlock()`.

## Stack, ABI, IRQ e lock

A stack inicial contém, do menor endereço para o maior: slots restaurados de `r15,r14,r13(arg),r12(entry),rbp,rbx`, seguidos pelo endereço de `thread_wrapper`. O topo de 16 KiB é alinhado a 16 e sete qwords produzem `rsp % 16 == 8`. Uma stack retomada salva seis registradores a partir da entrada ABI (`rsp % 16 == 8`) e preserva o mesmo módulo.

Após os seis `pop`, `rsp % 16 == 8`; `sub rsp,8` produz alinhamento 16 antes de `call scheduler_finish_switch`; o callee entra com módulo 8. `add rsp,8; ret` alcança `thread_wrapper` em task nova ou o frame antigo de `switch_context` em task retomada. `thread_wrapper` mantém `sti` depois do finish. O frame retomado executa seu próprio `irq_restore(flags)`. O finish não restaura IRQ e libera exatamente o lock herdado.

## Guards, wake e validação

`enqueue_task_locked` rejeita NULL, membership existente, on-CPU, estado não READY, Idle, ZOMBIE e objeto não registrado. `runqueue_take_next_locked` detach primeiro e rejeita membership/state/registry/on-CPU/Idle/current inválidos. Idle nunca entra na runqueue. Wake de BLOCKED/SLEEPING ainda on-CPU remove wait membership e marca `deferred_ready`; somente o finish, após limpar on-CPU, publica READY.

`scheduler_validate_runtime_invariants` verifica CPUs/current, unicidade, handoffs, runqueues, registry, IDs, Idle/ZOMBIE e telemetria sem corrigir estado. `schedtest check|stats|yield|reap0` expõe checks e workers finitos sem timer. Selftests de boot incluem `SWITCH_GUARDS_OK`.

## Arquivos

- `kernel/src/core/task.h`, `scheduler.h`, `scheduler.c`, `switch.S`
- `kernel/src/shell/commands/cmd_schedtest.{c,h}`, `registry.c`
- `makefile`, `AGENTS.md`
- este relatório

## Build e inspeção estática

- `kernel-check JOBS=2`: PASS
- `kernel-check JOBS=12`: PASS
- ambos SHA-256: `f061cfadc71c48754fc4744f74aba687d7a24ee8144182540209e6389561c9e3`
- `cmp`: 0
- `deps-check`: PASS
- imagem normal: PASS; SHA-256 antes da restauração final `2c2734987f5d7a717d81babc153337429e4b6988a3d7bdc746e3990123e76c8d`
- `objdump`: troca de `rsp`, seis pops, `sub rsp,8`, call relocada para finish, `add rsp,8`, `ret`
- `nm`: `scheduler_finish_switch`, `switch_context` e `thread_wrapper` presentes
- buscas obrigatórias: nenhum `enqueue_task_locked(prev)` antes do switch; nenhum unlock antes de `switch_context`; `current = next` somente no finish; assembly chama finish; guards consultam on-CPU

Warnings: somente legados XHCI/unused, GNU-stack de `entry.S` e LOAD RWX; nenhum warning novo de assembly, ponteiro, stack ou declaração implícita.

## QEMU e stress

| cenário | resultado |
|---|---|
| SMP=1 TCG, 16×1000 yields | PASS; started=finished=16317, pending=0, violations=0 |
| SMP=2 auto, 32×5000 yields | PASS; 160377/160377, pending=0, violations=0 |
| SMP=4 auto, 32×5000 yields | PASS; 160507/160507, pending=0, violations=0 |
| SMP=8 auto, 32×5000 yields | PASS; 160433/160433, pending=0, violations=0 |
| quantum 1, SMP=4, 32×5000 | PASS; 160863/160863, pending=0, violations=0 |
| soak SMP=8, 60 s | PASS; CPU-bound + yield + seis snapshots/checks + seis sessões TASKMAN; final 11988/11988, pending=0, violations=0 |
| negativo sem finish, SMP=1 | PASS do harness: parou após primeiro switch/DPC; shell não alcançada em 12 s |

A matriz alcançou shell, preservou BOOT/ID/FLAGS/REGISTRY/QUEUE/SNAPSHOT/IDENTITY e `SWITCH_GUARDS_OK`, sem PANIC, #PF, #GP ou FATAL. TASKMAN produziu `session_begin/session_end` e a shell retomou após ESC. O soak exercitou preempção IRQ com 16 workers CPU-bound, snapshots e reaper grace zero; o controle retornou PASS e não liberou on-CPU.

Artefatos: `artifacts/build/cl04-smp{1,2,4,8}-serial.log`, `cl04-quantum1-smp4-serial.log`, `cl04-soak-smp8-serial.log`, `cl04-negative-no-finish.log` e logs `kernel-check-j*.log`.

## Riscos restantes e status

A wake deferida é deliberadamente estreita. Atomicidade integral de wait queues/semaforos, wake reasons, timers/generation/cancelamento e lifecycle completo permanecem para CL-05+. Não foram implementados prepare/commit block nem qualquer item CL-05 posterior.

Status final: APPROVED WITH NON-BLOCKING NOTES (warnings legados e serial concorrente já conhecidos).
