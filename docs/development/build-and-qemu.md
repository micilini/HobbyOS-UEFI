# Build e QEMU

A interface canônica é o Makefile raiz. `make deps-check` inventaria separadamente toolchain do kernel, GNU-EFI/imagem e QEMU; use `SCOPE=kernel|image|qemu|all`. Em Debian/Ubuntu, os pacotes normalmente envolvidos são `build-essential`, `binutils`, `gnu-efi`, `mtools`, `qemu-system-x86`, `ovmf` e `python3`; o script apenas sugere, nunca instala.

## Build

```bash
make kernel-check JOBS=2
make image
```

`kernel-check` faz clean build, preserva o log em `artifacts/build/` e imprime SHA-256. Overrides: `ARCH`, `CC`, `LD`, `OBJCOPY`, `EFIINC`, `EFILIB`. A imagem requer GNU-EFI e mtools; o kernel não.

## QEMU

`ACCEL=auto` escolhe KVM somente com `/dev/kvm` acessível, senão TCG. `ACCEL=kvm` exige KVM e usa `-cpu host`; `ACCEL=tcg` usa a CPU emulada `max`. Ajuste `SMP`, `MEM`, `QEMU`, `OVMF_FD` ou o par `OVMF_CODE`/`OVMF_VARS`. O template VARS nunca é escrito: uma cópia vai para `.qemu/`.

Para classificação SMP do clock, selecione `TCG_THREAD=multi` ou
`TCG_THREAD=single`. Cada start registra acelerador, thread, SMP e versão do QEMU
em `.qemu/launch.env`. As matrizes canônicas são:

Para validar o acesso split32 ao contador HPET e rollovers naturais, use
`scripts/test-clock-rollover.sh all`. Os comandos de diagnóstico no guest são
`accounttest hpet-stats`, `accounttest hpet-rollover 1` e
`accounttest hpet-wrap-soak 130000`. Mesmo quando o HPET anuncia counter de 64
bits, o main counter é lido como high-low-high com retry limitado; a capability
não garante que uma transação MMIO de 64 bits seja atômica.

O gate LAPIC separa estado programado, liveness e taxa de serviço. Use
`accounttest lapic-config`, `accounttest lapic-liveness 500`,
`accounttest lapic-rate 2000 5` e `accounttest lapic-after-load`, ou a matriz
`scripts/test-lapic-rate.sh all`. A taxa somente é certificada em rounds
quiescentes, depois de os workers desaparecerem do registry e o reaper chegar a
`current_zombies=free_inflight=0`.

```bash
scripts/test-clock-matrix.sh
scripts/test-cl07-matrix.sh
scripts/test-cl08.sh all
```

O gate de quiescência de testes usa `accounttest reaper-quiescence 32`. Depois
que o workload termina, ele dirige `scheduler_reap_zombies(0)` e prova a
remoção pelos `task_handle_t` exatos, com três observações estáveis e
`free_inflight=0`. `accounttest clock-smp 32 1000000` usa o mesmo contrato; o
delta global de `reaped` permanece apenas telemetria.

O canário paginado do registry é `reaptest snapshot-boundary 100`. Ele mantém
um `cc-boundary` ZOMBIE sob hold na primeira página, remove esse handle exato e
exige generation diferente antes da segunda página. O contador global `reaped`
não participa da prova porque avança somente depois do free fora do lock.

Modo humano: `make run SMP=1 ACCEL=tcg` (foreground e janela gráfica). Modo agente:

```bash
make qemu-agent-start SMP=1 ACCEL=tcg
make qemu-agent-status
make qemu-agent-monitor HMP='info status'
make qemu-agent-type TEXT='taskman 1000' ENTER=1
make qemu-agent-key KEY=esc
make qemu-agent-logs
make qemu-agent-stop
```

O agente usa `.qemu/hmp.sock`, `.qemu/qemu.pid` e logs `.qemu/qemu-{serial,debugcon,trace}.log`. O start recusa instância viva e limpa apenas pid/socket stale; o stop valida que o PID pertence ao QEMU com este monitor, tenta HMP quit, SIGTERM e só então SIGKILL. Para diagnóstico, rode status; se stale for confirmado, start/stop remove apenas esses metadados. `scripts/wait-for-log.sh FILE PATTERN TIMEOUT` oferece espera limitada.

