# Panic de emergência

## Problema e invariante

O caminho de panic anterior elegia um owner, mas depois podia aguardar locks do
console, do próprio panic, do clock monotônico e do HPET. Um peer interrompido
por NMI podia reter qualquer desses objetos. Os wrappers normais de energia
também voltavam ao console. Assim, tanto o diagnóstico quanto a ação terminal
podiam deixar de progredir.

Depois de assumir ownership, o panic não pode esperar um lock que o owner ou
outro CPU possa reter. Toda espera por hardware ou por uma fonte de progresso
antes da ação terminal precisa de um orçamento finito. O laço final com
interrupções desabilitadas é a própria ação HALT.

## Inventário estático anterior à alteração

| Chamada alcançável | Locks, esperas ou acesso falível antes da alteração | Orçamento anterior | Destino da alteração |
|---|---|---|---|
| `panic_enter_owner_or_halt` → `lapic_get_id` | Leitura LAPIC por MSR ou MMIO; a via MMIO altera o trace global e pode repetir o acesso que falhou | Sem fallback de identidade | Identidade arquitetural local por CPUID, com slot topológico somente quando a correspondência lock-free já está publicada |
| `serial_write_all` / `serial_write_hex64_all` | Bypass do lock dependia de `g_panic_in_progress`; polling por porta era limitado, mas cada string não tinha limite próprio | 20.000 polls por tentativa, porta marcada morta após quatro falhas | Modo publicado antes da primeira saída, emissão de emergência incremental e limites explícitos por chamada/porta |
| Mensagem/título e `InterruptFrame` | Ponteiros podem estar ausentes ou ser a origem de nova exceção; o frame não contém registradores gerais | Sem recuperação universal de memória inválida | Dump completo somente enquanto a primeira entrada progride; reentrada usa apenas constantes e inteiros capturados |
| Trace MMIO global | Campos `volatile` são atualizados separadamente e consultas LAPIC posteriores podem sobrescrevê-los | Nenhum snapshot transacional | Cópia best-effort antes de qualquer NMI/ação; campos e limitação de coerência registrados na serial |
| `lapic_send_broadcast_halt` | Esperas do ICR são limitadas, porém os helpers LAPIC usam MSR/MMIO e alteram o trace | 100.000 polls antes e depois do ICR | Executar somente depois de capturar o trace; falha/reentrada segue para ação mínima, sem aguardar peers |
| `graphics_enable_buffering`, `console_clear`, `console_write` e cursores | `g_console_lock`, back-buffer e framebuffer podem estar indisponíveis ou retidos | Espera ilimitada no spinlock | Remover integralmente do panic; a tela normal pode permanecer congelada e a serial é o canal de emergência |
| `g_panic_lock` | O owner ou uma entrada interrompida podia reter o próprio lock | Espera ilimitada | Remover; ownership/profundidade ficam numa palavra atômica única |
| `clock_monotonic_ms` → `g_clock_lock` → HPET | Lock do clock e, em HPET de 32 bits, `g_hpet_lock`; amostras podem parar ou regredir | Espera ilimitada em lock e `hpet_usleep` | Countdown local por TSC quando CPUID fornece frequência válida, com teto absoluto e fallback por iterações |
| `hpet_usleep` | Espera por avanço do contador sem limite independente | Ilimitado se o contador parar | Não alcançável pelo panic |
| `power_restart` / `power_shutdown` | Console, ACPI enable e parser AML no caminho de falha | Esperas de I/O limitadas, mas dependências ordinárias e acessos amplos | Operações privadas mínimas: CF9/8042/triple fault; S5 usa somente dados previamente capturados e validados, senão HALT |
| NMI em `interrupts.c` | Qualquer NMI após `g_panic_in_progress` parava também o owner | Halt terminal indiscriminado | Peers param; NMI no owner entra no protocolo de reentrada |

## Coordenação de owner e reentrada

