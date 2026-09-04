# TMV1-CL-10-FIX3 — gate de configuração, liveness e taxa LAPIC

Data: 2026-08-02
Branch: `feat/taskman`
Base: `76d9a35f045322246fd6997226fcf008d79ed0ea`

## Diagnóstico

Em KVM dedicado, QEMU 8.2.2/SMP=8 apresentou spread de calibração próximo de
0,4% e aproximadamente 500–508 IRQs por CPU em 500 ms. Na execução agregada
anterior, `clock-smp 32 1000000` marcou os workers como done, mas o reaper ainda
emitia `FREE` quando `accounttest check` abriu uma única janela de 500 ms. Todos
os CPUs ficaram abaixo do nominal, entre 347/516 e 475/516.

A queda simultânea, junto da configuração consistente e do resultado dedicado
normal, separa disponibilidade de vCPU/taxa de serviço de erro de programação
de um timer local. KVM não precisa entregar em catch-up cada período virtual
perdido enquanto uma vCPU não executa. O guest continua usando clock monotônico
para accounting e não sintetiza IRQs ausentes.

## Estado e validação

Cada CPU publica calibração, vector, divisor, initial count, LVT/TDCR/TICR
readback, geração de programação, flags periodic/masked, count, último IRQ e
maior gap. Campos cross-CPU são lidos/escritos atomicamente. A programação só
fica ready quando vector 34, divisor 16, initial count, periodic e unmasked
conferem com readback.

O handler captura uma única amostra monotônica, registra count/gap/last IRQ e a
reutiliza no accounting. Não há serial no hot path.

## Separação dos testes

- `lapic-config` valida calibração e readback, mantendo o limite de spread.
- `lapic-liveness` exige count e last IRQ avançando em cada CPU, sem inferir
  taxa nominal.
- `lapic-rate 2000 5` calcula cinco ratios, mediana por CPU e worst median,
  mantendo o piso 70% e teto 130% em execução dedicada/quiescente.
- `lapic-after-load` executa 32 workers/1.000.000 reads, espera todos os handles
  ausentes, `current_zombies=free_inflight=0`, aguarda settle medido e só então
  repete config, liveness e rate.

`accounttest check` agora contém apenas gates rápidos e duros: clock/HPET,
configuração LAPIC, liveness, accounting e métricas.

## Evidência inicial da matriz

TCG multi e single passaram antes/depois de carga. Três boots KVM independentes
passaram; worst medians dedicadas antes/depois da carga foram:

```text
KVM run 1: 895 / 868
KVM run 2: 840 / 960
KVM run 3: 898 / 821
TCG multi: 951 / 950
TCG single: 976 / 976
```

Todos os `clock-smp` reportaram `reaped=32 free_inflight=0 zombies=0`, clock
API/local/cross zero, e nenhum CPU teve round com zero IRQ. O host tinha 12 CPUs
permitidas para 8 vCPUs; load inicial `2.63 2.15 1.71`. Evidência completa está
em `artifacts/build/cl10fix3-kvm-host.env`.

## Negatives

`HOBBYOS_LAPIC_NEGATIVE_MASK_ONE_CPU` realmente mascara o LVT de uma CPU;
liveness observou `delta=0 masked=1` e emitiu
`LAPIC_MASKED_CPU_DETECTED`. `HOBBYOS_LAPIC_NEGATIVE_WRONG_PERIOD` realmente
duplica o initial count de uma CPU; config/readback emitiu
`LAPIC_WRONG_PERIOD_DETECTED`. Ambos rodaram isolados e foram seguidos de
rebuild normal.

## Regressões HPET e CL-10

A matriz FIX2 foi repetida em TCG multi, TCG single e KVM. O selftest emitiu
`HPET_COUNTER_ACCESS_OK`, o negative causal emitiu
`TORN_64BIT_COUNTER_READ_DETECTED`, e o soak final atravessou quatro rollovers
naturais em 180.000 ms. Foram 1.626.431 migrações de readers, um retry split
normal e zero retry exhaustion, regressão API/local/cross ou corrupção.

A matriz modal SMP=1/2/4/8, TASKMAN normal/killed, concurrent callers,
snapshot race, stress e negatives passaram. No soak final:

```text
modal stress: normal=5000 killed=1000 recovered=100 busy=1000 token=1000 failures=100
snapshot race: transitions=100000 snapshots=100000 violations=0
caller cancellation: 100/100
TASKMAN: 100 ciclos (10 normais, 90 killed)
modal window: 180000 ms, 3 ciclos completos
session=INACTIVE contexts=0 quarantine=0 duplicates=0 heap_drift=0
```

## Regressões integrais e soak

`scripts/test-cl07-matrix.sh`, `scripts/test-cl08.sh all` e
`scripts/test-cl09.sh all` concluíram integralmente. Uma repetição aninhada do
sweep CL-07 expirou em 180 s apesar de os 16 workers continuarem progredindo;
o prazo de observação foi corrigido para 600 s, sem mudar workload ou sentinel.
A matriz repetida emitiu nove `KILL_SWEEP PASS` com
`killed=cleaned=reaped=16` e a CL-09 integral terminou em PASS.

No soak final, rate LAPIC antes da carga teve worst median 960. Depois de
`clock-smp 32 1000000`, quiescence comprovou `reaped=32 free_inflight=0
zombies=0`; o `lapic-after-load` teve worst median 957 e o rate final 956.
Configuração/liveness passaram em todos os CPUs, sem CPU com zero IRQ. Checks
finais de scheduler, sync, accounting, kill, reaper, input e modal passaram;
não houve panic, #PF, #GP ou FATAL.

Artefatos principais:

```text
artifacts/build/cl10fix3-lapic-tcg-multi.log
artifacts/build/cl10fix3-lapic-tcg-single.log
artifacts/build/cl10fix3-lapic-kvm-run1.log
artifacts/build/cl10fix3-lapic-kvm-run2.log
artifacts/build/cl10fix3-lapic-kvm-run3.log
artifacts/build/cl10fix3-kvm-host.env
artifacts/build/cl10fix3-negative-lapic-masked.log
artifacts/build/cl10fix3-negative-lapic-period.log
artifacts/build/cl10fix2-soak-smp8.log
```

## Build e status final

`make stack-check` passou com frame máximo 1680 bytes e zero violações. Builds
JOBS=2 e JOBS=12 passaram e foram byte a byte idênticos:

```text
kernel j2/j12: 6dbc7ba6e72cd61d6a420c7ff9611e7558e6c4fdf698ecfcdc5261fe991e3aa8
cmp: 0
kernel.elf: 6dbc7ba6e72cd61d6a420c7ff9611e7558e6c4fdf698ecfcdc5261fe991e3aa8
hobbyos.img: f5715904d063c7e88e22f30c5f8b9fc25b8014e17263d26c3db8067d24381864
nm -u: vazio
```

`make deps-check` e `make image` passaram. Os warnings restantes são legados
fora dos arquivos CL-owned (funções de debug não usadas, `.note.GNU-stack`,
segmento RWX e dois warnings do bootloader). Nenhuma macro negative permanece
no build normal. Status final: aprovado para o commit único da CL-10/FIX/FIX2/FIX3;
a TMV1-CL-11 não foi iniciada.
