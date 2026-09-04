# TMV1-CL-11-FIX6 — timer reference determinístico

## Base e diagnóstico

- Branch: `feat/taskman`.
- Base: `88645901d9393f4796af84cecd9e13d3d6671c70`.
- Worktree: CL-11/FIX1–FIX5 preservado, sem reset.
- A publicação ocorre sob scheduler lock; depois do unlock outro CPU pode
  executar a task enquanto `thread_create` ainda escreve o log e retorna.
- O worker antigo armava `timer_sleep_interruptible(20)` antes de o controller
  instalar o hold. A assinatura `claimed=0`, exit NORMAL indicava dispatch
  anterior ao hook, não underflow da referência.
- O par test-only `active/task_id` também era atualizado por stores separadas.

## Implementação

O contexto persistente possui canários e startup semaphore. A criação usa
`task_create_options_t.test_reap_hold`, eliminando o estado next-creation. O
controller observa BLOCKED/WAIT_SEMAPHORE, instala o hold exato sob
`g_timer_lock` e somente então libera o gate. O teste comprova, nesta ordem:
SLEEPING/ref=1, node CLAIMED com lifecycle/wait generation exatos, kill aplicado
atomicamente no wait, resultado CANCELLED, ZOMBIE KILLED quiescente,
`TIMER_REFS` como blocker isolado, defer real, release da task exatamente uma
vez, wake stale, dispatch e reap final. O cleanup desarma hook/hold e elimina o
handle antes de reutilizar o workspace.

O harness agora distingue explicit guest FAIL de fault/exit e de timeout sem
sentinela. Somente o timeout sem FAIL executa transport probe. O incidente FIX5
foi `GUEST_TEST_FAILURE caused by test race`, não transport loss.

## Evidências executadas

- `reaptest timer-ref`: PASS com gate/sleeping/claimed/KILLED/ref/defer/release/stale/reap.
- `reaptest timer-ref-loop 100`: PASS SMP=8.
- 3×1000 SMP=1 e 3×1000 SMP=8: PASS, NORMAL=0, residual=0.
- Negative `HOBBYOS_REAPTEST_NEGATIVE_LATE_TIMER_HOLD`: detectou exit NORMAL
  depois de TIMEOUT e ausência de node CLAIMED.
- Cinco heavy runs SMP=8: `[CL08][TIMER_REF_DETERMINISTIC] PASS runs=5`.
- CL-07 integral: PASS SMP=1/2/4/8.
- CL-08 integral: PASS, incluindo negatives e soak.
- CL-09 integral: `[CL09][CERTIFICATION] PASS`.

Os resultados CL-10, matriz CL-11, soak final, builds reproduzíveis e hashes
são registrados abaixo ao concluir a execução final.

## Status final

Em certificação. Nenhum commit ou push foi criado antes dos gates finais.
# Correção posterior do cancelamento pré-bloqueio

A execução FIX6 certificou timer references, mas a regressão CL-10 encontrou uma
janela na qual o caller recebia kill depois de a sessão ficar ACTIVE e antes de
entrar em `sem_wait_interruptible`. O kill era aceito sem wait para acordar; em
seguida, `scheduler_prepare_block` ignorava o kill pendente e bloqueava a task.
A correção e certificação estão em
`TMV1-CL-11-FIX7-final-certification.md`.