A primeira instrução do caminho terminal desabilita interrupções locais. A
identidade vem de CPUID `0x1f`, depois `0x0b`, com o APIC ID inicial de CPUID
`0x01` como fallback. Nenhuma dessas alternativas lê LAPIC por MMIO ou MSR. A
palavra atômica `g_panic_owner` guarda `cpu_id + 1` nos 32 bits altos e a
profundidade nos 32 bits baixos. Um compare-and-swap `acq_rel` elege o owner;
em seguida, uma store `release` publica `g_panic_in_progress` antes da primeira
escrita. A palavra de owner continua sendo a autoridade na pequena janela
entre essas duas operações, enquanto a flag preserva a interface dos
consumidores existentes.

Um CPU com outro token desabilita interrupções e entra diretamente no laço de
`hlt`, sem consultar topologia, serial ou locks. A segunda entrada com o mesmo
token eleva atomicamente a profundidade para 2, não consulta topologia, emite
apenas o ID arquitetural, `cpu_slot=unknown`, vetor conhecido, ação e o marker
de reentrada, e executa a ação sem dump ou countdown. A terceira entrada eleva
a profundidade para 3 e faz halt sem saída. O handler de NMI aplica a mesma
distinção: um peer para; o owner volta ao protocolo e não é confundido com
peer.

A resolução de slot ocorre apenas depois dessa classificação. Ela aceita a
topologia quando a contagem está no limite, cada registro possui seu próprio
slot, os APIC IDs são únicos e há exatamente um BSP. Se isso ainda não estiver
publicado, o diagnóstico mantém o ID arquitetural e registra
`cpu_slot=unknown slot_valid=0`. Nenhum array é indexado pelo APIC ID bruto.
A produção vinculou os atomics diretamente nas instruções x86_64; não há
símbolo `__atomic_*` nem dependência de `libatomic`.

## Canal e conteúdo do diagnóstico

O panic não chama framebuffer, graphics ou console. A tela existente pode
permanecer congelada; o canal obrigatório de tentativa é a serial. Com o modo
de emergência publicado, `serial_putc_all`, `serial_write_all` e
`serial_write_hex64_all` não adquirem `g_serial_lock`. O caminho limita a 32
portas descobertas, 20.000 polls por tentativa de byte, quatro falhas antes de
marcar uma porta como indisponível, 1.000.000 de polls para todo o panic e 512
bytes por chamada de string. O motivo tem limite adicional de 240 bytes. Uma
UART ausente ou que não progride pode perder todo o texto, mas não prolonga a
ação além desses limites.

O dump da primeira entrada usa o seguinte protocolo:

| Marker | Campos principais |
|---|---|
| `[PANIC][OWNER]` | ID, slot/validade, fonte da identidade, profundidade, origem, estágio, ação e timeout |
| `[PANIC][DUMP_BEGIN]` | canal e tipo `panic`/`exception` |
| `[PANIC][REASON]` | texto limitado e normalizado para uma linha |
| `[PANIC][VECTOR]` | validade e valor; o sentinela `0xFF` nunca é apresentado como vetor conhecido |
| `[PANIC][FRAME]` | validade, RIP, CS, RFLAGS, RSP e SS do frame existente |
| `[PANIC][ERROR_CODE]`, `[PANIC][CR2]` | validade separada e valor somente quando válido |
| `[PANIC][PAGE_FAULT]` | bits present, write, user, reserved e instruction quando vetor, error code e CR2 permitem |
| `[PANIC][CR3]` | valor do CR3 local |
| `[PANIC][MMIO]` | disponibilidade, endereço, operação, tamanho, valor e indicação de coerência limitada |
| `[PANIC][DUMP_END]` | fim do diagnóstico serial completo |
| `[PANIC][COUNTDOWN]`, `[PANIC][ACTION]` | fonte, orçamento, motivo de expiração e tentativa terminal |
| `[PANIC][REENTRY]` | via mínima da profundidade 2; profundidade 3 é deliberadamente silenciosa |

O trace MMIO é copiado antes do envio de NMI. Seus campos globais continuam
sendo atualizados separadamente por todos os CPUs; a leitura em até três
tentativas apenas fornece uma indicação de endereço estável e é descrita como
`best-effort-global`, não como snapshot transacional. Um fault ao ler a
mensagem, o frame ou outro dado da primeira entrada leva à via de reentrada,
que não relê esses ponteiros.