## Minimum version tested in TMV1-CL-01

- GCC 13.3.0
- GNU ld/objcopy 2.42
- GNU Make 4.3
- QEMU 8.2.2
- Python 3.12.3

Esta lista registra somente as versões efetivamente testadas e não promete compatibilidade além delas.

## Limitações

TMV1-CL-01 homologa build e harness, não o comportamento do TASKMAN. Bootstrap SMP, scheduler, lifecycle, input e validação funcional pertencem às fases posteriores.
## Certificação de input CL-09

O roteador e as filas de teclado/default/modal são validados por `inputtest`.
Os testes principais são `inputtest queue`, `inputtest try-wait`, `inputtest
producers`, `inputtest begin-boundary`, `inputtest end-boundary`, `inputtest
transitions 10000` e `inputtest keyboard`. A homologação completa é iniciada por
`make test-cl09`; o harness serializa o acesso a `.qemu`, sincroniza cada retorno
à shell e encerra o QEMU em sucesso ou falha.
## Owned modal UI certification (CL-10)

Use `make test-cl10` for the serialized SMP matrix, causal modal negatives,
TASKMAN normal/killed HMP flows, CL-07/08/09 regressions, and the final soak.
Useful interactive diagnostics are `modaltest check`, `modaltest stats`,
`modaltest open-close 1000`, token/owner tests, owner kill/exit tests, failure
injections, boundary tests, and `modaltest completion-once`.

Modal identity is the copied task ID plus lifecycle generation, never the task
name.  End and input operations require the current session token.  TASKMAN's
loop runs in `taskman-ui`; the shell waits on runtime completion.

## TASKMAN V1 paginado

```bash
taskmantest all
taskmantest setup 129
taskman 50
taskmantest cleanup
scripts/test-cl11.sh positive
scripts/test-cl11.sh all
scripts/test-hmp-transport.sh
```

Text/key injection uses explicit HMP hold times through the normal, stress and
sync profiles. Command timeouts are probed with an idempotent input marker so a
lost synthetic key sequence is reported separately from a shell/input stall or
a guest fault.

O TASKMAN usa a geometria real do console, desenha por regiao coordenada sem
newline/scroll, cresce o snapshot alem de 128 e usa ESC como unica saida.

## Timer-reference deterministico (CL-11-FIX6)

```bash
reaptest timer-ref
reaptest timer-ref-loop 1000
scripts/test-cl08.sh timer-ref-deterministic
```

O worker fica bloqueado em um gate antes de armar seu timer. O hold CLAIMED
exato e instalado sob o lock de timers antes da liberacao do gate. Assim o
teste observa SLEEPING/ref=1, CLAIMED, kill aceito, ZOMBIE KILLED, defer por
`TIMER_REFS`, uma unica release e wake stale sem depender da latencia serial.

## Cancelamento preblock e modal exatos (CL-11-FIX11)

```bash
synctest preblock-loop 1000 all
synctest preblock-timer-noise 1000
inputtest preblock-loop 1000
modaltest kill-caller-loop 1000 blocked
modaltest kill-caller-loop 1000 prewait
modaltest kill-caller-loop 1000 completion-ready
scripts/test-cl11.sh all
```

O canario timer-noise mantem um produtor de timers independente enquanto o
target preblock e validado por handle e lifecycle exatos. Stats globais devem
avancar, mas o target deve terminar sem nodes ou refs. Os casos preblock e
modais retêm os handles ate ZOMBIE, validam cleanup e exit reason e somente
reutilizam o workspace depois de handle ausente e `free_inflight=0`.

## Alinhamento de tasks CL-12

```bash
scripts/test-cl12.sh static
scripts/test-cl12.sh smp1
scripts/test-cl12.sh smp4
scripts/test-cl12.sh all
```

