# TMV1-CL-03-FIX — IDs completos em diagnósticos e reaper

## Identificação

- Data: 2026-07-30
- Branch: `feat/taskman`
- Base: `66e577d91680572c70ff36c741df4542a2eeb322`
- Worktree inicial: somente `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` não rastreado, preservado
- Escopo: correção pós-homologação da CL-03; CL-04 não iniciada

## Diagnóstico

A primeira entrega tinha dois defeitos em `scheduler.c`: `sched_serial_dec` usava 16 posições para um decimal `uint64_t` de até 20 dígitos; o reaper mantinha uma segunda conversão que reduzia `task_id_t` para `int` e limitava a saída a dez dígitos. IDs pequenos ocultaram os defeitos nos boots anteriores.

## Implementação

Foi criado `sched_u64_format_decimal`, formatter puro sem alocação que:

- cobre zero até `UINT64_MAX`;
- usa limite explícito de 20 dígitos e buffer de 21 bytes;
- termina a string em sucesso;
- retorna falha e string vazia quando o buffer é insuficiente;
- rejeita ponteiro nulo e tamanho zero sem dereference.

`sched_serial_dec` agora formata em buffer de 21 bytes e escreve a string completa. Os caminhos `[TASK][CREATE]`, `[TASK][KILL]`, `[TASK][IDENTITY]` e `[REAPER]` compartilham esse wrapper. O formatter manual do reaper foi removido integralmente.

O selftest cobre 0, 1, 9, 10, 9999999999, 10000000000, 9999999999999999, 10000000000000000 e `UINT64_MAX`, além de buffer insuficiente, ponteiro nulo e tamanho zero. Falha de formatação reprova `scheduler_validate_task_identity`.

## Arquivos alterados

- `kernel/src/core/scheduler.c`: formatter, wrapper, selftest e reaper.
- `docs/taskman-v1/reports/TMV1-CL-03-task-identity-flags.md`: referência pós-homologação.
- este relatório.

## Build e imagem

| Teste | Resultado | Evidência |
|---|---|---|
| `make kernel-check JOBS=2` | PASS | `artifacts/build/kernel-check-j2.log` |
| `make kernel-check JOBS=12` | PASS | `artifacts/build/kernel-check-j12.log` |
| SHA-256 j2 | PASS | `717561b6b8db5aa24e55e19384bde8588c4410dacd2a7ffb4863dbaf39d91713` |
| SHA-256 j12 | PASS | mesmo hash |
| `cmp -s` | PASS | retorno 0 |
| `make deps-check` | PASS | GNU-EFI, QEMU, OVMF e KVM disponíveis |
| `make image` | PASS | `0333c4d64f145c0e5f12dcaaf80129780e05b29dd53547cd8e3d953171a468c4` |

Warnings observados são os legados aceitos: XHCI/unused, GNU-stack ausente e segmento RWX.

## QEMU

| SMP | Aceleração | Resultado | Evidência |
|---:|---|---|---|
| 1 | TCG | PASS | shell; ID_FORMAT/ID_PARSE/identity e guards OK; 1 Idle |
| 4 | auto → KVM | PASS | shell; todos selftests OK; 4 Idles; TASKMAN e reaper OK |
| 8 | auto → KVM | PASS | shell; todos selftests OK; 8 Idles |

Logs: `artifacts/build/cl03fix-smp1-serial.log`, `cl03fix-smp4-serial.log`, `cl03fix-smp8-serial.log`. Nenhum PANIC, #PF, #GP ou FATAL foi encontrado.

### TASKMAN

Em SMP=4, `taskman` produziu `session_begin OK`, ESC produziu `session_end OK` e `help` foi enviado depois, sem alteração visual.

### Reaper

Em SMP=4 foram criados dois workers `smpstress`, enviados kills cooperativos para IDs 10 e 11 e observados:

- `[REAPER] Freed zombie PID=10`
- `[REAPER] Freed zombie PID=11`

## Teste negativo

A capacidade do wrapper foi temporariamente reduzida para 16 bytes. O boot SMP=1 emitiu `[TASK][SELFTEST] ID_FORMAT_FAIL` e a validação recusou a configuração. A mutação foi restaurada para 21 bytes; os três boots finais emitiram `ID_FORMAT_OK`. Evidência: `artifacts/build/cl03fix-negative-format-serial.log`.

## Buscas estáticas

- `int v = t->id`: zero ocorrências.
- casts de `.id` para `int`/`uint32_t`: nenhuma ocorrência relacionada a task ID. Os matches restantes são casts de índices de arrays em PS/TASKMAN e APIC ID em panic.
- buffers literais de 10–20 bytes em `scheduler.c`: zero ocorrências usadas para task ID.
- reaper: somente a mensagem e chamada subsequente a `sched_serial_dec(t->id)`; não há formatter manual.

## Escopo e notas

`task_id_t` permanece `uint64_t`; IDs, flags, memberships, snapshot e parser da CL-03 permanecem inalterados. Não houve mudança em context switch, scheduler policy, wait queues, semáforos, timers, accounting, kill CL-07, input ou UI. CL-04+ não foi implementada.

## Status final

`APPROVED WITH NON-BLOCKING NOTES`