## Countdown e ações terminais

`panic_config` normaliza o timeout para 1–30 segundos. O countdown usa
`lfence; rdtsc` somente quando CPUID `0x15` fornece denominador, numerador e
frequência de cristal não nulos e o resultado checado fica entre 1 MHz e
20 GHz. Multiplicações, soma e alvo são verificados. Nenhuma função de clock,
HPET, timer, scheduler ou sleep é chamada.

Mesmo com TSC válido há dois tetos: 2.000.000 de amostras consecutivas sem
progresso e um total de 50.000.000 de polls por segundo solicitado, limitado a
500.000.000. Uma amostra inválida ou frequência indisponível usa diretamente
1.000.000 de iterações por segundo solicitado, também com teto, e segue para a
ação sem uma segunda espera. Amostras regressivas contam como falta de
progresso; avanço esporádico continua preso ao teto total. Esses números
provam finitude por iterações, não exatidão em segundos.

HALT faz `cli` e permanece em `hlt`. RESTART tenta os registradores CF9 com
200.000 iterações, o controlador 8042 com 100.000 polls de prontidão e mais
200.000 iterações, e termina em triple fault se o reset anterior não surtir
efeito. SHUTDOWN usa somente portas PM1 de 16 bits e tipos S5 previamente
extraídos e validados fora do panic; após 400.000 iterações sem efeito, ou se
os dados não existirem, registra o fallback e faz halt. Nenhuma dessas vias
chama os wrappers normais de `power/`.

## Fechamento da evidência

A implementação de panic permaneceu byte-idêntica ao checkpoint revisado.
Os SHA-256 de `panic.c`, `panic.h`, `interrupts.c` e `serial.c` continuam,
respectivamente, `9f1e32d5f8d6db8ea3002e87e6d40db073e8008219421c321f3a0b63ac7d0e03`,
`ef0d96af46e897d932bcd29b8fc07bf3d6adb181dd8c520b988538a9b1e6096c`,
`911ac149fd7e4640877804203480587febadf0e3c790a8c4cbd2be203a73a26e` e
`f8f88de33528a90a9f549a18f8f35fe52fa082c2e339241c14dbde03601b3139`.
As alterações deste fechamento ficaram no transporte e nos verificadores de
host, mais dois handlers de diagnóstico: amostragem limitada de IRQ e prova por
handle da referência de timer. Scheduler, reaper, timer, LAPIC, HPET e os
contadores de IRQ de produção não mudaram.

### Transporte e retorno do handler

A imagem de regressão usa `SELFTEST=1`, `SELFTEST_AUTORUN=0`, sem flags de
panic. Cada comando passa por `tasktest exec <seq> <crc> <payload>` e precisa
ter um único ACCEPT, BEGIN e END com a mesma sequência e checksum. Os intervalos
de linhas e bytes são semiabertos; uma linha do comando seguinte nunca pertence
ao anterior. Envio de teclado, texto humano e status do handler permanecem
campos distintos. Um END com status 1 é preservado como falha, mesmo quando há
texto favorável.

A imagem de produção não contém esse protocolo. Nela, o controlador GDB/MI
arma um breakpoint em `shell_dispatch_command_line`, lê o payload em memória,
remove esse breakpoint, deixa o guest executar até o endereço de retorno real
e lê `RAX`. O log bruto preserva payload, RIP e retorno. Esse procedimento não
chama o handler pelo debugger e não escreve seu resultado. A pausa do debugger
não é usada nas janelas de autoridade de cadência.

### Resultado tardio preservado