O gate CL-12 é deliberadamente focado: static/build, um boot TCG SMP=1, um
boot TCG SMP=4 e build final reproduzível. Ele não chama a matriz histórica ou
o soak da CL-11; a consolidação integral pertence à CL-13.

`ps [page] [page_size]` usa páginas 1-based, size default 32 e máximo 128. O
snapshot é copy-only, validado por generation/total e repetido até quatro
vezes quando o registry muda. PS e TASKMAN usam `task_format` e
`task_snapshot_t`, mas cada interface mantém seu próprio sampler e sua própria
janela de CPU.

`kill <pid>` solicita cancelamento cooperativo; não interrompe código de kernel
arbitrário. Parser, resultado do scheduler e status CLI aparecem na telemetria
opt-in de `killtest cli-telemetry on`, com return codes distintos.

`taskdiag` é read-only. Os subcomandos principais são `summary`, `task <pid>`,
`scheduler`, `reaper`, `modal`, `input`, `accounting`, `all`, `check` e
`selftest`. `taskdiag trace <pid>` habilita apenas a comparação daquele PID
entre PS e TASKMAN; `taskdiag trace off` desabilita o trace. Não há tracing
contínuo por default.

## Certificação consolidada TASKMAN V1 (CL-13)

```bash
make selftest-kernel
make test-cl13

scripts/test-taskman-v1.sh preflight
scripts/test-taskman-v1.sh static
scripts/test-taskman-v1.sh selftest
scripts/test-taskman-v1.sh matrix
scripts/test-taskman-v1.sh quantum
scripts/test-taskman-v1.sh negatives
scripts/test-taskman-v1.sh soak4
scripts/test-taskman-v1.sh soak8
scripts/test-taskman-v1.sh final-build
scripts/test-taskman-v1.sh report
```

`SELFTEST=1` registra `tasktest`; `SELFTEST_AUTORUN=1` também executa a matriz
bounded depois do scheduler, timers, input, modal e reaper iniciarem. O release
padrão usa ambas as flags em zero, não registra `tasktest` e nunca inicia
autorun. Logs do autorun são validados por `scripts/parse-selftest-log.py` e os
resultados de todos os estágios entram em `artifacts/build/cl13-ledger.tsv` e
`.json`.

A matriz CL-13 usa TCG e KVM sem sudo ou afinidade especial. `accounttest
lapic-config` e `accounttest lapic-liveness` são gates; a campanha limpa de
`lapic-rate` não é repetida. Fault injection roda cada macro em um guest
isolado, exige o sentinela causal `DETECTED`, reconstrói o release normal e
executa o canário positivo correspondente. Os soaks de 180 segundos mantêm um
único QEMU por cenário e terminam com heap sem drift e todos os validadores.

### Canário de âncora TASKMAN da CL-13-FIX2

```bash
scripts/test-cl13-taskman-anchor-focused.sh static
scripts/test-cl13-taskman-anchor-focused.sh negative-anchor
scripts/test-cl13-taskman-anchor-focused.sh positive-anchor
scripts/test-cl13-taskman-anchor-focused.sh variants
scripts/test-cl13-taskman-anchor-focused.sh soak-smoke
scripts/test-cl13-taskman-anchor-focused.sh all
```

Sessões TASKMAN dirigidas pelo host devem executar `taskmantest anchor-reset`
imediatamente antes de `taskmantest auto-exit-frames`. O reset garante altura
real para um frame WIDE/COMPACT; frames de fallback por `TOO_SHORT` ou
`TOO_NARROW` não satisfazem o alvo. O timeout curto da matriz tenta ESC,
aguarda o fim modal e coleta `taskmantest stats` para distinguir fallback de
layout, render stall, falha guest-side e encerramento do QEMU.

### Transporte transacional SELFTEST da CL-13-FIX3

```bash
scripts/test-cl13-transport-focused.sh all
source scripts/harness-framed.sh
```

