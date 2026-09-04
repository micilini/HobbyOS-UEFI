# TMV1-CL-02 — Bootstrap global/per-CPU do scheduler

## Identificação

- Data: 30/07/2026
- Branch: `feat/taskman`
- Commit-base/HEAD inicial: `9636c900f83d01da9fba69c870b903d3f58b940b`
- Worktree inicial: somente `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` não rastreado e preexistente; preservado fora do commit
- Escopo: bootstrap global/per-CPU, slots lógicos, readiness/start, ordem de boot e logs; nenhuma fase CL-03+

## Diagnóstico confirmado

O fluxo antigo iniciava APs antes de `scheduler_init()`, embora `ap_kernel_entry()` já usasse lock, registry e filas globais. Scheduler e reschedule indexavam arrays diretamente por `lapic_get_id()`, com limites conflitantes de 256 e 32 CPUs. O BSP era marcado ONLINE durante discovery e `g_system_ready_for_scheduling` era escrito externamente por `kernel_init.c`. Isso permitia lock/lista não inicializados, acesso fora de array, ONLINE sem timer/scheduler e scheduling antes de readiness validada.

## Arquitetura implementada

- `HOBBYOS_MAX_CPUS=32` é a única capacidade pública, em `smp/cpu_limits.h`.
- `cpu_slot_t` denso separa identidade lógica de `apic_id`; lookup desconhecido falha sem fallback.
- Discovery valida contagem, BSP único, APICs únicos, slots densos, roundtrip e lookup negativo. O BSP permanece PREPARE.
- Scheduler possui estados `UNINITIALIZED`, `GLOBAL_READY`, `STARTED` e `scheduler_cpu_state_t` por slot com `current`, `idle`, `initialized`, `timer_ready` e `online`.
- `scheduler_global_init()`, init BSP/AP, timer-ready e `scheduler_start()` têm retornos explícitos e guards de ordem/duplicidade/slot/APIC.
- O boot agora executa global init e BSP init antes dos APs; timer-ready precede ONLINE em BSP/AP; serviços precedem START; interrupções são habilitadas após START.
- `get_current_task`, idle/current, accounting e `g_need_resched` usam exclusivamente slot. `last_cpu_id` mantém nome legado, mas contém slot lógico nesta fase.
- Selftest de bootstrap prova init per-CPU antes do global, global duplicado, slots `HOBBYOS_MAX_CPUS`/INVALID e init BSP duplicada sem mutação.
- Logs estruturados registram GLOBAL_INIT, CPU_INIT, TIMER_READY, ONLINE e START com slot/APIC separados.

## Arquivos alterados

- `kernel/src/smp/cpu_limits.h`: limite canônico.
- `kernel/src/smp/smp_topology.{h,c}`: slots, mapping, validação, estados e contadores.
- `kernel/src/core/scheduler.{h,c}`: contrato global/per-CPU/start, guards, selftest e migração dos maps.
- `kernel/src/core/interrupts.c`: reschedule por slot e readiness pública.
- `kernel/src/smp/smp_boot.c`: resolução APIC→slot e ordem AP scheduler→timer→ONLINE.
- `kernel/src/core/kernel_init.c`: nova sequência BSP/global/AP/serviços/start.
- Este relatório.

## Toolchain e ambiente

- GCC 13.3.0; GNU ld 2.42; GNU Make 4.3
- QEMU 8.2.2; Python 3.12.3
- OVMF combinado: `/usr/share/ovmf/OVMF.fd`
- KVM: disponível; `ACCEL=auto` selecionou KVM

## Testes

| ID | Comando/cenário | Resultado | Evidência |
|---|---|---|---|
| Entrada | `make kernel-check JOBS=2`; `JOBS=12`; `make deps-check` | PASS | baseline `765ba2db1ae1dd5c722b7716541b00fec8e74f0b2de0a561408f974fa05619b2` |
| Build | `make kernel-check JOBS=2` | PASS | `artifacts/build/kernel-check-j2.log` |
| Build paralelo | `make kernel-check JOBS=12` | PASS | `artifacts/build/kernel-check-j12.log` |
| Reprodutibilidade | `cmp` e `sha256sum` | PASS | ambos `fe27920d09fa4a0762e6acda073aeab18b3eff533d9bd153cca626f1f2416c70` |
| Imagem | `make image` | PASS | `hobbyos.img` `375d1c547c0506d01850892d7a989c677d8d4c25d7b8222f83461d3eab895aea` |
| Guards negativos | selftest no boot, antes/depois do global/BSP init | PASS | duas sentinelas `[SCHED][SELFTEST] BOOT_GUARDS_OK` |
| SMP=1 | TCG; START + shell | PASS | `artifacts/build/cl02-smp1-*.log` |
| SMP=2 | auto→KVM; START + shell | PASS | `artifacts/build/cl02-smp2-*.log` |
| SMP=4 | auto→KVM; START + shell | PASS | `artifacts/build/cl02-smp4-*.log` |
| SMP=8 | auto→KVM; START + shell | PASS | `artifacts/build/cl02-smp8-*.log` |
| TASKMAN | SMP=4, `taskman`, ESC, `help` | PASS | `session_begin OK` e `session_end OK`; help injetado depois |
| Stop | HMP quit após cada cenário | PASS | pid/socket removidos; nenhum QEMU ativo |
| Falhas | grep em serial/debugcon/trace | PASS | zero PANIC/#PF/#GP/FATAL/UNKNOWN_APIC/GLOBAL_NOT_READY |
| Regressão | buscas por maps/readiness/MAX_CPUS inseguros | PASS | nenhum match |

## Contagem por cenário

| SMP | discovered | online | failed | slots | apic_ids | CPU_INIT_OK | TIMER_READY | SHELL_READY |
|---:|---:|---:|---:|---|---|---:|---:|---:|
| 1 | 1 | 1 | 0 | `0` | `0` | 1 | 1 | 1 |
| 2 | 2 | 2 | 0 | `0,1` | `0,1` | 2 | 2 | 1 |
| 4 | 4 | 4 | 0 | `0,1,2,3` | `0,1,2,3` | 4 | 4 | 1 |
| 8 | 8 | 8 | 0 | `0..7` | `0..7` | 8 | 8 | 1 |

## Notas

- O parser MADT atual fornece APIC IDs de 8 bits, não ACPI IDs; `acpi_id` fica explicitamente desconhecido (`UINT32_MAX`).
- Permanecem warnings legados XHCI/unused, `.note.GNU-stack`, LOAD RWX e `guest_errors` MMIO já aceitos na CL-01.
- O valor cru do LAPIC timer foi preservado; calibração pertence à CL-06.

## Status final

APPROVED WITH NON-BLOCKING NOTES
