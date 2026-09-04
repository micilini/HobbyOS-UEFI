# TMV1-CL-06 — Accounting temporal e `%CPU` real

Data: 2026-07-31
Branch: `feat/taskman`
Base: `4d6184471daac67beebec7614af96acd120aa96e`

## Diagnóstico e arquitetura

O modelo anterior somava uma unidade por entrada no IRQ de timer e dividia essa
contagem por milissegundos. Além de não medir tempo, HPET e LAPIC compartilhavam
o vetor 32 no BSP: uma interrupção LAPIC podia rearmar o HPET, atualizar uptime e
preemptar pelo mesmo caminho. A implementação desta fase elimina essa ambiguidade.

O HPET é a única fonte monotônica. `clock_monotonic_ns/us/ms()` lê o contador,
estende o contador de 32 bits sob lock quando necessário, converte por quociente e
resto e satura explicitamente, sem multiplicação larga nem dependência de libgcc.
Um guard serializa a publicação monotônica e tenta novamente uma leitura
transitoriamente atrasada entre vCPUs; exaustão é contabilizada como regressão.
`timer_get_uptime_ms()` é apenas um wrapper e não existe mais `g_ticks`.

Os vetores são agora HPET=32, keyboard=33 e LAPIC=34. O stub HPET reconhece e
rearma o comparator, executa timers/deferred e não preempta. O stub LAPIC captura
o tempo antes do lock, contabiliza a task local, solicita reschedule e executa EOI
antes da eventual troca. As estatísticas dos dois IRQs são independentes.

## LAPIC

Cada CPU calibra seu timer local em one-shot contra uma janela HPET de 10 ms,
com divisor 16. `elapsed_ticks`, frequência, ticks/ms e initial count periódico
são validados; count zero ou frequência inválida impedem `timer-ready`. Não há
initial count cru `10000000`. O valor textual `10000000` restante nos gates é a
janela de 10 ms em nanossegundos (e constantes não relacionadas ao count).

Resultados finais: SMP=1 `62593` ticks/ms; SMP=2 `62723..62990`, spread 0,4%;
SMP=4 spread 0,1%; SMP=8 spread 0,2%. Todas as CPUs receberam IRQ local e ficaram
dentro da tolerância de 20% documentada para QEMU.

## Accounting e snapshots

`task_t` contém `runtime_ns_total` e `last_cpu_slot`. Cada slot lógico mantém
`last_account_ns`, total contabilizado, eventos, regressões e overflows. O runtime
fecha no IRQ LAPIC, no começo de schedule voluntário/preemptivo e em exit. O ponto
semântico da troca é o começo de `schedule_impl`: a task anterior fecha ali e a
seguinte começa ali, sem gap ou dupla contagem. Um snapshot captura um único
`now_ns`, fecha todas as CPUs online sob o lock e só então copia as tasks. ZOMBIE
não acumula. Soma e conversões saturam com telemetria em vez de wrap unsigned.

## Métricas, Time+, ps e TASKMAN

`task_metrics` fornece um sampler compartilhado com 256 baselines por `task_id_t`
de 64 bits. Primeira amostra e task nova são `--`; ZOMBIE é `0.0%`; regressão e
valor acima de 100% ficam marcados e incrementam anomalias. A fórmula usa
quociente/resto. Entradas ausentes do snapshot são removidas do baseline.

O formatter compartilhado produz `MM:SS.mmm` ou `HH:MM:SS.mmm`, suporta horas
com largura crescente, buffers insuficientes e `UINT64_MAX`. `ps` e TASKMAN não
mantêm mais samplers ou cálculos locais; ambos usam runtime em ns, Time+ e slot
lógico. O sampler do TASKMAN é reiniciado em cada abertura. O layout, modal,
refresh padrão e comportamento desta fase foram preservados.

## Arquivos alterados

- Clock/HPET/LAPIC: `clock.[ch]`, `hpet.[ch]`, `lapic.[ch]`.
- IRQ/boot: `idt.[ch]`, `interrupts.c`, `switch.S`, `irq_stats.c`,
  `kernel_init.c`, `smp_boot.c`, `timer.c`, `panic.c`.
- Scheduler/métricas: `task.h`, `scheduler.[ch]`, `task_metrics.[ch]`.
- Shell: `cmd_ps.c`, `cmd_taskman.c`, `cmd_accounttest.[ch]`, `registry.c`.
- Integração/documentação: `makefile`, `AGENTS.md` e este relatório.

## Builds e símbolos

- `make kernel-check JOBS=2`: PASS.
- `make kernel-check JOBS="$(nproc)"` (12 jobs): PASS.
- SHA-256 dos dois `kernel.elf`:
  `b4f506ffdc2e84af4700d5fd8cc77b60c511a824ca11781287cb3596f279d859`.