Os runners CL-13 enviam linhas de shell como `tasktest exec <seq> <crc>
<payload>`. O CRC-32/ISO-HDLC é validado antes de side effects, a sequência é
monotônica por boot e `BEGIN`/`END` separam entrega de frame de execução do
handler. Retry da mesma sequência é permitido somente antes de `BEGIN`; após
conclusão, o guest responde `REPLAY` sem executar novamente. Linha parcial é
submetida/limpa com três Enters bounded antes da nova tentativa. ESC, setas,
PageUp/PageDown e Home/End continuam HMP raw porque são input modal real. Essa
API só existe com `SELFTEST=1`; o release final não registra `tasktest`.

### Fixture real de 257 tasks (CL-13-FIX4)

```bash
taskmantest fixture-capacity
taskmantest fixture-status
scripts/test-cl13-fixture257-focused.sh all
scripts/test-taskman-v1.sh resume-after-fixture257
```

O fixture test-only aceita de 1 a 257 tasks. Os casos 129 e 257 atravessam,
respectivamente, os crescimentos 128 para 256 e 256 para 512 do modelo real do
TASKMAN. `setup` valida parser/range/busy antes de efeitos, publica cada handle
exato e faz rollback pelo mesmo cleanup se criação ou visibilidade falhar.
`cleanup` é idempotente, aguarda todos os handles desaparecerem e exige
`free_inflight=0`. A matriz final continua usando frames para comandos de shell
e HMP raw somente para teclas modais; o artefato release não contém autorun.

### Contratos assíncronos da CL-13-FIX5

```bash
scripts/test-cl13-async-contract-focused.sh all
```

`framed_send_complete` permanece SYNC estrito: o marker funcional antecede
END. Scheduler stress usa `framed_schedtest_async`, que valida START antes de
END, extrai o run ID não zero, chama `schedtest async-wait <run> <timeout_ms>`
e aceita o marker correlacionado antes ou depois do END do lançamento. O
negative entry-window usa `framed_schedtest_async_negative`.

Os contratos são explícitos: SYNC, ASYNC, LAUNCH_ONLY e MODAL. `smpstress` é
launch-only e termina pelo kill sweep posterior; TASKMAN permanece modal com
start/finish framed ao redor das teclas raw.

### Registros SELFTEST e gate JSON da CL-13-FIX6

```bash
source scripts/harness-selftest.sh
scripts/test-cl13-selftest-record-focused.sh all
```

Cada resultado e summary SELFTEST e construido em um buffer bounded de 256
bytes e enviado por uma unica chamada `serial_write_all`. O helper extrai o
primeiro segmento AUTORUN, normaliza apenas o terminador CRLF e chama o parser
estrito com `--require-clean-framing` e os casos obrigatorios. O gate exige
autorun PASS, zero FAIL/SKIP, summary balanceado, `case_count=pass` e
`framing_repairs=0`; texto estrangeiro entre records e ignorado, mas texto na
mesma linha de um record continua sendo rejeitado.

O focused FIX6 atualmente para em SMP8/TCG run 2: cinco records atomicos
ficaram prefixados por fragmentos xHCI que ja tinham uma linha serial aberta,
produzindo 112 casos parseados contra summary 117. A classificacao preservada
e `REJECTED_CL13_FIX6_SELFTEST_SUMMARY`; nao prossiga para a matriz consolidada
sem uma nova decisao causal.

### Line fence SELFTEST da CL-13-FIX7

```bash
scripts/test-cl13-selftest-line-focused.sh all
```

Registros parseaveis SELFTEST sao emitidos como uma unica string
`\n[SELFTEST][...]\n`. O primeiro newline encerra qualquer linha parcial de um
writer estrangeiro e permanece protegido pelo mesmo lock da chamada que
emite o registro. Resultados, summary, AUTORUN BEGIN/PASS/FAIL e EMIT_ERROR
seguem esse contrato; o driver serial e os writers xHCI nao mudam.

O parser rejeita explicitamente markers SELFTEST encontrados fora da coluna
zero, sem extrair substrings. O helper compartilhado exige
`framing_repairs=0`, `line_boundary_violations=0`, balance entre summary e
casos e os casos P0 obrigatorios em todo boot SELFTEST novo.

### Readiness, records HARNESS e synctest assíncrono (CL-13-FIX8)