Na execução histórica, o comando de cadência SMP24 continuou classificado como
TIMEOUT no intervalo 924:924. A linha
`[ACCOUNT][LAPIC_RATE] FAIL ... worst_median_x1000=526 ...` existe depois, na
linha 1071 do serial completo, dentro do intervalo atribuído ao comando
seguinte. O serial tem SHA-256
`1742baeaa5f4d95911b9e320ae2f05ef88fd59d04f108006c9d05c8a90948475`.
Essa observação é `LATE_RESULT_OBSERVED`: corrige a afirmação anterior de que
o resultado nunca foi emitido, mas não altera o timeout bruto, não move a linha
e não determina se o atraso ocorreu no despacho, input, execução ou coleta.

### Comparação focal

Os candidatos anteriores e sob revisão foram construídos em árvores separadas,
com o mesmo controlador corrigido, QEMU 8.2.2, OVMF, KVM e host com 12 CPUs
schedulable. Os ELFs foram
`387208416d5df3818a2da80810c948cc92e767d277ce7c5b73dcf2cecbc4507b`
para a base anterior ao panic e
`308c4609d5523817713c40e1c30fe7097d38f6ddcd5618266d40b94ef7f602c3`
para o checkpoint de panic.

| Diagnóstico | Base anterior | Checkpoint de panic | Conclusão causal |
|---|---|---|---|
| `irq boot`, SMP8 | status 0, oito snapshots | status 1, snapshots incompletos | condição transitória apareceu em um candidato, sem evidência de CPU ausente |
| `irq boot`, SMP24 | status 1, incompleto | status 1, incompleto | reproduzido nos dois candidatos; polls imediatos não davam prazo para sair de contexto transitório |
| `reaptest timer-ref`, SMP4/24 | status 0 | status 0 | os FAIL históricos não foram reproduzidos; não foram atribuídos ao panic nem a corrupção do reaper |
| rate, SMP4 | status 0, PASS | status 0, PASS | controle não oversubscribed válido nos dois |
| rate, SMP24 | status 0, PASS | status 1, FAIL | ambos concluíram pelo transporte causal; variação de cadência sem autoridade no host oversubscribed |

A primeira tentativa da base anterior também foi preservada: o matcher de
prontidão do host não reconheceu o marker existente. Ela é classificada como
falha de observação, e a nova coleta usa outra VM. A comparação completa está
em [`runtime-comparison`](../../artifacts/build/panic-safety/20260905-150503-0300/runtime-comparison/).

### Diagnósticos de IRQ e referência de timer

`irq boot` e `irq check` mantêm a exigência de snapshot consistente. Ambos
usam um prazo global de 5000 ms, polls de 128 tentativas e `timer_sleep(1)`
entre amostras; o prazo não é renovado por slot. Underflow, mismatch, retorno
maior que entrada, contador unexpected/imbalance ou violação do contrato de
runtime falham imediatamente com slot e razão. `irq_check` ainda exige que
`irq_bootstrap_validate()` retorne verdadeiro. No resultado final SMP24,
`irq boot` entregou exatamente os slots 0–23 e terminou com
`snapshots=24 polls=41 elapsed_ms=216`. Os seis `irq check` do perfil usaram
29–70 polls e 11–460 ms, todos com `reason=ok`. Isso demonstra que houve
instabilidade transitória real durante a coleta sem dispensar nenhum slot.

O teste de referência agora identifica o alvo por handle e lifecycle generation.
Ele confirma o nó de timer claimed e sua referência, kill/zombie/off-CPU,
deferimento exato do alvo por `TASK_REAP_DEFER_TIMER_REFS`, hold de observação,
liberação do timer, igualdade acquire/release, zero nó pendente/claimed e
remoção final do mesmo handle. O total global de zombies removidos é apenas
dado auxiliar.

A fixture concorrente cria outro zombie reapável. Em SMP4 e SMP24 ela registrou
`competitor_reaped=1`, `competitor_target_present=1` e `legacy_blocked=0`: uma
remoção alheia torna falso o antigo predicado baseado no retorno global sem
invalidar a proteção do alvo. Três pares independentes passaram em cada perfil.
O binário adversarial que suprime o release retornou status 1 e foi aceito pelo
oráculo somente como `timer-reference-release-absent` detectado.

### Verificadores fechados por padrão

