# TMV1-CL-10-FIX2 — rollover do HPET e certificação modal final

Data: 2026-08-02
Branch: `feat/taskman`
Base: `76d9a35f045322246fd6997226fcf008d79ed0ea`

## Diagnóstico

O soak CL-10/FIX terminou com o runtime modal limpo, mas registrou 83.862
regressões locais da fonte, 664.493 clamps de lag cross-CPU e máximo de
42.949.635.930 ns. API regressions e retry exhaustion permaneceram zero.

O ambiente causal usa QEMU 8.2.2, TCG thread multi, SMP=8. O HPET anuncia main
counter de 64 bits, período de 10 ns (`period_fs=10000000`), mas a MemoryRegion
aceita acessos de no máximo quatro bytes e fornece low/high separadamente.

```text
wrap_low32_ns = 2^32 × period_ns
              = 4.294.967.296 × 10
              = 42.949.672.960 ns

observed_max_lag = 42.949.635.930 ns
difference       = 37.030 ns
                 = 3.703 ticks
```

Na leitura antiga, o low podia ser obtido antes do rollover e o high depois:

```text
low_before = 0xFFFFF...
rollover
high_after = old_high + 1

assembled    = (high_after << 32) | low_before
actual_after = (high_after << 32) | low_after
assembled - actual_after ≈ 2^32 ticks
```

A amostra artificialmente futura contaminava o watermark local e global. As
amostras normais seguintes então pareciam regressões locais ou lag cross-CPU,
embora a API continuasse monotônica por clamp.

## Implementação

O main counter não possui mais leitura ou escrita MMIO de 64 bits. Counter de
64 bits usa high-low-high e somente aceita a combinação quando os dois highs
são iguais, com limite de quatro retries. Exhaustion produz sample inválida e
hard failure no classificador. Counter de 32 bits preserva extensão software
sob `g_hpet_lock`, com wrap count e guarda de overflow; suspend maior que um
período de wrap não faz parte da V1.

Capabilities são lidas como low/high estáticos de 32 bits. Init desabilita o
counter, escreve low/high zero, limpa status e só então habilita. Comparator 0
é desarmado sob a lock antes das escritas low/high e tem a configuração
restaurada depois, evitando um target intermediário low-novo/high-antigo.

As estatísticas distinguem `SPLIT64_STABLE` e `EXTENDED32`, contam reads,
retries, exhaustions, rollovers e máximo de retries usando somente operações
atômicas. Eventos do clock guardam mode, retries e high/low brutos junto aos
watermarks existentes.

## Selftests e negative

O selftest cobre leitura estável, mismatch no rollover, retry válido, limite
`UINT64_MAX`, out nulo, extensão 32-bit, wrap software e exhaustion sintético.
Boot normal exige `[CLOCK][SELFTEST] HPET_COUNTER_ACCESS_OK`.

O build `HOBBYOS_HPET_NEGATIVE_TORN_COUNTER_READ` executa causalmente o modelo
antigo com `previous=0x00000007FFFFFFF0`,
`torn=0x00000008FFFFFFF0` e `actual_after=0x0000000800000010`.
Somente após medir o avanço próximo de 2^32 ticks emite
`[CLOCK][NEGATIVE] TORN_64BIT_COUNTER_READ_DETECTED`.

## Evidência inicial de rollover real

No QEMU 8.2.2 causal, TCG multi SMP=8:

```text
[ACCOUNT][HPET_ROLLOVER] PASS wraps=1
high_before=0 high_after=1
low_before=4284998121 low_after=725444
api=0 local=0 cross=0
split_retries=1 retry_exhaustion=0
reader_failures=0 migrations=162
```

O disassembly de `hpet_read_counter64_split` contém exatamente três chamadas a
`mmio_read32`, nos offsets `0xf4`, `0xf0`, `0xf4`, e nenhuma carga MMIO de 64
bits.

## Certificação

O negative causal passou e foi seguido de rebuild normal. A matriz HPET passou
em TCG multi, TCG single e KVM, todos SMP=8/QEMU 8.2.2: rollover real,
`clock-smp 32 1000000` e um `accounttest check` isolado terminaram em PASS com
API/local/cross zero e exhaustion zero. O soak HPET de 130.000 ms observou três
wraps, uma retry split, zero exhaustion e API/local/cross zero.

`make stack-check` passou com máximo de 1920 bytes e zero frames acima de 2048.
Os gates estáticos encontraram zero `mmio_read64`/`mmio_write64` em `hpet.c`.

## Falha do gate agregado

Ao iniciar `scripts/test-cl10.sh all`, a repetição da matriz chegou ao KVM após
TCG multi/single. Rollover KVM e `clock-smp 32 1000000` passaram novamente com
API/local/cross zero. O `accounttest check` seguinte falhou no gate de taxa do
LAPIC:

```text
[ACCOUNT][LAPIC_CPU] slot=2 delta=347 expected=516
[ACCOUNT][LAPIC] FAIL cpus=8 window_ns=516971940 expected=516
irq_min=347 irq_max=475 spread_x10=4
[ACCOUNT][CHECK] FAIL
```

O valor mínimo equivale a 67,2% do esperado e ficou abaixo do piso existente de
70%. A falha foi preservada, não reclassificada nem ocultada. Pelo fail-fast, a
matriz modal, regressões CL-07/08/09, soak final de 180 s e builds/hashes finais
não foram executados nessa tentativa. Status: REJECTED; nenhum commit criado e
CL-11 não iniciada.

## Correção posterior do gate LAPIC

A correção HPET passou em TCG multi/single e KVM. O gate agregado falhou porque
`accounttest check` usava uma única janela de 500 ms de IRQs entregues como
proxy simultânea de configuração, liveness e disponibilidade de vCPU. A
separação, diagnóstico e certificação final estão documentados em
`TMV1-CL-10-FIX3-lapic-gate.md`.
