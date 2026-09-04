# TMV1-CL-11-FIX18 — fechamento final da CL-11

Data: 2026-08-16
Branch: `feat/taskman`
Base: `88645901d9393f4796af84cecd9e13d3d6671c70`
Status: **APPROVED**

## 1. Falha FIX17

O guest aprovou `reaptest check` e o soak, mas `verify_soak()` exigia
literalmente `refs=0`. O log continha `refs=1`, `inflight=0`, `backlog=0`,
`structural=0` e `violations=0`; por isso a única falha foi o gate host-side.

## 2. `refs_current` não é leak

`scheduler_reaper_stats_snapshot()` soma `task_wake_timer_refs` de todas as
tasks vivas no registry. O valor representa referências de timer atualmente
possuídas, não referências perdidas nem ZOMBIEs pendentes de reap.

O check do guest valida a equação:

```text
refs_acquired == refs_released + refs_current
```

Ele também reprova underflow, release failure, duplicate release,
`free_inflight`, falha estrutural e callback/publish residual. Assim,
`PASS refs=1` prova uma referência ativa e contabilizada.

## 3. Origem da referência ativa

A task permanente `reaper` dorme com
`timer_sleep(TASK_REAPER_PERIOD_MS)`, cujo período é 2000 ms. O sleep mantém uma
task timer reference até dispatch ou cancelamento; ao acordar, a task faz o scan
e arma o próximo sleep. O estado estável esperado é, portanto, `refs_current=1`.

## 4. Baseline SMP

As últimas linhas `REAPTEST CHECK` dos quatro positivos produziram:

| SMP | Aceleração | refs | inflight | backlog | structural | violations |
|---:|:---:|---:|---:|---:|---:|---:|
| 1 | TCG | 1 | 0 | 0 | 0 | 0 |
| 2 | KVM | 1 | 0 | 0 | 0 | 0 |
| 4 | TCG | 1 | 0 | 0 | 0 | 0 |
| 8 | TCG | 1 | 0 | 0 | 0 | 0 |

O baseline derivado e exigido é `REAP_REF_BASELINE=1`. O soak também terminou
com `refs=1` e com todos os campos estruturais em zero.

## 5. Verificadores corrigidos

`scripts/test-cl11-fix16-final.sh` e `scripts/test-cl11-final-resume.sh` agora
usam `parse_reap_check()` e comparam o valor com o baseline estável derivado dos
quatro SMPs. O valor positivo de `refs` não é mais inferido como leak.

## 6. Revalidação offline

`scripts/test-cl11-fix18-resume-after-soak.sh` não iniciou QEMU. Ele confirmou
os estágios FIX16 até negatives, a única rejeição antiga no estágio soak, a
arquitetura estática do reaper e todos os artefatos runtime existentes.

O probe foi revalidado com 5 sessões, 50 full frames, fallback e scroll zero,
duas páginas, 142 capturas, gap máximo de 1094 ms e heap drift zero. Os quatro
positivos SMP e os quatro negatives causais, seguidos do reset normal, passaram.

## 7. Soak revalidado

O soak existente passou com:

```text
duration_ms=3367550
sessions=100
full_frame_target=1500
full_frames=1500
fallback_frames=0
auto_exit_sessions=100
auto_exit_shortfalls=0
max_frame_gap_ms=1748
modal_scroll_delta=0
```

Também passaram navigation/churn 10000, zombies/kill-render/layouts 100,
snapshot-boundary 20, timer-ref 100, os três preblocks 100, os três modos modais
100 e três lotes clock-smp/reaper-quiescence. Todos os checks internos e a busca
por faults passaram.

## 8. Continuidade de fontes

Os três manifests do core protegido são byte a byte idênticos, com digest do
manifest `b3d6e178945d758b9ec094dbb664bafc554868f9fd71d66c2fada07f0f76af93`.
Os manifests do runtime TASKMAN autorizado antes/depois do build também são
idênticos, com digest
`f0fe8b01d04bd0e9d395db4d16f0dddad0e24c4c6906e39fabb589fe58e3d31a`.

Nenhum arquivo runtime foi alterado pela FIX18 e nenhum QEMU foi executado.

## 9. Build final

`scripts/test-cl11.sh final-build` passou:

- stack-check PASS, máximo 1984/2048;
- kernel-check j2 e jN PASS;
- binários j2/jN idênticos;
- deps-check e image PASS;
- `nm -u kernel.elf` vazio;
- zero warning em `cmd_taskman.c` e `cmd_taskmantest.c`.

Hashes finais:

```text
15464229438b91d669e069230bec88ebcf3a27ca7243df0812f1051404d7030d  kernel.elf
da553b861c31087afc6819673e63487267e1425cec6c7790ed795618419eb807  hobbyos.img
```

## 10. Git e status

O fechamento será registrado no único commit após a base com a mensagem
`feat(taskman): finish the complete paginated TASKMAN V1 interface`. Não houve
push. `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` permanece fora do stage e a
TMV1-CL-12 não foi iniciada.

Status final: **APPROVED**.