O verificador de regressão aceita correspondência por tokens completos, exige
slots exatos, candidato/perfil/VM/argv coerentes, BEGIN/END e checksums, run ID
assíncrono, screendump e tempo de observação do TASKMAN. O controle válido foi
aceito e 20 fixtures foram rejeitadas, incluindo slot intermediário ausente,
duplicado, fora da faixa e `slot=3` confundido com `slot=30`; marker depois do
fim; END ausente, incorreto ou duplicado; status 1 com texto PASS; replay;
checksum e run ID errados; resultado tardio no comando seguinte; controle de
cadência com host insuficiente; outro ELF; IRQ inesperada; prova de amostragem
ausente; Timer0 ativo; release de timer ausente; e TASKMAN sem settle.

O verificador de panic reabre serial, dois snapshots GDB e o stream QMP bruto.
Ele exige a tentativa ACTION antes do evento de reset/desligamento, evento da
mesma VM depois do trigger e antes do cleanup, origem e motivo compatíveis, e
silêncio mais estado terminal para a terceira entrada. O controle válido de
restart foi aceito e 13 fixtures inválidas foram rejeitadas, inclusive evento
anterior ao trigger, evento apenas no JSON derivado, restart sem ACTION,
shutdown produzido pelo cleanup e identidades de artefato vazias. Os probes
que o verificador anterior aceitava indevidamente agora falham na verificação
completa. As fixtures ficam em
[`oracle-fixtures`](../../artifacts/build/panic-safety/20260905-150503-0300/oracle-fixtures/).

## Binários exercitados

| Papel | Kernel SHA-256 | Imagem FAT SHA-256 |
|---|---|---|
| Regressão com transporte framed | `d9417d19f867654a498009a770d3d5953155f163c967933388e3fffc4fd69696` | `cbacb3f33ecb71b7a5d3cd77407a7fdc5a004e9210f7edb7f03b3fb3152cd5bc` |
| Negativo de timer-reference | `3fa4671839c23a8cfd26d74b41ecd3dc6df1a5c37f80d9a48c468bc5df58e7e1` | `f269711f14c95123237da42cda16447767704289ba82dc0192ce8c367c056ec2` |
| Panic instrumentado | `90d5b43d3b3bcc733b3f0d39eb5b15c2fed6e52b06591804b3cb284b6cbcb78a` | `12a194fb189bb7c4c973f0f05a2d6906061d19248ad188ddca6c639a4b44c2d8` |
| Panic adversarial | `b1a48e790b23ab7acb97ac0b3e5f4e6ab8f332d607d445e6e43054ff73d5595f` | `aa2df10bdd2ee8785ef894f7bc64afd5df26cb6e56aca3823a1fddae0c928ea6` |
| Produção no gate de regressão | `13fb98d56eb311107e3d87114042ffadaae523d0a33ecb5f913a6b10a6841b81` | `766db9ea96d0e84c74e148d095a16b5292f3b2d945bfeb3db2953ec7ed9c242a` |
| Produção no gate de panic | `13fb98d56eb311107e3d87114042ffadaae523d0a33ecb5f913a6b10a6841b81` | `6f2904df516a3f61ee09f8fd2f4560d91577a950685f86e864feb4c031bd2a95` |

Todas as imagens contêm `BOOTX64.EFI`
`3f0665e6a85825b4821d36f37c2f3d9f7e1f63c37b6fac726158ee66d7133b66`.
Os dois containers de produção têm metadados FAT diferentes, mas o mesmo
kernel e os mesmos cinco payloads de origem. O hash bruto continua
identificando o container concreto.

## Matriz causal de panic

`scripts/test-panic-lock-contention.sh all` retornou 0 em QEMU 8.2.2. O
verificador exigiu o conjunto completo de 39 diretórios: 38 ensaios positivos
e um adversarial. Cada ensaio possui imagem copiada, launch, serial, debugcon,
trace, QMP, GDB, resultado e hashes em
[`panic-final-current-candidate`](../../artifacts/build/panic-safety/20260905-150503-0300/runtime/panic-final-current-candidate/).

