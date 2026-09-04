# TMV1-CL-03 — Identidade, flags imutáveis e invariantes de task

## Identificação

- Data: 2026-07-30
- Branch: `feat/taskman`
- Commit-base/HEAD inicial: `ed8c3871adc72f82c78fb8bf666b7fbf6413c61e`
- Worktree inicial: limpo exceto `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md`, não rastreado e preservado
- Escopo: somente CL-03; context switch, semáforos, timers, accounting e UI não foram redesenhados

## Diagnóstico do baseline

O baseline usava `int` para IDs, PID zero em todas as Idles, proteção de kill por nomes, inferência de membership por ponteiros de lista e snapshot sem offset, truncamento ou geração. Isso tornava lookup/baselines ambíguos em SMP e permitia corrupção silenciosa por add/enqueue duplicado.

## Arquitetura implementada

### Identidade

`task_id_t` é `uint64_t`; `TASK_ID_INVALID` reserva zero. O allocator global começa em 1, é monotônico, detecta exaustão em `UINT64_MAX` e nunca retrocede no reap. A alocação ocorre sob `g_scheduler_lock` antes de publicação.

Cada Idle usa o mesmo allocator, recebe flags `IDLE|SYSTEM|KILL_PROTECTED`, associa `last_cpu_id` ao slot lógico e usa `idle/cpuN`. IDs observados: SMP1 `cpu0=1`; SMP2 `cpu0=1,cpu1=2`; SMP4 `cpu0=1..cpu3=4`; SMP8 `cpu0=1..cpu7=8`.

### Flags e criação

Flags: IDLE, SYSTEM, USER, KILL_PROTECTED, KILLABLE (reservada) e MODAL_UI (reservada). A proteção de kill consulta somente `TASK_FLAG_KILL_PROTECTED`; rename não altera flags. Idles, input-thread, shell-thread, reaper e dpc-worker são classificadas explicitamente nos call sites. SYSTEM não implica proteção.

`thread_create_ex` é o caminho central. Ele valida classe/flags, aloca objeto e stack, atribui ID, conclui nome/contexto e somente então registra e enfileira. Wrappers existentes delegam a ele. Falhas anteriores à publicação liberam os recursos.

### Membership e snapshot

`registry_registered` controla registry; `queue_membership` distingue NONE, runqueue interativa, runqueue normal e WAIT. Helpers rejeitam add/remove/enqueue duplicado, limpam nodes após detach e atualizam `g_all_tasks_count` somente em transições reais. `g_task_registry_generation` incrementa em add/remove bem-sucedido.

`scheduler_snapshot_tasks` retorna `written`, `total`, `offset`, `truncated` e `registry_generation`, copia dados sob o lock e aceita offsets fora do total. PS e TASKMAN usam o novo resultado e baselines `task_id_t`.

O parser decimal rejeita vazio, sinal, trailing data e overflow antes de multiplicar; aceita todo `uint64_t`, inclusive `UINT64_MAX`. Impressão serial/console usa caminhos de 64 bits.

## Arquivos alterados

- `kernel/src/core/task.h`: tipo, flags e memberships.
- `kernel/src/core/scheduler.h`: criação explícita, snapshot e IDs públicos.
- `kernel/src/core/scheduler.c`: allocator, Idles, criação, registry/queue, snapshot, parser, validação e telemetria.
- `kernel/src/core/kernel_init.c`: classificação das tasks críticas e gate de identidade.
- `kernel/src/core/dpc.c`: dpc-worker protegida por flag.
- `kernel/src/shell/commands/cmd_kill.c`: parser/ID de 64 bits.
- `kernel/src/shell/commands/cmd_ps.c`: baseline e snapshot novos.
- `kernel/src/shell/commands/cmd_taskman.c`: baseline e snapshot novos.
- este relatório.

## Toolchain

- GCC 13.3.0
- GNU ld 2.42
- GNU Make 4.3
- QEMU 8.2.2
- Python 3.12.3
- GNU-EFI: `/usr/include/efi`, `/usr/lib`
- OVMF combinado: `/usr/share/ovmf/OVMF.fd`
- KVM: disponível; `ACCEL=auto` selecionou KVM