- Comparação j2/j12: byte a byte idêntica (`cmp=0`).
- `make deps-check`: PASS; `make image`: PASS.
- SHA-256 final de `hobbyos.img`:
  `531da042be16d7cd1cb8734867d1c23943e0d463b680d40693dbf269d3c70dfb`.
- `nm -u kernel.elf`: vazio; nenhum helper 128-bit/libgcc não resolvido.

Logs de build: `artifacts/build/kernel-check-j2.log` e
`artifacts/build/kernel-check-j12.log`.

## Matriz QEMU e funcional

| Cenário | Resultado relevante |
|---|---|
| SMP=1 TCG | clock 1.000.000 reads, regressions=0; idle=99,9%; busy=95,1%; sleep=8,7%; share max=50,1%; sampler/format/long/check PASS |
| SMP=2 auto | LAPIC spread=0,4%; clock/check/sched/sync PASS |
| SMP=4 auto | allcpu=399,7%/400%; anomalies=0; check PASS |
| SMP=8 auto | allcpu=799,3%/800%; migrate=799,1%/800%, 1753 migrações; check PASS |

Os quatro logs registram selftests de clock, LAPIC, sampler e formatter, shell
alcançada, uma calibração por CPU e ausência de panic, #PF, #GP e FATAL.
`schedtest check` terminou com `started == finished`, handoff pendente zero e
violações zero; `synctest check` terminou com violações/lost wakes zero.

As aberturas do TASKMAN foram exercitadas com janelas 100/250/1000/2000 ms,
sempre com begin/end e retorno funcional à shell. Dois `ps` separados por janela
confirmaram o caminho compartilhado; primeira amostra/task nova ficam inválidas
por contrato e ZOMBIE fica congelado/0% pelo sampler e pelo accounting.

## Soak SMP=8

Soak de 120 s com 16 workers, carga allcpu e migração passou. Resultado final:

- total `7989/8000`, non-idle `7989/8000`, máximo individual `523/1000`;
- 32.881.458 leituras, regressões=0, saturações=0;
- 1.778.506 eventos, runtime `1550835250050` ns, overflows=0;
- LAPIC em oito CPUs, spread=0,2%, nenhuma CPU sem IRQ;
- sampler anomalies=0, scheduler violations=0, lost wakes=0;
- `accounttest check`, `schedtest check` e `synctest check`: PASS;
- QEMU encerrado ao final.

## Teste negativo

Um build temporário com `HOBBYOS_ACCOUNT_NEGATIVE_IRQ_TICKS` restaurou runtime
por IRQ e distorceu os períodos de teste. O harness detectou total `9133/4000`,
oito anomalias e máximo individual `1276`, emitindo
`[ACCOUNT][NEGATIVE] IRQ_TICK_MODEL_DETECTED`. O QEMU foi parado, o código
temporário removido e todos os builds/matrizes normais refeitos. A macro não está
presente no resultado. Evidência: `artifacts/build/cl06-negative-irq-ticks.log`.

## Gates, regressões e warnings

Os gates confirmam: sem `runtime_ticks_total`, `g_ticks`, `INT_VECTOR_TIMER`,
`scheduler_account_timer_tick`, sampler duplicado ou `hpet_time_ms`; stubs HPET e
LAPIC separados; `runtime_ns_total`, `last_account_ns`, clock canônico e sampler
compartilhado presentes. O hardcode LAPIC antigo não existe.

CL-01 a CL-05-FIX permanecem verdes: build reprodutível, guards de bootstrap,
slots/IDs/flags/registry/queue/snapshot, switch/handoff e synchronization checks.
O compilador ainda exibe warnings de estilo preexistentes e alguns avisos de
indentação nas rotinas compactas novas; não há warning convertido em erro nem
efeito funcional observado.

## Artefatos e status

- QEMU: `artifacts/build/cl06-smp{1,2,4,8}-serial.log`.
- Soak: `artifacts/build/cl06-soak-smp8-serial.log`.
- Negativo: `artifacts/build/cl06-negative-irq-ticks.log`.
- Relatório: este arquivo.

Status final: **APPROVED WITH NON-BLOCKING NOTES**. Nenhuma macro negativa,
socket/PID ou QEMU ativo integra o resultado. Afinidade, lifecycle/kill/reaper,
input router, UI final e qualquer item TMV1-CL-07+ permanecem fora do escopo e
não foram iniciados.

## Pós-homologação CL-06-FIX (2026-07-31)

A auditoria posterior fechou overflow de muldiv, validade/delays HPET, timestamp exato do snapshot, rate LAPIC por janela, completion de workers, lifecycle real e telemetria opt-in de ps/TASKMAN. A nova matriz SMP=1/2/4/8 e soak misto de 120 s terminaram com regressions/saturations/overflows/anomalies/drops/violations zero. Hash final corrigido da imagem: `5e47f8e8f4d2603e1bcdf799b4c2ef288908802d7787ddbe15edb1384680d6a9`. Evidência completa em `TMV1-CL-06-FIX-metric-clock-validation.md`.