| Cenário | Perfis e repetições | Resultado terminal | Status host |
|---|---|---|---:|
| Panic simples e exceção `ud2` | SMP1/TCG e SMP4/TCG | dump completo, frame/vetor coerente e HALT observado | 0 |
| Console, clock e retenção conjunta | SMP4/TCG; conjunta ×3 em TCG, ×3 em KVM e SMP24/KVM | locks reais adquiridos por peer distinto; owner concluiu dump e HALT | 0 |
| Reentrada de profundidade 2 | SMP4/TCG ×3 e SMP4/KVM ×3 | uma saída mínima, sem novo dump/countdown, seguida de HALT | 0 |
| Reentrada de profundidade 3 | SMP4/TCG ×3, SMP4/KVM ×3 e SMP24/KVM | silêncio após profundidade 2; GDB confirmou profundidade 3, RIP terminal e IF=0 | 0 |
| Fonte constante | SMP4/TCG ×3 e SMP4/KVM ×3 | limite de falta de progresso e HALT | 0 |
| Fonte inválida, regressiva e intermitente | SMP4/TCG | fallback/teto total observados e HALT | 0 |
| HPET congelado e UART sem progresso | SMP4/TCG | amostra virtual estável ou polling limitado; ação terminal progrediu | 0 |
| RESTART e SHUTDOWN | SMP4/TCG | ACTION correspondente e evento QMP guest-originated compatível | 0 |
| Imagem normal | SMP1/TCG | boot/IRQ, dump serial e reset externo da produção sem injeção | 0 |
| Espera adversarial por lock | SMP4/TCG | timeout de 4 s, lock real ainda retido, sem fim de dump/ação; `NEGATIVE_DETECTED` | 0 |

Para HALT, duas observações GDB separadas por execução livre confirmam
vCPUs/slots, RIP terminal e IF=0. Pausar a VM ou observar apenas
`query-status=running` não satisfaz o oráculo. RESET/SHUTDOWN exigem o evento
QMP bruto depois do trigger e antes de qualquer cleanup do host.

## Regressão corrente

`scripts/test-foundation-regression.sh all` retornou 0. A evidência final está
em [`foundation-final-after-irq-budget`](../../artifacts/build/panic-safety/20260905-150503-0300/runtime/foundation-final-after-irq-budget/).

| Perfil | Cobertura | Resultado |
|---|---|---|
| Q35 TCG SMP1/2 | boot, slots, IRQ atual, configuração e liveness | PASS |
| PC TCG SMP4 | boot, slots, IRQ atual, configuração e liveness | PASS |
| Q35 KVM SMP4 | série completa, consumidores, TASKMAN e controle de rate | PASS; rate 999, faixa 998–1002 |
| Q35 KVM SMP8 | boot, oito snapshots, IRQ, configuração e liveness | PASS |
| Q35 KVM SMP24 | série completa, 24 snapshots, consumidores, TASKMAN e soak | PASS com observação de cadência não autoritativa |

No SMP24, o resultado bruto foi FAIL, `worst_median_x1000=576`, faixa
445–855, com status 1 dentro da transação. O host tinha 12 CPUs schedulable
para 24 vCPUs. Os limites 700/1300, cinco rounds, janela de 2000 ms e período
LAPIC de 1000 us permaneceram inalterados. O controle KVM SMP4 tinha capacidade
suficiente e passou; por isso somente a autoridade da cadência SMP24 é
classificada como `NONAUTHORITATIVE_OVERSUBSCRIBED`. Nenhum resultado funcional
recebe essa exceção.

O soak SMP24 durou 376 s e registrou checkpoints aos 76, 150, 226, 301 e
376 s. Cada checkpoint executou novas transações de liveness, `irq check` e
`synctest check`, sequências 29–43, todas com status 0. TASKMAN abriu após
`clear`, permaneceu 2,5 s, gerou screendump real, saiu por ESC e terminou sem
resíduos de modal/workspace/render.

