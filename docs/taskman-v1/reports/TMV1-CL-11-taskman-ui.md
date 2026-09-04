# TMV1-CL-11 — TASKMAN V1 paginado e escalável

O endurecimento FIX3 e o resultado bloqueante da certificação estão em
`TMV1-CL-11-FIX3-final-certification.md`.

Data: 2026-08-02
Branch: `feat/taskman`
Base: `88645901d9393f4796af84cecd9e13d3d6671c70`

## Diagnóstico e arquitetura

A UI anterior usava snapshot máximo 128, painel 24x110, refresh 250 ms,
newlines que podiam causar scroll e qualquer special/Q/Enter como saída. O
worker modal independente e seu contrato de ownership foram preservados.

O console agora oferece clear/write-line bounds-safe sob seu lock. As operações
atualizam history e framebuffer, preservam cursor, clipam largura, completam a
linha com espaços e nunca chamam scroll. Estatísticas separam writes, clears,
clips, regiões rejeitadas e scroll count.

## Layout, modelo e paginação

`taskman_layout_compute` deriva a região de cols/rows e cursor. Wide exige 118
colunas, compact 76; narrow/short permanecem responsivos a ESC. A quantidade de
linhas visíveis é geometria disponível menos title/summary/header/footer.

O modelo começa em 128, cresce em potências de dois até 4096, reutiliza arrays
heap de snapshots/samples, repete captura quando truncada e nunca guarda
`task_t *`. Falha de alocação e cap test-only produzem truncamento explícito. O
heapsort iterativo ordena exclusivamente por PID. Summary cobre todo o modelo.

Page count, showing range e seleção são reconciliados após cada captura.
Up/Down, PageUp/PageDown e Home/End navegam; Q, Enter, Left, Right e specials
desconhecidas são ignorados. Somente ESC encerra.

## Contrato visual

O título é `HobbyOS TASKMAN`. Wide mostra PID, USER=Root, CPU, LCPU, AFF=Any,
STATE, CLASS, Q, TIME+, %CPU, KILL, MEM~ e NAME. Compact preserva USER e AFF.
Time+ usa o formatter compartilhado, %CPU usa sampler por ID, kill usa o
formatter do scheduler e MEM~ usa B/KiB/MiB/GiB. Nomes são bounded, sanitizados
e truncados sem wrap.

## Evidência inicial

Selftests passaram para 0/1/20/50/128/129/257, sort
`9,1,UINT64_MAX,5,2`, geometrias wide/compact/narrow/short, refresh, ESC-only,
formatters, 10.000 churns e ZOMBIE. Em SMP=4 TCG, 129 workers reais produziram
138 tasks capturadas, duas páginas e zero truncamento. Navegação HMP não saiu
com Q/Enter/Left/Right, saiu com ESC, teve scroll delta zero e cleanup removeu
129 workers com heap drift zero.

## Certificação final

Os quatro negatives causais passaram em QEMU isolado. A matriz positiva passou
em SMP=1/2/4/8 (TCG e KVM quando disponível), incluindo 129 tasks reais,
paginação HMP, cap 64 com truncamento explícito, falha da primeira alocação,
kill-render, zero scroll e heap drift zero. Durante esse gate foi encontrada e
corrigida uma amostra do sampler com `sample_time_ns` sem avanço: a frame agora
permanece sem percentual em vez de classificar a ausência de intervalo como
regressão. A repetição terminou com zero anomalias de clock/métricas.

A matriz CL-07 passou. CL-08 `all` passou. CL-09 `all` falhou uma vez e passou
integralmente na repetição. Entretanto, `scripts/test-cl10.sh all` voltou a
falhar no mesmo gate herdado, em boot limpo SMP=8 TCG:

```text
command: reaptest grace
expected: [REAPTEST][GRACE] PASS
[REAPTEST][FIXTURE] FAIL stage=validate id=15 started=1 finished=1
state=4 class=1 queue=0 on_cpu=0 wait_active=0 wait_kind=0
schedule_count=2 hold_count=0 mask=0x0000000000000002
```

A primeira fixture do mesmo boot passou; a segunda foi liberada antes da
validação do hold. A CL-11 não altera scheduler/reaper, mas o gate integral é
obrigatório e falhou de modo reproduzido em duas execuções agregadas. Portanto,
o status final é **REJECTED**, o soak CL-11 e o commit não foram executados.
O worktree foi preservado e CL-12 não foi iniciada.

## Fechamento definitivo FIX18

O fechamento definitivo da CL-11 está documentado em
`TMV1-CL-11-FIX18-final-closure.md`. A FIX18 corrigiu exclusivamente o
verificador host-side que tratava `refs_current=1` como leak, derivou o baseline
estável dos quatro logs SMP, revalidou offline o soak guest-side já aprovado e
concluiu o build reproduzível j2/jN. Nenhum QEMU foi iniciado nessa retomada.
