# TMV1-CL-07-FIX4 — segurança de stack e handoff

## Base, fault e causa

Base: `108164f08cc04f94a7f4c4a8d7c69f9097e4082c` (`feat/taskman`). A sequência combinada CLI/UI da FIX3 expôs `SCHED: invalid switch finish state`. A medição com `-fstack-usage` confirmou frames de 12400 bytes em `test_protected`, 8320 em `lifecycle`, 6832 em `test_smpstress_sweep`, 6640/6288 no sampler, 6240 em `snapshot_id`/`scan_pending_all` e 6208 em `ui_setup`. A cadeia `ui_setup -> wait_state -> snapshot_id` consumia cerca de 12,4 KiB antes dos frames da shell e IRQs. Esse risco foi eliminado, mas os guards não confirmaram que ele era a causa única do fault.

## Orçamento e refactors

A stack comum permanece em 16 KiB, com guard inicial de 512 bytes preenchido com `0xA5`, canário `0xA5A5A5A5A5A5A5A5` e frame estático máximo de 2048 bytes. Idle não recebe guard inexistente. O killtest passou a usar workspace persistente e lookup copy-only `scheduler_snapshot_task_by_id`; o sampler usa dois bancos pertencentes ao objeto; lifecycle e selftest usam workspaces persistentes; os selftests de identidade deixaram de manter duas `task_t` automáticas.

`make stack-check` compila com `-fstack-usage`, rejeita frames acima de 2048 e dynamic/unbounded, e grava os 20 maiores em `artifacts/build/stack-usage-report.txt`. O gate passou com máximo de 1920 bytes (`lapic_test`); `xhci_probe_ports`, maior frame de produção remanescente, usa 1632 bytes. Os antigos ofensores estão abaixo do limite: `task_identity_selftests` 1344, `ui_status` 1184, sampler selftest 464 e smpstress sweep 448; os demais caminhos críticos ficaram fora do top 20.

## Guards, low-watermark e diagnóstico

Criação preenche a stack completa, reserva a guard antes do frame inicial e valida RSP/guard no início e fim do switch e ao escolher a próxima task. `schedtest stack` reporta `used`, `free` e `guard=OK`. Corrupção registra ID, RSP, limites e offset antes do panic. O finish captura uma máscara de condições incluindo handoff, prev/next/current, CPU slot, fila, estado e guards, libera o lock e emite `[SCHED][FINISH_FAULT]` antes do panic.

Após cleanup, `thread_exit_with_reason` resolve novamente o CPU slot e valida current/on_cpu/current_cpu_slot/cpu.current antes de accounting e conclusão, eliminando o uso do slot pré-cleanup após uma possível migração.

## Negativos e reprodutor

O negativo sintético de finish identificou o bit `NEXT_READY` em `artifacts/build/cl07fix4-negative-finish-diagnostic.log`. O negativo controlado alterou um byte somente na guard de uma task off-CPU e produziu `STACK_GUARD_CORRUPTION_DETECTED`, preservado em `artifacts/build/cl07fix4-negative-stack-guard.log`. O rebuild normal posterior deixou ambos os hooks inativos.

Em QEMU SMP=4, `killtest ui-cli-repro 100` passou uma janela de 100 iterações, com setup/status dos seis estados, parser CLI, cleanup e checks, sem contextos ou holds residuais. Entretanto, uma segunda janela no mesmo QEMU TCG falhou durante `ui_setup`: `[SCHED][FINISH_FAULT] ... failed=0x47`, isto é, handoff ativo, prev, next e current ausentes no finish. Nenhum guard havia falhado. Os logs estão em `artifacts/build/cl07fix4-smp4-pending-overwrite-fault.log` e `artifacts/build/cl07fix4-smp4-repeat-finish-fault.log`.

## Certificação

`make stack-check` passou, os builds j2/jN foram idênticos (`865812816b390a5b09ffa9ca8e06cf9654f4186e8c9ba9772436943fc8a0a344`) e o soak SMP=8 anterior à reprodução final teve 308852 ms, dez ciclos core, 10000 rounds, dez sweeps e checks verdes. Contudo, a recorrência do finish fault em SMP=4 TCG invalida a certificação global. Status: **REJECTED**. Nenhum commit FIX4 foi criado. A FIX4 não inicia CL-08.

## Causa raiz posterior

Os guards descartaram overflow como causa unica. O `failed_mask=0x47` e os
panics de pending overwrite revelaram captura de CPU slot antes de desabilitar
preempcao. A correcao/certificacao final esta em
`TMV1-CL-07-FIX5-migration-safe-handoff.md`.