Os controles de produção usaram o ELF `13fb98d5...1b81`: smoke SMP1/TCG,
série e consumidores SMP4/KVM, `irq boot` SMP8/24, timer-reference SMP4/24 e
rate SMP4/24. O retorno real foi observado em GDB; o rate SMP24 preservou
status 1 e todos os outros diagnósticos exigidos retornaram 0. A política de
produção registrou `autorun=0 tasktest=0 manual_diagnostics=1`.

## Builds, higiene e tentativas preservadas

| Gate | Retorno bruto | Classificação |
|---|---:|---|
| `make kernel-check` | 0 | PASS |
| `make stack-check` | 0 | PASS; máximo global 1808 bytes, zero violações |
| `scripts/test-panic-lock-contention.sh all` | 0 | PASS |
| `scripts/test-foundation-regression.sh all` | 0 | `PASS_WITH_NONAUTHORITATIVE_RATE_OBSERVATION` |
| imagem normal de produção | 0 | PASS; sem macros ou símbolos de injeção |
| `scripts/test-interrupt-bringup.sh all` histórico | 1 | `PREFLIGHT_ONLY` / `HARNESS_BASELINE_MISMATCH` |
| `scripts/test-timer-clockevent.sh all` histórico | 1 | `PREFLIGHT_ONLY` / `HARNESS_BASELINE_MISMATCH` |
| `scripts/test-taskman-visual.sh focused` histórico | 1 | `PREFLIGHT_ONLY` / `HARNESS_BASELINE_MISMATCH` |

Os harnesses históricos continuam intactos e seus retornos auditados não foram
promovidos a execuções novas. Os pins e contratos antigos permanecem
incompatíveis com HPET somente clocksource, Timer0 quiescente e BSP LAPIC como
único clockevent global.

As primeiras execuções deste fechamento foram preservadas. Elas incluem erro
do matcher de prontidão A, tentativas anteriores dos oráculos, erro de quoting
do screendump, observação GDB que reencontrava o breakpoint de entrada,
TASKMAN observado antes do primeiro frame completo, uma amostra SMP24 de
configuração LAPIC com status 1, e o `irq check` status 1 no segundo checkpoint
da execução imediatamente anterior. Esta última linha tinha contrato global
coerente, mas a validação repetida não concluiu; ela motivou o prazo único de
amostragem. Nenhum bruto foi alterado ou escolhido retroativamente.

No ELF normal não existem `HOBBYOS_PANIC_TEST`,
`HOBBYOS_PANIC_NEGATIVE_LOCK_WAIT`,
`HOBBYOS_REAPTEST_NEGATIVE_NO_TIMER_RELEASE`, nem símbolos, hooks, faults ou
ponteiros de locks introduzidos por essas opções. O ELF não possui referências
`__atomic_*`/libatomic indefinidas.
O panic normal continua sem dependência alcançável de console, graphics,
clock, HPET, power ou spinlock. Os arquivos visuais e demais arquivos
protegidos coincidem com a base.

## Limites e validação física

Esta validação é virtual. Conforme a
[política de validação](validation-policy.md), o fechamento físico ainda deve
exercitar panic, halt, reset e desligamento no candidato final e completar o
[baseline preservado](baseline.md). Continua existindo uma amostra física de
3267 ms, sem mediana e sem associação operacional confirmada ao candidato.
Amostras do kernel inicial e final não podem ser misturadas.

## Estado técnico desta entrega

| Item | Estado |
|---|---|
| Implementação de panic | preservada, hashes idênticos ao checkpoint |
| Protocolo de owner, peers e reentradas | PASS |
| Dependências de lock, dump, countdown e UART | PASS |
| Halt, restart e shutdown observados externamente | PASS |
| Oráculo e controle adversarial de panic | PASS |
| Transporte e retornos exatos dos handlers | PASS |
| Snapshots de IRQ | PASS |
| Referência de timer por alvo | PASS |
| Regressão corrente | PASS_WITH_NONAUTHORITATIVE_RATE_OBSERVATION |
| Build, stack, produção e smoke normal | PASS |
| Arquivos protegidos | UNCHANGED |
| Validação física | DEFERRED_TO_FINAL_CLOSURE |
