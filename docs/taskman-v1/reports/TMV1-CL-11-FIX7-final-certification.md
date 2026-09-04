# TMV1-CL-11-FIX7 — certificação do cancelamento pré-bloqueio

## Correção e certificação posterior

A FIX7 implementou a serialização central de kill e prepare interruptível, mas a
certificação permaneceu incompleta. A auditoria encontrou que o loop modal ainda
aceitava workers `wait_gate_entry` com exit NORMAL e não cobria o caso em que
completion já estava sinalizada quando o caller cancelado entrou no wait.

A certificação está em `TMV1-CL-11-FIX8-final-certification.md`.

## Base e diagnóstico

- Branch: `feat/taskman`.
- Base: `88645901d9393f4796af84cecd9e13d3d6671c70`.
- O log FIX6 registrou caller `10476`, owner `10477`, sessão ACTIVE e kill do caller
  aceito, sem o segundo kill.
- A sessão podia ficar ACTIVE antes de o caller preparar o wait de completion.
- `scheduler_request_kill_locked` marcava `kill_pending` sem wake quando ainda não
  existia wait. A API de prepare posterior não consultava esse estado.

## Implementação

Foi introduzido `scheduler_prepare_block_interruptible`, com resultado explícito
PREPARED/CANCELLED/ERROR. A verificação de cancelamento e a publicação do wait são
serializadas por `g_scheduler_lock`. CANCELLED libera o scheduler lock sem alterar
generation, estado, fila ou metadados do wait.

Semáforo, timer sleep e input queue foram migrados para a nova API. O timer libera o
node ainda privado sem armá-lo ou adquirir task ref; a input queue contabiliza o wait
cancelado sem waiter residual. `scheduler_wait_stats_t` mede prepares, commits,
aborts, preblock cancellations e erros.

## Evidência executada

- `make kernel-check JOBS=2`: PASS.
- Reprodutor `modaltest kill-caller`, SMP=8: 50/50 PASS.
- `synctest preblock-sem`, SMP=8: PASS, sem waiter residual.
- `synctest preblock-timer`, SMP=8: PASS, zero arm e zero ref.
- `modaltest kill-caller`, SMP=8, junto aos canários: PASS.
- Artefatos: `artifacts/build/cl11fix7-kill-caller-repro-50.log` e
  `artifacts/build/cl11fix7-preblock-focused.log`.

## Status

REJECTED nesta execução: permanecem pendentes o canário de input, o hook modal
prewait, os loops 3×1000, o negative causal, as regressões integrais, a matriz CL-11,
o soak de 180 segundos e os builds/hashes finais. Nenhum commit foi criado.
