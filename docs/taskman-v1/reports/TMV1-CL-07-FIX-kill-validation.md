# TMV1-CL-07-FIX — kill lifecycle validation

Data: 2026-07-31
Branch: `feat/taskman`
Base: `f705d2074c39d6bc78d742847e96428273261dc6`

## Correções

Kill durante `exit_started` retorna `TASK_KILL_ALREADY_EXITING` antes de killability/pending, sem alterar pending, timestamp ou reason. CLI, logs e stats expõem o resultado. Cancellation points ficam desabilitados após o claim do exit.

`thread_exit_with_reason` valida o motivo, reclama o lifecycle sob o scheduler lock, limpa `cleanup_fn/cleanup_ctx`, executa cleanup fora do lock uma vez e valida a conclusão antes de ZOMBIE. Stats distinguem fases, callbacks e motivos.

O harness usa contextos independentes. READY usa hold test-only da criação; RUNNING/on-CPU, waits e EXITING são observados e classificados sob o mesmo lock pelo helper comum. O reaper hold guarda somente ID. Em UP, o worker publica atomicamente sua observação RUNNING/on-CPU pelo mesmo helper locked. SLEEPING/BLOCKED exigem wait kind/active e queue membership exatos.

## Resultados executados

- `make kernel-check JOBS=2`: PASS durante desenvolvimento.
- SMP=1 TCG, SMP=2 e SMP=8, `killtest all`: PASS; zero panic/#PF/#GP/FATAL nos logs finais.
- SMP=4: core suite e checks scheduler/sync/account: PASS.
- READY, RUNNING, SLEEPING, BLOCKED, duplicate, protected/Idle, non-killable, rename, NORMAL, ZOMBIE, EXITING normal/killed e parser puro: PASS.
- `killtest check`: `pending_eternal=0 cleanup_missing=0 wrong_reason=0 holds=0`.

Logs: `artifacts/build/cl07fix-smp1-serial.log`, `cl07fix-smp2-serial.log`, `cl07fix-smp4-serial.log`, `cl07fix-smp8-serial.log`.

## Gates não concluídos

Não foram concluídos: timeout-race 500/2000/5000/10000, negative build, sweep real de smpstress, parser CLI completo via HMP, telemetria ps/TASKMAN, soak SMP=8 de 120 segundos e nova matriz completa de regressão. Nenhum PASS foi inventado.

## Status

`REJECTED`: implementação e suítes core aprovadas; certificação runtime integral pendente. TMV1-CL-08 não foi iniciada.

## Build final

- kernel j2/jN/cmp: PASS, `15e6f4fb21bc9ba3dac1dec3873c3d6dd16dc791642184656e2d1ab4794a4677`.
- `deps-check` e `image`: PASS.
- `hobbyos.img`: `449fc90ce3add6506ce4efa62516d141ede0a59e2e69de1b91783cf655db8546`.
- `nm -u kernel.elf`: sem símbolos indefinidos.

## Certificação final posterior

A segunda entrega permaneceu rejeitada por ausência da matriz integral. A certificação posterior está em `TMV1-CL-07-FIX2-final-certification.md`.
