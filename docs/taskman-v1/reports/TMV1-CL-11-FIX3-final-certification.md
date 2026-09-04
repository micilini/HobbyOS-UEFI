# TMV1-CL-11-FIX3 — certificação final

Status: **REJECTED**. Nenhum commit foi criado.

## Base e transporte

- Branch `feat/taskman`; base `88645901d9393f4796af84cecd9e13d3d6671c70`.
- O worktree CL-11/FIX/FIX2 e o roadmap não rastreado foram preservados.
- O transporte antigo usava delay de 0,08 s e omitia o hold de `sendkey`;
  o default QEMU de 100 ms permitia sobreposição física de teclas.
- O transporte novo sempre envia hold explícito de 30 ms. Perfis: normal
  0,10 s, stress 0,20 s e sync 0,25 s. Selftests de keymap/timing passaram.
- `hmp_text`/`hmp_key` centralizam a injeção. Timeout é classificado por marker
  idempotente como `TRANSPORT_LOSS`, `SHELL_INPUT_STALL` ou `GUEST_FAILURE`, sem
  repetir comandos não idempotentes.
- A barreira pós-stress executa shell sync, probe e invariantes de input.

Resultado dedicado SMP=8/TCG:

```text
[HMP][TRANSPORT] PASS during_stress=25 after_stress=25 lost=0 duplicates=0
```

Artefato: `artifacts/build/cl11fix3-hmp-transport-smp8.log`.

## Formatação, região e heap

- Summary/footer usam packers bounded de duas linhas; token que não cabe falha.
- `line_builder_t` registra truncamento; stats cobrem formatters e builders.
- A região anterior é verificada vazia após clear e antes do frame seguinte.
- `taskmantest formatting-max` passou em 90 e 160 colunas com valores máximos.
- `heap-begin`/`heap-end` medem `HeapStats.used_bytes` e blocos usados após
  reaper idle; textos hardcoded de drift foram removidos.
- O soak mede tempo modal ativo, frames esperados/reais e delivery ratio.
- Stack-check passou: maior frame 1984 bytes, limite 2048.
- Smoke CL-11 SMP=1 passou com truncamentos, clipping, stale cells e clear
  failures zerados.

## Falha bloqueante

A primeira tentativa integral revelou um argumento legado de harness sendo
interpretado como profile. O call site foi corrigido e `zombie-mem` isolado
passou com render TASKMAN e cleanup.

Na repetição limpa, CL-09 avançou pela matriz/soak CL-07 e entrou na CL-08.
O gate SMP=8 falhou dentro do guest:

```text
[ACCOUNT][CLOCK_SMP] FAIL reads=1000000 migrations=43458
api_regressions=0 local_regressions=0 cross_lag=0
reaped=0 free_inflight=0 zombies=18
```

As tasks foram liberadas logo depois e o marker sync chegou; não foi perda HMP
nem guest morto. É falha de quiescência CL-08/accounting envolvendo reaper. A
missão proíbe alterar scheduler/reaper/accounting sem novo fault objetivo, então
nenhuma alteração desse tipo foi feita.

CL-09/CL-10 integrais, matriz CL-11 final, negatives finais, soak CL-11 de 180 s,
hashes j2/jN, imagem/hash final e commit permanecem não aprovados. QEMU foi
encerrado e TMV1-CL-12 não foi iniciada.
## Correção posterior da quiescência

A execução FIX3 comprovou o transporte HMP, mas o `clock-smp` falhou esperando
passivamente um reaper NORMAL a partir da shell INTERACTIVE com polling de 1 ms.
Os 32 workers foram liberados imediatamente depois do retorno do comando. A
correção e certificação final estão em
`TMV1-CL-11-FIX4-final-certification.md`.