```bash
source scripts/harness-runtime-ready.sh
scripts/test-cl13-runtime-ready-focused.sh all
scripts/test-cl13-harness-record-focused.sh all
scripts/test-cl13-synctest-async-focused.sh all
```

O boot publica `SHELL_READY` apenas depois de terminal, `shell_init` e thread
da shell. `RUNTIME_READY` valida CPUs, scheduler, DPC, input/modal, PCI e uma
janela bounded de quiescencia do hotplug. O autorun SELFTEST completo ocorre
depois desse marker, e `TEST_READY` e a autorizacao exclusiva para
`tasktest exec`; frames antecipados sao rejeitados sem consumir sequencia.

Records FRAME/BEGIN/END/REPLAY/STATUS do HARNESS usam buffer bounded e uma
unica chamada serial com line fence. O parser host ancora esses records na
linha completa. `synctest sem`, `boundary`, `boundary-noise`, `sleep`,
`cancel`, `timer-cancel` e `race` sao ASYNC: o host valida START e END,
extrai o run ID e conclui por `synctest async-wait` sem reenviar o payload.

### Clock e servico de sleep (CL-13-FIX9/FIX10)

```bash
scripts/test-cl13-clock-contract-focused.sh all
scripts/test-cl13-clock-sleep-contract-focused.sh all

accounttest clock-contract
accounttest clock-wrapper-loop 100000
accounttest clock-loop 10
accounttest sleep-profile 10 128
accounttest sleep-profile-set
```

`timer_get_uptime_ms` e um wrapper que faz outra leitura do clock monotonico.
Leituras consecutivas em milissegundos podem atravessar uma ou varias bordas;
o contrato e `before <= wrapper <= after`, sem igualdade exata.

Clock correctness e latencia de servico de sleep sao gates separados. O sleep
nao pode retornar antes do deadline, com tolerancia de 1 ms correspondente a
granularidade do timer. Overshoot e retorno tardio permanecem visiveis como
telemetria. O limite historico `target * 2 + 50 ms` mede qualidade p95 e nao
reprova uma unica amostra. Os boots KVM preservam load, CPU PSI, cpuset,
`cpu.max`, `/proc/stat`, threads de maior CPU, versao QEMU e acesso KVM para a
classificacao. FIX9/FIX10 nao modificam clock, timer, HPET, LAPIC ou scheduler.

### Oraculo de status framed no soak (CL-13-FIX12)

```bash
framed_send_status_complete \
  "inputtest producers 100000" 900 stress 0 \
  "[INPUTTEST][PRODUCERS] PASS accepted=100000"

scripts/cl13-soak-checkpoint.py verify-fix11-input-failure
scripts/test-cl13-status-oracle-focused.sh all
scripts/test-taskman-v1.sh resume-after-soak8-input-marker
```

Os contratos framed sao separados: SYNC_RECORD exige o record estruturado;
SYNC_STATUS usa `ACCEPT`, `BEGIN` e `END`/`REPLAY` com status permitido;
ASYNC correlaciona run ID e async-wait; LAUNCH_ONLY valida o inicio; MODAL
mantem o handler aberto durante teclas raw. Em SYNC_STATUS o marker humano e
somente diagnostico e pode ser dividido por output concorrente sem contradizer
um `END status=0`.

O canario focused conserva `smpstress` com periodo de 1000 ms para provocar
colisoes. O soak usa `SMPSTRESS_PERIOD_MS=10000`, alterando apenas a frequencia
de log: workers, jobs A/B/C, carga, kill/reap e todos os volumes permanecem
iguais. O resume FIX12 reusa por hash o checkpoint FIX11 e o soak4; executa
somente o focused curto, um soak SMP8/KVM completo e o build final.

### Baseline exato do heap modal (CL-13-FIX13)

```bash
scripts/cl13-modal-checkpoint.py verify-fix12-modal-failure
scripts/test-cl13-modal-heap-focused.sh all
scripts/test-taskman-v1.sh resume-after-soak8-modal-baseline
```

