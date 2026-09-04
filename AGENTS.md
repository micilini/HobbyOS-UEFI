# HobbyOS-UEFI

Kernel x86_64 freestanding em C. O layout principal é `bootloader/`, `kernel/`,
`shared/` e `scripts/`; o Makefile raiz é a interface canônica.

## Build canônico

```bash
make deps-check
make kernel-check
make image
```

Toda mudança de kernel executa `make kernel-check`. Mudanças que afetam a
imagem executam também `make image`. Frames automáticos do kernel não podem
ultrapassar 2048 bytes; use `make stack-check` quando o caminho alterado roda
em task do kernel.

Para candidatos bare-metal de produção, use obrigatoriamente
`make production-image`. `SELFTEST_AUTORUN=1` exige `SELFTEST=1`. Breadcrumbs
de boot usam uma linha serial atômica por marker. Não introduza limite de CPUs
como atalho diagnóstico; o LAPIC permanece em 1 ms salvo experimento posterior
explicitamente autorizado.

## QEMU para agente

```bash
make qemu-agent-start SMP=1 ACCEL=tcg
make qemu-agent-status
make qemu-agent-monitor HMP='info status'
make qemu-agent-type TEXT='taskman' ENTER=1
make qemu-agent-key KEY=esc
make qemu-agent-logs
make qemu-agent-stop
```

Use HMP, não automação de janela. Acompanhe serial, debugcon e trace. Sempre
pare o QEMU e remova apenas sockets/PIDs temporários pertencentes à execução.
Somente um harness pode possuir `.qemu` por vez.

## Regras de runtime

- Em modo IOAPIC, o PIC legado permanece mascarado e toda redirection entry
  desconhecida começa mascarada. IRQ ISA é resolvida por MADT ISO e GSI base.
- Timers LAPIC são calibrados em 1000 us, preparados mascarados e ativados
  somente no release. Inicializar o contador HPET não arma timer0.
- HPET é somente clocksource de produção: Timer0 permanece quiescente. O
  LAPIC do BSP é o único clockevent global; APs fazem apenas accounting e
  preempção locais com o mesmo período de 1000 us.
- APs aguardam o barrier de interrupções com IF=0. Preempção por hard IRQ exige
  handoff voluntário do contexto bootstrap e gate por CPU.
- Serviços externos e MSI só são liberados depois que seus handlers e
  consumidores estão prontos. Nova lógica por CPU usa a topologia descoberta.
- Scheduler e switch devem preservar runqueue, on-CPU, stack guards e o
  handoff físico exato. Estado CPU-local exige pinning por `irq_save`.
- Objetos de espera usam prepare/commit/abort e respeitam `object lock ->
  scheduler lock`. Kill pendente e prepare de wait interruptível são
  serializados pelo scheduler lock.
- Nunca guarde `task_t *` em callback futuro. Depois de publicar uma task, use
  handle ID/generation. Reaper libera somente depois de cleanup, notification,
  off-CPU, filas limpas e referências zero.
- Kernel threads são non-killable por padrão. Kill é cooperativo, tasks
  killable precisam de cancellation points e contexto com ownership não é
  liberado manualmente.
- Clock monotônico não é contagem de IRQ. HPET usa leitura high-low-high; uma
  capability de contador largo não torna a transação MMIO atomicamente larga.
- O gate `accounttest lapic-rate` preserva o threshold 700. Resultado virtual
  só tem autoridade de cadência quando CPUs schedulable do host >= vCPUs do
  guest; oversubscription preserva o FAIL bruto e exige controle KVM não
  oversubscribed, sem retry, pinning ou alteração do kernel.
- Input escolhe destino e enfileira atomicamente sob o router lock. Consumer
  não adquire router/modal lock a partir da queue. Input modal não pode vazar
  para a shell.
- Owner modal é task ID mais lifecycle generation, nunca nome. Toda API de
  input/end exige token. O worker é independente e a shell aguarda completion.
  Cleanup primário e recovery de owner executam exatamente uma vez.
- Testes concorrentes precisam observar concorrência real. Não declare PASS
  por ausência de fault, por contador global não causal ou por sequência
  artificialmente serializada.

## TASKMAN

Os contratos públicos vivem em `docs/taskman-v1-*.md`. TASKMAN usa snapshot
copy-only, ordenação por PID, paginação, refresh entre 50 e 2000 ms e ESC como
única tecla de saída. `USER=Root`, `AFF=Any`, `MEM~` é estimativa de kernel,
`%CPU` é amostra por delta e seleção não executa kill.

Frames estáveis não podem limpar a região inteira. Header e rows compartilham
um único schema de offsets. A apresentação usa `ConsoleCell` e cores nativas;
escapes ANSI crus são proibidos. `graphics.c/.h` e `task_format.c/.h` ficam
protegidos em manutenção estritamente visual.

Gates ativos da apresentação:

```bash
scripts/test-taskman-visual.sh naming
scripts/test-taskman-visual.sh image-payload
scripts/test-taskman-visual.sh focused
scripts/verify-taskman-visual-evidence.py all
```

O SHA-256 bruto da imagem FAT identifica um candidato concreto, mas não é um
oráculo de igualdade entre formatações. Continuidade de uma imagem recriada é
provada pelo kernel e pelos cinco payloads extraídos; serial de volume e
timestamps pertencem aos metadados do container.

A revisão visual humana é separada dos gates automatizados. Não marque o
checklist automaticamente e não substitua artefatos congelados de release por
um candidato de revisão.

## Disciplina de trabalho

Preserve mudanças preexistentes. Não use Git destrutivo, não esconda falhas,
não antecipe campanhas e não faça push sem pedido explícito. Uma falha deve ser
classificada como build, transporte, shell, guest fault ou comando estruturado.
Use status exato do handler e registros machine-readable; marker humano
intercalável não é um segundo oráculo.

Gates permanentes do bring-up:

```bash
irq check
irq controllers
irq routes
irq boot
scripts/test-interrupt-bringup.sh all
scripts/test-timer-clockevent.sh all
```