## Testes

| ID | Comando/cenário | Resultado | Evidência |
|---|---|---|---|
| B1 | baseline `kernel-check JOBS=2` | PASS | hash `fe27920d09fa4a0762e6acda073aeab18b3eff533d9bd153cca626f1f2416c70` |
| B2 | baseline `kernel-check JOBS=12` | PASS | mesmo hash |
| B3 | baseline `deps-check` / `image` | PASS | imagem `df2aa9c58d97cd2a2bbff671f9a76743e6654f8c830aced2211b065146c8b1d8` |
| F1 | final `kernel-check JOBS=2` | PASS | `artifacts/build/kernel-check-j2.log` |
| F2 | final `kernel-check JOBS=12` | PASS | `artifacts/build/kernel-check-j12.log` |
| F3 | reprodutibilidade | PASS | ambos `2958d08a00ca4d8c5b9d5d2ba9c64eb8e835dd20f160f8131573377e31a19307` |
| F4 | `make image` | PASS | `8d35a494c5b760c8f732de257bfa6fe01fdeeb888985ab9adf253ff5f21b2e90` |
| S1 | SMP=1 TCG | PASS | identity total=5, idles=1; shell pronta |
| S2 | SMP=2 auto/KVM | PASS | identity total=6, idles=2; shell pronta |
| S4 | SMP=4 auto/KVM | PASS | identity total=8, idles=4; shell pronta |
| S8 | SMP=8 auto/KVM | PASS | identity total=12, idles=8; shell pronta |
| ST | selftests | PASS | FLAGS_RENAME, REGISTRY_GUARDS, QUEUE_GUARDS, SNAPSHOT_CONTRACT e ID_PARSE OK em todos os boots |
| CH | churn SMP=4 | PASS | `smpstress 64 0 1000 9999 20 250`; workers 0..63 criados, IDs monotônicos não-zero; `smpstress-63` observado |
| KI | kill de Idle | PASS | SMP8: `[TASK][KILL] result=PROTECTED id=2` para `idle/cpu1` |
| TM | TASKMAN/HMP | PASS | session_begin/session_end antes e depois do churn; `help` enviado depois |
| PF | busca panic/fault | PASS | nenhum PANIC/#PF/#GP/FATAL nos quatro logs finais |
| QS | stop controlado | PASS | HMP quit; nenhuma instância ativa ao final |

### Matriz SMP

| SMP | discovered | online | failed | slots | IDs/APIC | CPU_INIT_OK | TIMER_READY | SHELL_READY |
|---:|---:|---:|---:|---|---|---:|---:|---:|
| 1 | 1 | 1 | 0 | 0 | idle 1 / apic 0 | 1 | 1 | 1 |
| 2 | 2 | 2 | 0 | 0..1 | idle 1..2 | 2 | 2 | 1 |
| 4 | 4 | 4 | 0 | 0..3 | idle 1..4 | 4 | 4 | 1 |
| 8 | 8 | 8 | 0 | 0..7 | idle 1..8 | 8 | 8 | 1 |

Logs: `artifacts/build/cl03-smp{1,2,4,8}-serial.log`. O serial legado pode intercalar caracteres de eventos concorrentes durante o churn intenso; sentinelas de boot e invariantes foram coletadas antes do churn e permaneceram íntegras.

## Notas não bloqueadoras

Persistem warnings legados aceitos: símbolos não usados, XHCI legado, GNU-stack ausente em assembly e segmento RWX do linker. O trace de guest_errors mantém MMIO legado já documentado. Nenhum warning novo relevante da CL-03 foi observado.

## Limites de escopo

Não foram implementados `on_cpu`, handoff pós-switch, correção da janela de requeue, atomicidade de wait queues, timer handles, accounting em ns, novo contrato de kill/cleanup, reaper novo, input/modal/UI ou paginação. CL-04+ não foi iniciada.

## Status final

`APPROVED WITH NON-BLOCKING NOTES`


## Correção pós-homologação

A primeira entrega possuía truncamento potencial em logs seriais e redução do ID no formatter manual do reaper. A correção está documentada em `TMV1-CL-03-FIX-task-id-diagnostics.md`.