O retorno zero de `scheduler_reap_zombies(0)` informa apenas que aquela
chamada nao claimou tasks; nao prova quiescencia. O `modaltest open-close`
aguarda o handle exato do warmup desaparecer, observa tres vezes
`current_zombies=0` e `free_inflight=0` e exige tres snapshots estaveis de
bytes e blocos antes do baseline. Os handles exatos de todos os workers sao
mantidos em BSS e precisam estar ausentes do registry antes do snapshot final.

O record atomico preserva `direction=ZERO|UP|DOWN` e `signed_delta`. `DOWN`
classifica baseline contaminado por cleanup tardio e nunca e convertido em
leak por valor absoluto. O focused mantem `smpstress` ativo em dois boots
SMP4/KVM e tres SMP8/KVM. A retomada valida os checkpoints anteriores sem
reexecuta-los e roda apenas um novo soak SMP8/KVM completo.

### Ownership modal versus heap global (CL-13-FIX14)

```bash
scripts/cl13-modal-ownership-checkpoint.py verify-fix13-focused
scripts/test-cl13-modal-ownership-focused.sh all
scripts/test-taskman-v1.sh resume-after-modal-global-heap-noise
```

`modaltest open-close` separa o contrato de ownership modal do heap global.
O hard gate interno exige o desaparecimento dos handles exatos, quiescencia do
reaper, deltas exatos de runs/workers/completions/cleanup/signals e invariantes
limpas de runtime, sessao, router, shell e test controls. Os snapshots de heap
e de timer permanecem no record atomico como diagnostico.

Sob servicos concorrentes, um delta de 96 bytes/um bloco pode ser apenas um
`timer_node_t` transitorio de `timer_sleep(1)`. Por isso o gate de heap global
do cenario e `taskmantest heap-begin`/`taskmantest heap-end`, executado somente
depois do sweep de `smpstress`. A retomada FIX14 verifica por hash as
evidencias anteriores, nao repete os gates historicos e executa apenas o
focused FIX14 e um novo soak SMP8/KVM completo.

### Transacao TASKMAN e batching de PS (CL-13-FIX15)

```bash
taskmantest auto-session 50 1
taskmantest auto-session-loop 25 1 1
tasktest ps-loop 100
scripts/cl13-taskman-soak-checkpoint.py verify-fix14-taskman-failure
scripts/test-cl13-taskman-transaction-focused.sh all
scripts/test-taskman-v1.sh resume-after-taskman-arm-marker
```

`taskmantest auto-session` limpa a ancora, arma o auto-exit e chama
`cmd_taskman` diretamente dentro de um unico handler. O retorno so e zero
quando os stats before/after provam uma sessao, o numero exato de full frames,
zero fallback/shortfall/scroll/clipping e test controls restaurados. Os textos
`AUTO_EXIT_ARM` e `AUTO_EXIT` permanecem diagnosticos e podem ser intercalados
por telemetria concorrente.

`auto-session-loop` preserva o pattern 50/1000/2000 ms pelo indice global e
mantem exactly-once no frame externo. `tasktest ps-loop` chama o `cmd_ps` real
em cada iteracao; a unica otimizacao e retirar round-trips HMP. No soak, cinco
batches totalizam 500 snapshots PS e quatro batches totalizam 100 sessoes
TASKMAN sob stress. A retomada reutiliza as evidencias historicas e executa
somente o focused FIX15 e um novo soak SMP8/KVM completo.

## Fechamento e release TASKMAN V1

A documentação canônica começa em `docs/taskman-v1-contract.md`; arquitetura,
lifecycle, input/modal, uso, comandos, limitações, plano de testes,
homologação e handoff ficam nos demais `docs/taskman-v1-*.md`.

```bash
scripts/verify-taskman-v1-evidence.py all
scripts/verify-taskman-v1-docs.py all
scripts/test-cl14-release.sh all
scripts/package-taskman-v1-release.sh
```

O fechamento CL-14 reutiliza a evidência congelada por hash e não repete
matrix, quantum, negatives ou soaks. Seu cone permitido é documentação,
verificação estática, build release/SELFTEST e um único smoke release SMP.
Bare-metal é uma campanha posterior. A autorização de entrada na V2 não
implementa afinidade, ownership de memória, processos ou scheduler topológico.
