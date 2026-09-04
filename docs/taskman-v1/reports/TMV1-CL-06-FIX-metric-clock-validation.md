# TMV1-CL-06-FIX — Metric and clock validation

Data: 2026-07-31

Branch `feat/taskman`; base `5ad2aabbf53766f7da7bc89fb345422b239e2bb5`.

## Correções dos bloqueadores

- `%CPU`: `task_metrics_muldiv_u64` calcula `floor(value*multiplier/divisor)` por quociente/resto e divisão binária, sem produto intermediário largo ou `__uint128_t`; divisor zero e resultado não representável falham.
- HPET: períodos zero ou acima de 100.000.000 fs (limite de 100 ns da especificação) são rejeitados com `[CLOCK][HPET] INVALID_PERIOD`; base/período ficam indisponíveis e o boot não aceita clock fabricado.
- Delays HPET: us/ms→ticks usam muldiv seguro; soma de target, comparator 32-bit e durations extremas falham sem wrap. O vetor/APIC do BSP configurados são preservados e reutilizados, sem literal `(32,0)`.
- Snapshot: o timestamp único que fecha todas as CPUs agora integra `task_snapshot_result_t`; ps/TASKMAN usam esse valor. Isso eliminou falsos >100% causados pela diferença entre fechamento e leitura posterior do clock.
- Selftests: sampler, formatter, long e HPET validity são independentes. Sampler cobre first/0/50/100/>100/regressão/ZOMBIE/nova/remoção/ID64/null/cap/wall; formatter compara strings exatas e buffers; long cobre muldiv extremo e overflow.
- Harness: LAPIC mede deltas por CPU em janela real (default 2 s, tolerância ±30%); clock mede 10/100/1000 ms e 1M reads; workers têm started/finished/IDs/watchdog; lifecycle observa first/new/ZOMBIE congelado 0%/reap.
- `accounttest check` valida clock/HPET/vetores, accounting, stats compartilhadas e rate LAPIC. `core`/`all` executa a suíte curta real.
- Telemetria opt-in: `[PS][METRIC]` e `[TASKMAN][METRIC]`, habilitada por `accounttest ui on` (alias HMP-safe de `ui-telemetry`).

## Evidência

Selftests: `SAMPLER_GUARDS_OK`, `FORMAT_GUARDS_OK`, `LONG PASS` e `HPET_VALIDITY_OK`. Clock SMP=1: 1.000.000 reads; d10=10.676.270 ns, d100=100.059.550 ns, d1000=1.000.986.920 ns. LAPIC: SMP=1 1998/2000 IRQ; SMP=2 1999/1999; SMP=4 e SMP=8 dentro de ±30%, nenhuma CPU zero.

Lifecycle real: `[ACCOUNT][LIFECYCLE] PASS first=1 new=1 zombie=1 removed=1`. Workloads aguardaram todos os workers: SMP=1 idle 999/1000, busy 951, sleep 89, share max 502; SMP=2 allcpu 1997/2000, migrate 2749; SMP=4 allcpu 3994/4000, migrate 4557; SMP=8 allcpu 7990/8000, migrate 3889.

ps registrou primeira amostra inválida e seguinte válida. TASKMAN nas janelas 100/250/1000/2000 ms registrou `session_begin`, frame 1 inválido, frames posteriores válidos, Time+ monotônico, `session_end` e nenhuma amostra >100%.

Soak SMP=8: 20 s sleeping + 20 s migrating + 80 s allcpu. Resultado final allcpu 7992/8000, completion 16/16; migrações 14833. Stats: 28.088.066 reads, regressions=0, saturations=0, overflows=0, metric regressions=0, over100=0, drops=0. Scheduler started=finished=63280, pending=0, violations=0; sync violations/lost wakes=0; nenhum panic/#PF/#GP/FATAL.

Negativos: `cl06fix-negative-muldiv.log` contém `UNSAFE_MULDIV_DETECTED`; `cl06fix-negative-irq-ticks.log` contém `IRQ_TICK_MODEL_DETECTED`. Ambos foram builds temporários e o build normal foi restaurado.

## Build e hashes finais

- kernel-check j2: PASS
- kernel-check jN: PASS
- SHA-256 j2/jN/kernel final: `06de4243cbdbf28f83b3c460b4e8d8e6454c418dc755c5ff5182bfb1a4652685`
- reprodução: `cmp=0`
- deps-check/image: PASS
- SHA-256 `hobbyos.img`: `5e47f8e8f4d2603e1bcdf799b4c2ef288908802d7787ddbe15edb1384680d6a9`
- `nm -u kernel.elf`: vazio

## Artefatos e gates

Logs: `artifacts/build/cl06fix-smp{1,2,4,8}-serial.log`, `cl06fix-taskman-metrics.log`, `cl06fix-soak-smp8-serial.log`, `cl06fix-negative-{irq-ticks,muldiv}.log`. Gates estáticos não encontram multiplicação insegura, período inventado, duration multiplication, literal HPET `(32,0)` ou selftest compartilhado.

Status: **APPROVED**. TMV1-CL-07+ não iniciadas.
