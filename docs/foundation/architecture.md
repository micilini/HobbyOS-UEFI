# Contratos de acesso e descoberta da arquitetura

## Escopo

Esta entrega define contratos para MMIO, CPUID, MSR e portas de I/O sem mudar
drivers, timers, topologia, scheduler ou alocadores. HPET continua somente como
clocksource, Timer0 permanece quiescente, o LAPIC do BSP continua sendo o
clockevent global e o período continua em 1 ms. A nova tabela de recuperação
integra o layout de seções já validado; ela não instala RO, NX ou W^X.

O inventário encontrou os acessores MMIO em `kernel/src/cpu/mmio.h`, um trace
global de melhor esforço consumido pelo panic, as rotinas CPUID/MSR e três
declarações externas de portas em `cpu.c`/`cpu.h`. `core/io.h` já continha a
interface inline completa. Os consumidores foram auditados sem migração para
as novas variantes relaxed. Em particular, os fences explícitos de `xhci.c`
permanecem byte-idênticos.

## MMIO ordered e relaxed

Cada accessor executa exatamente um acesso `volatile` da largura solicitada.
As rotinas existentes conservam o contrato forte: um `MFENCE` com clobber de
memória do compilador aparece antes e depois do acesso. Assim, operações de
memória executadas pela CPU antes e depois ficam ordenadas em relação ao
acesso MMIO. O clobber impede somente reordenação pelo compilador; o `MFENCE`
fornece a fronteira arquitetural x86.

As variantes `mmio_read*_relaxed` e `mmio_write*_relaxed` executam o acesso
volátil sem clobber de memória nem fence. Nenhum consumidor de produto foi
migrado nesta entrega. Em ambos os contratos, o chamador deve fornecer uma
região viva, mapeada com tipo de memória adequado e naturalmente alinhada. As
rotinas não escolhem PAT/MTRR, não validam lifetime e não confirmam que uma
escrita posted chegou ao dispositivo. Essa conclusão depende do protocolo do
dispositivo, frequentemente por readback de um registrador especificado.

O probe hospedado compila o header real isoladamente com otimização apenas
para inspeção. O disassembly exige dois `mfence` e um acesso no probe ordered,
e zero `mfence` com um acesso no relaxed. Ele usa RAM controlada e demonstra
geração de código e limites de escrita; não demonstra ordenação física de
PCIe, DMA ou atributos de cache.

## Trace por CPU e panic

Com `HOBBYOS_DEBUG_ASSERT=1`, existe um único conjunto de registros em
`mmio.c`, um por slot publicado da topologia. A seleção usa o identificador
arquitetural lido por CPUID e exige uma correspondência única na topologia;
APIC ID cru nunca indexa o array e identidade ainda indisponível não é
atribuída ao slot zero. O caminho não adquire locks, não consulta scheduler e
não usa MMIO para descobrir a própria CPU.

Um escritor não bloqueante por slot publica primeiro o estado de tentativa e,
quando o acesso retorna, o estado concluído. Leituras pendentes mantêm o valor
inválido; uma operação que faultar não reutiliza o valor anterior. Um contador
de sequência delimita o snapshot e stores atômicos publicam seus campos. Uma
reentrada no mesmo slot não espera pelo escritor interrompido e não o
sobrescreve. O leitor recebe um registro coerente ou indisponibilidade depois
do orçamento fornecido, inclusive se encontrar uma versão ímpar deixada pelo
contexto interrompido.

O panic captura esse snapshot antes das consultas posteriores de identidade e
mantém o protocolo de ownership, reentrada e ação terminal anterior. Ele não
adquire lock do trace. O diagnóstico explicita CPU/slot, validade, endereço,
operação, largura, fase e validade do valor. O registro é o último acesso
observado, não uma atribuição automática de causa. Com asserts desligados, os
callsites não calculam identidade nem publicam trace, e o panic informa
`trace=disabled`.

## CPUID por domínio e subleaf

`cpu_get_cpuid_count(leaf, subleaf, ...)` fornece ECX explicitamente à
instrução. Saídas são opcionais e sempre ficam zeradas quando a consulta é
recusada. `cpu_get_cpuid` continua com sua assinatura anterior e chama a API
nova com subleaf zero, sem herdar ECX residual.

Os máximos dos domínios básico, hipervisor e estendido são consultados por um
helper bruto e validados separadamente. Folhas de hipervisor só são aceitas
quando o bit de presença existe e o máximo retornado pertence ao intervalo do
domínio. O intervalo reservado entre hipervisor e estendido é recusado.

Subleaves são aceitas somente para protocolos conhecidos: tipo de cache para
4 e `0x8000001d`, máximo declarado para 7, 14, 17 e 18, terminador EBX para B
e 1F, e bitmaps XCR0/XSS para D. Nas demais folhas, a API aceita somente
subleaf zero. Essa escolha conservadora evita inventar um máximo universal;
uma folha futura com enumeração própria precisa de uma regra explícita.

O selftest compara a API com CPUID bruto independente, verifica limites e
compara leaf 4 subleaves 0 e 1 no modelo TCG `Haswell`, que expõe ambas.

## Acesso seguro a MSR

As APIs rápidas existentes permanecem para consumidores previamente
validados. `cpu_read_msr_safe` recebe destino não nulo, deixa seu valor intacto
em falha e só publica EDX:EAX quando RDMSR conclui. `cpu_write_msr_safe`
retorna sucesso somente depois de WRMSR e possui clobber de memória. O termo
*safe* limita-se a recuperar #GP da instrução protegida; ele não autentica o
ponteiro de saída nem torna uma escrita semanticamente segura.

Cada site e seu destino formam uma entrada imutável de 16 bytes em
`.cpu_msr_fixup`. O linker publica limites exclusivos e mantém a tabela em um
segmento carregável somente leitura, entre `.rodata` e a fronteira de página
de `.data`. O handler de #GP considera somente contexto de kernel, error code
zero e RIP exatamente igual a um site. Antes de devolver um destino, ele
valida alinhamento, tamanho, limite finito de entradas, endereços em `.text` e
duplicidade de todos os sites. Nenhuma flag global de recuperação é usada.

O teste lê TSC, salva/altera/confere/restaura FS base e preserva IF. Em KVM com
`ignore_msrs=N`, GDB observa #GP exato tanto no RDMSR inválido quanto no WRMSR
inválido, seguido do destino cadastrado e da continuação. A versão instalada
do QEMU TCG trata RDMSR desconhecido como zero e segue a execução; esse perfil
registra `rdmsr_limited=1` e não o apresenta como exceção recuperada. No mesmo
TCG, uma escrita inválida em PKRS produz o #GP protegido. Uma VM negativa
executa WRMSR no site nomeado fora da tabela; GDB observa aquele RIP, o #GP
segue para o panic normal e o guest não retorna.

Como a interface remota QEMU TCG/GDB pode notificar duas vezes o mesmo
hardware breakpoint sem uma segunda entrada no guest, o negativo mantém uma
observação causal adicional no próprio handler. Sob `HOBBYOS_ARCH_TEST`, depois
de recusar o fixup e antes do panic, o kernel registra contagem, RIP, error code
e CS do #GP não tratado. O contrato exige contagem exatamente um e contexto
exato, junto de um único frame serial e dois snapshots externos do halt. Todas
as notificações GDB continuam preservadas e precisam apontar para esse mesmo
contexto; elas não definem mais, sozinhas, a cardinalidade de entradas do
kernel. O registro inexiste fora de `HOBBYOS_ARCH_TEST` e, portanto, não
acrescenta estado nem stores ao candidato de produção.

## Portas de I/O

`core/io.h` é a interface canônica do kernel para `inb/outb`, `inw/outw`,
`inl/outl` e `io_wait`. `cpu.h` a inclui para preservar consumidores antigos;
as declarações externas conflitantes e as definições de `cpu.c` foram
retiradas. Translation units hospedadas incluem ambos os headers nas duas
ordens e repetidamente. Os testes apenas compilam e inspecionam os opcodes;
IN/OUT privilegiado não é executado no host. Definições privadas em drivers
protegidos e a interface independente do bootloader não foram alteradas.

## Gate e evidência

`scripts/test-arch-contract.sh` oferece `host`, `build`, `runtime`,
`production`, `fixtures`, `verify` e `all`. O replay
`python3 scripts/verify-arch-contract.py all <evidence-root>` não compila nem
inicia VMs. O verificador cruza fontes, candidatos, perfis, seriais completos,
observações GDB/QMP, tabela real, seções, símbolos, disassembly e cleanup.

A matriz focal usa Q35/TCG/SMP1, Q35/TCG/SMP4, Q35/KVM/SMP4 e
Q35/KVM/SMP24. Cada CPU publica um padrão distinto em RAM privada; o debugger
lê todos os registros enquanto os escritores permanecem em rendezvous e só
então os libera. Uma VM TCG/SMP1 separada comprova que #GP fora da tabela
continua fatal. A produção Q35/KVM/SMP4 chega a runtime-ready sem estado,
markers ou símbolos de trace/teste.

Os controles negativos cobrem subleaf ignorada, leaf acima do máximo, ECX
residual, MSR alegado sem instrução, #GP alheio recuperado, tabela duplicada ou
fora de `.text`, snapshot misturado, espera sem limite, trace em produção,
marker fragmentado, falta de continuação, headers conflitantes e órfã aceita.
O gate de layout acrescenta controles binários para limite da tabela, site
duplicado, destino fora de `.text` e opcode incorreto.

Os recibos textuais da campanha final são preservados em
[`artifacts/build/arch-contract/20260906-170840-0300/text`](../../artifacts/build/arch-contract/20260906-170840-0300/text/).
Os ELFs, imagens e artefatos binários completos permanecem localmente e são
identificados por hash nos recibos, mas não integram o pacote leve de chat.

## Resultados da validação

A verificação de entrada reconciliou localmente o fechamento dos registros de
asserção com o commit de entrada, preservou o caso histórico intercalado como
rejeitado e aceitou a matriz nova de 25 execuções. A revisão independente pelo
chat permaneceu indisponível por falha da ferramenta de leitura; nenhum teste
ou parecer foi atribuído ao revisor nessa condição.

O gate de arquitetura terminou com os quatro perfis planejados. O candidato
instrumentado tem SHA-256
`a515c1f421dd05c8c334338b56d4bf7835cffd349262e2f9116836371f79e84f`.
Nos perfis KVM, GDB observou os #GP nos dois sites exatos da tabela e a
continuação nos respectivos fixups. O perfil SMP24 publicou 24 registros
distintos, com máscara `0x00ffffff`, enquanto os escritores estavam no
rendezvous. O negativo separado observou o #GP fora da tabela, o panic normal
e ausência de retorno. O controle de produção chegou a runtime-ready sem
símbolos, estado ou markers do trace e dos testes.

| Gate | Resultado observado |
| --- | --- |
| Arquitetura | PASS; quatro perfis positivos, um #GP alheio fatal e 14 negativos rejeitados |
| Layout | PASS; dois pares, cinco candidatos, 2613 seções de entrada, órfã rejeitada e 19 negativos rejeitados |
| Asserções | PASS; 25 execuções, quatro perfis e dez cenários |
| Formatação | PASS; três perfis host, dois guests com 22 casos, cinco séries modal/heap e duas geometrias visuais |
| Panic | PASS; 38 positivos e um adversarial detectado |
| Regressão corrente | PASS; seis perfis, produção e negativo de referência de timer verificados separadamente |
| Registros focais | PASS; STATS com 52 campos e registros de asserção completos; históricos divididos continuam rejeitados |
| Build | `kernel-check`, `stack-check` e `production-image` com retorno zero; maior frame automático 1808 bytes |

O layout final aceitou os papéis `assertions-debug`, `formatting-test`,
`panic-instrumented`, `foundation-framed` e `production-debug-off`. Nos dois
pares, o trampoline permaneceu com 376 bytes, SHA-256
`bc95935ba23b30aff63cfd16c28d4c65dc13fd8fbdf9e8f099671845305c1189`
e offsets de patch idênticos. A comparação com o script histórico fornece
`_text_start` e `_text_end` apenas na linha de link da auditoria, calculados
por `ADDR(.text)` e `SIZEOF(.text)`, porque o objeto atual valida os destinos
da tabela e o script anterior ainda não publicava esses símbolos. Isso não
altera o script histórico, os objetos comparados ou a linkagem canônica.

Duas tentativas de coleta estática foram preservadas antes do resultado final:
uma recusou um diretório fora do repositório e outra expôs a ausência daqueles
símbolos no script histórico. Nenhuma iniciou VM ou indicou falha do ELF. A
coleta final executou no diretório ignorado exigido pelo coletor.

O ELF de produção final tem SHA-256
`8135226256ab0ba54cf5c9c5b4745419c2aa6deb40b746259f0624a730a27c73`.
Três imagens FAT recriadas têm metadados de contêiner distintos, mas os cinco
payloads extraídos são byte-idênticos: bootloader, kernel, fonte, logo e
`startup.nsh`. O trace de MMIO e a injeção não estão no perfil de produção;
as APIs de produto e as fences permanecem.

Na regressão, o controle KVM/SMP4 produziu PASS bruto de cadência com
`worst_median_x1000=996`. O perfil KVM/SMP24 concluiu 43 transações e cinco
checkpoints em 389 segundos, mas preservou FAIL bruto com
`worst_median_x1000=242`. A autoridade permanece não autoritativa porque o
host oferecia 12 CPUs schedulable para 24 vCPUs; nenhuma falha funcional foi
dispensada por essa classificação.

Todos os dez replays offline finais retornaram zero: arquitetura, layout,
asserções, formatação, regressão geral, produção, negativo de timer, panic,
STATS e registros de asserção. As VMs da campanha e a VM manual de diagnóstico
foram encerradas pelos monitores locais, com término dos processos confirmado.

## Limites

O trace não diagnostica acesso anterior à publicação confiável da topologia,
não promete segurança de NMI universal e não prova causalidade do último
acesso. A API CPUID cobre protocolos de subleaf enumerados explicitamente e
não certifica suporte geral a AMD. A recuperação trata apenas #GP dos dois
sites cadastrados. Nenhum driver foi migrado para relaxed, nenhuma permissão
de página foi instalada e nenhuma mídia física foi exercitada. A validação
física continua diferida para o fechamento final.

## Revalidação da atribuição e das provas

Uma revisão externa posterior abriu os fontes e a evidência textual, refez os
testes hospedados e encontrou três lacunas: a CPU não permanecia pinada entre a
identificação e o acesso MMIO, dois loops do observador deixavam o contador
unsigned fazer underflow antes da classificação de timeout, e o verificador
aceitava campos derivados sem reconciliá-los com GDB/QMP. Essa revisão não fez
novos boots nem replay dos binários omitidos do pacote leve.

No perfil com trace, `mmio_trace_begin` agora executa `irq_save` antes da
primeira coleta de identidade. O token conserva as flags e a atribuição até
`mmio_trace_complete`, que publica a conclusão e restaura exatamente o IF do
chamador. Um token sem writer conserva somente a responsabilidade de restaurar
seu próprio pin; uma reentrada não libera o pin externo. O snapshot corrente
também fica pinado durante resolução e leitura. Produção elimina esse caminho
por `#if HOBBYOS_DEBUG_ASSERT` e mantém o acesso e as fences do produto.

A resolução conserva um cache limitado de identificador arquitetural completo
para slot. As entradas usam sondagem aberta; uma colisão procura outra entrada
e nunca sobrescreve uma identidade diferente. O teste hospedado usa
deliberadamente os IDs 11 e 18, que têm o mesmo índice inicial, enquanto dois
writers publicam e um leitor observa progresso. A primeira versão do cache
substituía uma entrada na colisão e foi preservada apenas como tentativa
rejeitada. A matriz hospedada final passou com 70 checks em cada perfil normal,
UBSan e ASan com trace, e 19 no perfil sem trace.

As duas esperas do observador agora consomem um budget sem pós-decremento e
decidem pelo estado realmente observado. Os testes cobrem zero, um, sucesso na
última observação, condição inicialmente satisfeita e expiração. No guest,
o ensaio em task confirmou IF `1 -> 0 -> 1`, CPU e slot estáveis durante a
operação, e snapshots com IF inicialmente ligado e desligado. Em KVM/SMP4,
foram aceitas 1024 amostras enquanto os quatro writers progrediram; as
sequências do slot zero, por exemplo, avançaram de 8764 a 10812. O rendezvous
inicial permanece somente como controle de identidade estável.

O controlador persiste sete registros CPUID e quatro registros MSR por
execução. Em KVM/SMP4, leaf 4 retornou EAX `0x0c000121` na subleaf 0 e
`0x0c000122` na subleaf 1; API e CPUID bruto coincidiram. A leaf básica máxima
foi `0x20`, a estendida `0x80000008`, e as consultas imediatamente superiores
foram recusadas com saídas zeradas. O wrapper de leaf 7 coincidiu com a consulta
bruta de subleaf zero.

No mesmo perfil, a leitura segura de TSC retornou sucesso e `0x9cc20709`; o
RDMSR recusado no índice `0x570` preservou a sentinela
`0x9e3779b97f4a7c15`; o WRMSR recusado no índice `0x6e1` devolveu falha. FS base
foi salvo como zero, alterado para um, relido e restaurado para zero. GDB
observou #GP com error code zero e CS `0x8` nos sites
`0xffffffff8201f2a8` e `0xffffffff8201f31b`, seguido dos probes depois dos
fixups `0xffffffff8201f2b2` e `0xffffffff8201f325`. Em TCG, o RDMSR
desconhecido `0xffffffff` continuou retornando zero sem #GP; esse resultado
permanece limitação do modelo, enquanto o WRMSR recusado forneceu a prova
causal naquele perfil.

O verificador final reabre serial, launch, GDB e QMP, confere tamanho/hash e
deriva sites de fault, fixups, continuidade, células, slots, CPUs, progresso,
budgets e valores CPUID/MSR. Além dos 14 negativos do resumo, 32 mutações de
evidência exercitam o caminho completo; quatro controles válidos passam. Entre
as rejeições estão endereço zero, IDs permutados, bruto ausente ou
contraditório, site/CS/error code/fixup divergentes, CPUID ou FS base
inconsistentes, uma segunda entrada #GP registrada pelo kernel, writers
congelados rotulados como concorrentes e trace ativo em produção.

O gate de arquitetura final passou em Q35/TCG/SMP1, Q35/TCG/SMP4,
Q35/KVM/SMP4 e Q35/KVM/SMP24, no #GP alheio fatal e na produção. O ELF
instrumentado tem SHA-256
`14bb61d71fea8930fa6f8910a227894df700791b9b6f666bbd8bedf7b10458ae`;
o ELF de produção tem
`f312c85e73027a58df2025d4ed293e7eb032a3268bfc01077cce12dd0e1f25c2`.
A análise de layout desses dois candidatos, `kernel-check` e `stack-check`
passaram; o maior frame continuou em 1808 bytes. A matriz de asserções
recompilada passou 25 de 25 casos no candidato novo.

A regressão focal que motivou o cache aceitou KVM/SMP4, SMP8 e SMP24. No
SMP24, `irq boot` passou com 24 snapshots em 2896 ms; o controle SMP4 teve
`worst_median_x1000=994`, e o SMP24 preservou FAIL bruto
`worst_median_x1000=239` como observação sem autoridade por oversubscription.

A campanha ampliada não foi promovida a PASS. Na terceira série modal/heap
SMP8, o comando `taskmantest heap-end` terminou com status 1: o uso passou de
1030704 para 1030768 bytes, com 86 blocos alocados antes e depois. As quatro
séries anteriores passaram; a série visual ainda não havia iniciado. Depois,
o gate de panic aceitou 29 casos e rejeitou
`intermittent-clock-tcg-smp4`: o countdown ainda executava, três das quatro
CPUs estavam no halt terminal e o owner não estava. Os brutos de ambas as
falhas foram preservados sem retry. Heap, panic e seus gates estão fora do
escopo desta correção, portanto a regressão completa permanece bloqueada em
vez de ser enfraquecida ou alterada nesta entrega.

As tentativas anteriores e esta campanha ficam em arquivos duráveis sob
[`artifacts/build/arch-contract/20260906-194056-0300/durable`](../../artifacts/build/arch-contract/20260906-194056-0300/durable/).
O pacote leve inclui os fontes e os brutos textuais focais; ELFs, imagens e
snapshots binários continuam somente no armazenamento local. A validação
física permanece diferida.

## Fechamento causal das regressões

A investigação posterior preservou as duas falhas acima e separou o estado do
guest, o resultado do coletor, a decisão do verificador e a completude da
campanha. A revisão independente da entrega que iniciou esta investigação não
conseguiu abrir o pacote; os resultados desta seção são observações locais e
não são atribuídos ao revisor.

### Checkpoints de heap modal

No bruto preservado de `q35-kvm-smp8-run3`, a transação 8 terminou com
`taskdiag check` em status zero, `refs_current=1` e `free_inflight=0`. A
transação seguinte executou `taskmantest heap-end` e o próprio handler
devolveu status 1: `used_bytes` passou de 1030704 para 1030768, enquanto a
contagem de blocos alocados permaneceu em 86. O serial original tem SHA-256
`caa05294db7e56d5f4f182a958f8963c5fd5bd659bd0dc23d41594bf36ad021e`.
O coletor parou nessa falha real do comando; por isso não produziu o resultado
agregado de formatação nem iniciou a série visual.

Duas execuções diagnósticas previamente limitadas usaram Q35/KVM/SMP8, o
mesmo ELF de SHA-256
`659b278b5a39fc60b67e92ec4968c1deb77a1a489f4cd54f3e2ec9b2ab71d6e4` e a
sequência canônica de dez comandos. Ambas terminaram o `heap-end` com drift
zero. A segunda fez exatamente doze leituras GDB somente leitura depois do
baseline e doze antes do endpoint final. Todas registraram 1030768 bytes e 86
blocos alocados, mas os endereços dos nós de timer e a forma da lista de blocos
mudaram entre amostras. A primeira execução também mostrou um bloco alocado de
96 bytes mudar de endereço e um novo fragmento livre de 32 bytes apesar dos
totais iguais. O primeiro runner terminou com erro apenas no seu bookkeeping
de cleanup, depois dos dez comandos completos; a segunda execução confirmou o
cleanup.

O predicado `heap_snapshot_quiescent()` observa o heap global, mas cobre apenas
o reaper: exige zombies e liberações em zero, uma referência corrente e três
amostras iguais ou menores. Ele não cobre outros usuários assíncronos do mesmo
alocador. `timers.c` aloca nós de 96 bytes e `dpc.c` aloca jobs de 32 bytes fora
desse predicado. As execuções diagnósticas comprovam churn desses objetos, mas
não identificam a alocação histórica de 64 bytes, pois aquela VM não teve a
lista do heap capturada.

Assim, o drift original não foi reproduzido na série limitada e não pode ser
classificado como vazamento. Os endpoints do oráculo atual também não estão
demonstrados como comparáveis. O requisito de drift exatamente zero permanece;
não foi acrescentada tolerância, subtração ou retry. Fechar esse ponto requer
autorização focal para tornar o oráculo de `cmd_taskmantest.c` sensível ao
lifetime medido, ou para expor uma condição limitada de quiescência dos demais
usuários do heap. Nenhum fonte de produto foi alterado nesta investigação.

### Countdown intermitente em TCG

O relógio injetado avança um tick a cada 1024 amostras. O alvo de um segundo a
1000000 Hz não pode ser alcançado dentro do limite de 50000000 polls, que
permite no máximo 48828 ticks completos; a saída contratual é, portanto,
`iteration-limit`. Na execução histórica, o serial chegou a
`state=start ... poll_limit=50000000`. Depois do limite de parede de 25
segundos do coletor, duas observações GDB separadas por 0,487 segundo viram o
contador avançar de `0x2039ed7` para `0x20e250d`. Três peers já estavam no
halt terminal; o owner continuava em `panic_countdown`. Não houve evidência de
deadlock nem de falha da ação, porque a ação ainda não tinha sido alcançada.

Uma execução diagnóstica planejada do mesmo ELF e perfil recebeu uma janela de
observação de 60 segundos. Em 22,31 segundos totais ela publicou
`state=iteration-limit polls=50000000`, anunciou `ACTION` para halt e mostrou
as quatro CPUs no halt terminal, com IF limpo, em duas observações. Isso
demonstra que o limite anterior do coletor cortava progresso finito do guest.

O runner agora concede 60 segundos aos casos TCG comuns. Ele ainda termina
cedo quando observa o evento terminal, e o controle adversarial conserva seu
orçamento separado de quatro segundos. O gate completo, usando o verificador
anterior sem relaxamento, aceitou os 38 casos positivos e detectou o
adversarial. O ELF instrumentado permaneceu em
`cc66e644d3e0314c3944b4d7755c3e868ed162c726cedce7737ce601bcc0d882` e o de
produção em
`f312c85e73027a58df2025d4ed293e7eb032a3268bfc01077cce12dd0e1f25c2`.

Os resumos derivados, hashes dos brutos e o mapa de cobertura ficam em
`artifacts/build/runtime-regression/20260906-224332--0300/`. O gate de
arquitetura e a matriz de 25 asserções foram reabertos somente por replay
offline e passaram. A formatação original continua recusada, e a regressão
corrente completa não foi executada, porque a comparabilidade do endpoint de
heap permanece sem fechamento. A validação agregada, portanto, continua
bloqueada sem desfazer a correção do trace ou enfraquecer os gates.

## Checkpoints comparáveis do consumidor modal

A ocorrência histórica de 64 bytes permanece intacta e recusada. Seu serial
continua com SHA-256
`caa05294db7e56d5f4f182a958f8963c5fd5bd659bd0dc23d41594bf36ad021e`,
o uso passou de 1030704 para 1030768 bytes e os dois snapshots declararam 86
blocos alocados. Não existe captura histórica capaz de identificar o objeto;
por isso, a identidade da alocação continua `NOT_DETERMINED`, e o resultado
não foi reclassificado como vazamento nem como sucesso.

O consumidor modal agora delimita o ensaio com quatro comandos framed:
`memory-checkpoint-begin`, `memory-checkpoint-track`,
`memory-checkpoint-progress` e `memory-checkpoint-end`. O primeiro verifica que
não há worker `smpstress` anterior, sessão/modal ativo, contexto modal vivo ou
modelo de TASKMAN retido. O segundo captura cada worker controlado pelo teste
como `task_handle_t`, incluindo ID e geração, junto de seus contadores de
schedule, runtime e estimativa de memória. O terceiro exige progresso de
schedule e runtime nos mesmos handles enquanto a carga e as operações modais
estão ativas.

O endpoint final exige que os mesmos handles tenham desaparecido e que
`free_inflight` tenha chegado a zero depois da retirada do registro da task.
Isso fecha a janela em que o reaper já tornou o handle invisível, mas ainda
libera a alocação fora do lock. O endpoint também cruza os deltas exatos de
criação, conclusão, cleanup e sinalização das sessões modais, exige contextos
e quarentena zerados, rota padrão, shell não pausada e modelo de TASKMAN
liberado. As esperas têm limite de 30000 ms, sem underflow; expiração,
retenção e drenagem pendente produzem falha distinta e retorno não zero.

O oráculo autoritativo tem escopo `TARGET_IDENTITIES`. O snapshot global do
heap permanece no registro com seus valores e delta reais, sempre marcado
`global_comparable=0`. Ele serve como observação de consistência e não como
prova de ausência de vazamento do kernel inteiro, pois timers, DPCs e outros
usuários independentes continuam ativos. O gate não aplica tolerância de 64
bytes nem permite que uma liberação externa compense um recurso identificado
retido.

Os registros usam armazenamento estático privado do handler, com capacidade
de 1536 bytes e canários adjacentes. Cada registro é formatado por inteiro antes
de uma chamada a `serial_write_all`; a LF inicial pertence à mesma mensagem e
impede que texto parcial de um produtor anterior receba o prefixo do
checkpoint. Falha de capacidade ou dos canários publica somente
`MEMORY_CHECKPOINT_ERROR` e impede a continuidade do protocolo. O maior frame
automático medido no caminho final foi 1904 bytes, abaixo do limite de 2048.

O teste hospedado compila o handler real e a libc real, substituindo somente
serviços indisponíveis no host. Normal, UBSan e ASan passaram, cada um com 12
casos e 73 verificações. Os controles cobrem budget zero e sucesso no limite,
geração incorreta, ausência de progresso, erro e cleanup, retenção deliberada
de 64 bytes, compensação líquida zero por outro recurso, drenagem assíncrona e
capacidade máxima de 32 workers. O verificador de formatação aceitou o controle
e rejeitou 18 fixtures, dez delas específicas dos checkpoints.

A campanha final executou exatamente duas VMs Q35/KVM/SMP4 e três
Q35/KVM/SMP8. Todos os workers capturados progrediram e foram liberados; os
cinco endpoints ficaram `ESTABLISHED`, com cleanup completo. Os deltas globais
observados foram zero em todas as cinco execuções, mas continuam classificados
como não comparáveis para uma afirmação global. O gate de formatação passou
com a cobertura modal, heap, WIDE e COMPACT no candidato correspondente.

Na mesma árvore, o gate de arquitetura passou nos quatro perfis, no #GP alheio
e na produção, e a matriz de asserções passou 25 de 25 casos. `kernel-check`,
`stack-check` e `production-image` também passaram. O fechamento agregado não
foi promovido: a regressão corrente parou no `irq boot` de Q35/KVM/SMP24 com
timeout do snapshot do slot 12 (`entered=15796`, `returned=15795`, `depth=1`),
antes do soak; uma execução posterior do gate de panic parou antes de armar o
cenário `joint-lock-kvm-smp24`, por timeout de prontidão durante a varredura
PCI. Esses eventos pertencem a caminhos de produto protegidos nesta correção,
foram preservados sem retry e permanecem bloqueios separados do contrato modal.

As evidências completas desta rodada ficam sob
`artifacts/build/modal-checkpoints/20260907-010254--0300/` em armazenamento
persistente. O pacote de revisão contém fontes e evidência textual focal; os
ELFs, imagens e arquivos compactados das campanhas permanecem locais. A
validação física continua diferida para o fechamento final.

## Coleta de snapshots IRQ e prontidão antes do panic

As falhas de `irq boot` preservadas pertencem a execuções distintas. A
campanha modal registrou primeiro o slot 12 com `entered=15796`,
`returned=15795` e `depth=1`. A execução final daquela campanha, usada como
entrada desta correção, registrou o slot 17 com `entered=18573`,
`returned=18572`, `depth=1`, 29 polls e 5195 ms. Nenhuma amostra foi aceita
como quiescente, e nenhuma delas demonstra por si só que a CPU ficou travada.
Os brutos e os candidatos permanecem separados.

O handler anterior iniciava um deadline comum de 5000 ms e, dentro do mesmo
laço, publicava cada `BOOT_CPU` antes de tentar o slot seguinte. Um probe do
handler real mostrou que 300 ms de custo simulado por linha consumia todo o
deadline antes da segunda observação de um slot transitório. Sem custo de
saída, o mesmo slot passava na tentativa seguinte. Esse ensaio demonstra a
dependência indevida da aquisição em relação à apresentação; ele não
decompõe retroativamente os 5195 ms da execução histórica.

`irq boot` agora valida a quantidade descoberta de CPUs, aloca antes do
deadline um vetor privado para no máximo `HOBBYOS_MAX_CPUS` e coleta todos os
slots sob um único budget de aquisição de 5000 ms. Somente depois da coleta
começa a publicação. Cada registro é formado em um buffer de 768 bytes e
enviado por uma chamada de serial. O resultado conserva os campos anteriores
e acrescenta `acquisition_ms`, `publication_ms` e `budget_ms`; a publicação
medida compreende os registros anteriores ao próprio `BOOT_RESULT`. A coleção
prova a validade individual de cada slot no instante de sua amostra, não um
snapshot global simultâneo.

Falta de armazenamento, topologia inválida, snapshot indisponível, timeout ou
erro de formatação mantêm retorno não zero e nunca produzem
`BOOT_RESULT PASS`. O armazenamento é liberado depois da publicação e em
todos os caminhos de erro posteriores à alocação. O frame automático de
`irq_boot` ficou em 1344 bytes; o maior frame da árvore medida permaneceu em
1904 bytes.

O teste hospedado compila `cmd_irq.c` e a libc reais, substituindo apenas
snapshot, tempo, heap e saída. Normal, UBSan e ASan passaram com 13 casos e 90
verificações em cada perfil. Entre os controles estão 32 CPUs, saída lenta,
slot sempre em IRQ, topologia e armazenamento inválidos, truncamento,
cleanup e a reprodução isolada da ordem antiga. O verificador aceitou o
controle e rejeitou sete mutações de execução, além de oito controles
negativos do observador de boot.

Duas VMs independentes Q35/KVM/SMP24 executaram o handler corrigido no ELF
`f49be7eb01f70662dd8751bbc90a1ad1ecee5b29750e02b6b32d4a9caa56156f`.
Ambas produziram 24 registros e terminaram o comando com status zero. A
primeira registrou 50 polls, 880 ms de aquisição e 975 ms de publicação; a
segunda registrou 59 polls, 3724 ms e 1785 ms. Os 5509 ms totais da segunda
não excedem o budget de coleta: 3724 ms pertencem à aquisição e a saída
ocorre depois dela.

O timeout histórico de `joint-lock-kvm-smp24` ocorreu antes do trigger, aos
45 segundos de parede. Seu ELF
`b4f607b67197b429ff8396108b0249c6826fa21ab090ff34b2d4c9ac43facc94`
foi observado em duas inicializações independentes com deadline absoluto de
120 segundos. Elas chegaram a `RUNTIME_READY` em 10,299 e 8,686 segundos e
permaneceram em observação até checkpoints de 45,027 e 45,022 segundos, sem
armar panic. `SHUTDOWN guest=false reason=host-qmp-quit` continuou classificado
como cleanup. Esses dois boots mostram que o candidato alcança prontidão
nesse perfil; não transformam o timeout antigo em PASS nem constituem uma
distribuição universal de latência.

O gate de panic aplica 120 segundos de budget absoluto de boot a todos os
casos KVM/SMP24 e preserva budgets separados depois do trigger. A matriz final
passou nos 38 casos positivos e detectou o adversarial, incluindo os casos
KVM/SMP24. Os 60 segundos dos casos TCG comuns e os quatro segundos do
adversarial permanecem inalterados. Nenhum trigger ocorre antes de
`RUNTIME_READY`.

Arquitetura, asserções, formatação e panic passaram nos candidatos atuais; os
replays offline abriram as campanhas duráveis e repetiram os verificadores sem
alterar os brutos. Treze papéis distintos de ELF, incluindo produção,
instrumentação, negativos e regressão framed, passaram individualmente no
verificador de layout. O ELF de produção compartilhado por esses gates tem
SHA-256 `07584735e05e541006a5da52cbddeb03f33d400c15ec29f5f9364dffd1ef30a5`.

A regressão corrente completa ainda não foi certificada. Em duas execuções
limitadas do mesmo ELF framed em KVM/SMP24, o guest rejeitou uma linha antes do
dispatch: `synctest timer-cancel`, sequência 12, com espaçamento de 0,25 s, e
`irq routes`, sequência 3, com espaçamento de 0,50 s. Ambas registraram
`reason=syntax`, mantiveram a sequência esperada, não produziram BEGIN/END e
tiveram cleanup completo. A segunda tentativa refuta que apenas ampliar esse
intervalo elimine o defeito de transporte; não existe resultado do handler a
classificar. Não houve novo retry, e o soak de cinco checkpoints continua
ausente. O guard de `TEST_READY` foi mantido no runner porque uma tentativa
anterior demonstrou o envio antes da prontidão de teste; ele não torna um
frame rejeitado aceitável.

O audit focal originalmente referenciado pelo wrapper modal existe em
`artifacts/build/modal-checkpoints/20260907-010254--0300/focal/static/source-audit.json`,
com 788 bytes e SHA-256
`4d7468d92aaae11a974e740cd7efb584efe2ded5ece77810269edefe5e3742ef`.
Ele permanece distinto do audit posterior em `staged-check`. Heap, XHCI, IRQ
context, bootstrap, panic e os demais componentes protegidos não foram
alterados para obter estes resultados. A validação física continua diferida.

## Integridade observável do transporte framed

A rejeição histórica de `accounttest lapic-config`, sequência 5, permanece
classificada como falha antes do dispatch. O host pretendia e registrou os 49
bytes ASCII de `tasktest exec 5 2a3ded43 accounttest lapic-config`, enquanto o
guest publicou `actual_crc=cebb6cc7`, sem `ACCEPT`, `BEGIN`, `END` ou retorno de
handler. Essa execução não capturou o buffer consumido pelo parser. Seus bytes
pré-parser permanecem `NOT_OBSERVED`, e a causa histórica não é atribuída ao
emissor, ao QEMU, ao xHCI ou ao guest a partir do checksum isolado.

Um controle hospedado e virtual posterior localizou uma deficiência do harness
reproduzível, sem reclassificar a execução histórica. Quando key-down e key-up
eram enviados em pedidos QMP separados, o ACK do pedido não garantia que o
guest já tivesse consumido a liberação. Sob essa janela, um candidato corrente
recebeu `tasktest exec 66` no lugar do prefixo de sequência 6. O controlador
agora forma o frame uma vez e envia cada caractere por um único
`input-send-event` com down e up. Depois de cada stroke, lê por QMP/HMP os bytes
físicos do buffer da shell e o estado do teclado xHCI calculados do ELF e do
header reais. Ele somente avança quando observa o prefixo exato e todas as
teclas liberadas. Não há retransmissão.

Antes do Enter, o mesmo buffer precisa permanecer igual ao frame completo por
0,5 segundo. Um observador GDB privado confirma a string integral na fronteira
de `shell_dispatch_command_line`; o guest é retomado antes da liberação do
Enter, e tanto a retomada quanto a liberação são verificadas com limites
finitos. Os limites atuais são 5 segundos por caractere, 90 segundos por frame,
60 segundos para alcançar a fronteira de dispatch e 5 segundos para cleanup do
observador. O QMP usa socket privado e seu ledger conserva cada pedido e
resposta sem reconstruir expected values.

No candidato com ELF
`f49be7eb01f70662dd8751bbc90a1ad1ecee5b29750e02b6b32d4a9caa56156f`,
uma VM Q35/KVM/SMP24 observou exatamente os 34 bytes de
`tasktest exec 1 4fa3301b irq check` no buffer físico e no dispatcher. Houve um
`ACCEPT`, um `BEGIN` e um `END`, mas o handler retornou 1 após timeout de
snapshot do slot 18 (`entered=8117`, `returned=8116`, `depth=1`). A série
declarada foi interrompida na primeira execução, sem tentativa substituta. Esse
resultado aprova a correlação de entrada e dispatch e mantém a regressão
agregada bloqueada pelo resultado real do handler.

Uma VM independente do mesmo candidato enviou os 60 bytes exatos de
`tasktest exec 1 00000000 inputtest marker transport-rejected`. O oráculo
independente calculou `d4f1198f`; o guest rejeitou uma vez por CRC e não
publicou `ACCEPT`, `BEGIN`, `END` nem o marker do handler. O primeiro replay
offline dessa captura expôs uma comparação incorreta de um resumo redundante
de estabilidade no verificador. Os brutos não foram modificados; depois da
correção, o replay dos mesmos arquivos aceita o negativo e continua exigindo
rejeição antes do dispatch.

O verificador reabre serial, launch, ledger QMP, observação GDB e mapa de
endereços, confere hashes e deriva os bytes das respostas físicas. Ele
recalcula o mapeamento símbolo-para-físico a partir dos segmentos `PT_LOAD` do
ELF local e distingue integridade da entrada do status do handler. Mutações de
resposta QMP, endereço físico, estado de teclado, bytes no dispatcher,
transação e cleanup são rejeitadas mesmo quando os campos derivados do JSON
continuam favoráveis.

Os testes host passaram em 84 verificações. As fixtures da evidência corrente
rejeitaram 23 mutações, e o conjunto de regressão preservado rejeitou 45
negativos. O transporte corrigido não altera a gramática do envelope, CRC,
parser, shell, input do guest ou qualquer arquivo de produto. A matriz completa
de seis perfis e o soak SMP24 não foram executados porque o primeiro comando do
perfil corrente chegou ao handler e retornou falha. As cinco campanhas válidas
anteriores continuam preservadas como evidência anterior, sem serem
apresentadas como novas execuções. A validação física permanece diferida.

## Fechamento do convoy do relógio e da observação física

O timeout preservado do slot 18 não era uma falha de transporte. O frame foi
observado integralmente no buffer e no dispatcher, e o handler iniciou. Uma
execução diagnóstica do mesmo ELF mostrou 23 das 24 vCPUs em
`spin_lock_irqsave -> clock_monotonic_ns`, a maioria a partir do hard IRQ LAPIC,
enquanto a CPU restante executava a leitura MMIO do HPET. Com 24 vCPUs sobre 12
CPUs schedulable do host, a preempção do proprietário transformava o lock global
em convoy; reduzir CPUs ou alterar o período de 1000 us não foi usado como
correção.

O caminho split-64 de produção agora forma a amostra HPET com a transação
high-low-high existente, mas sem proprietário global. `irq_save` fixa a CPU
durante a identidade, a leitura e a atualização do histórico bruto por CPU. O
valor público é um máximo atômico por compare-exchange; o baseline global é
capturado antes do MMIO para não classificar leituras sobrepostas pela ordem de
conclusão. Estatísticas são atômicas e o ring diagnóstico usa `trylock`; falhar
ao registrar um detalhe no ring não bloqueia hard IRQ nem altera os contadores
agregados. O fallback de extensão de contador de 32 bits conserva sua
serialização própria de wrap e não é confundido com o caminho split-64.

O candidato focal passou Q35/KVM em SMP4, SMP8 e SMP24. No mesmo SMP24,
`accounttest clock-smp 32 1000000` terminou com um milhão de leituras, 9826
migrações e zero regressão local, lag cruzado ou corrupção; onze `irq check`
consecutivos retornaram zero em 4--36 ms, sempre com 24/24 CPUs,
`unexpected=0` e `imbalance=0`. A produção também executou seu primeiro
`irq check` SMP24 com retorno zero.

A primeira campanha integral posterior preservou duas falhas adicionais antes
do Enter. Q35/TCG/SMP2 observou `length=11` junto de `tasktest e\0`; PC/TCG/SMP4
observou `length=24` junto de `tasktest exec 2 f7d2aea\0`. Em ambos os casos o
pedido QMP era único, não houve dispatch e o span físico colocava o buffer antes
de `g_len`/`g_pos`. A VM pôde escrever a tecla entre essas regiões durante um
único `xp`, produzindo uma imagem rasgada: bytes antigos e metadados novos. As
duas amostras continuam classificadas como falha na campanha original.

O schema 26 corrige o oráculo sem repetir entrada. Cada snapshot registra o
span da shell antes e depois da leitura xHCI e só é classificável quando as
duas imagens são byte a byte iguais. Diferença gera `UNSTABLE` e apenas uma nova
leitura dentro do budget existente; divergência estável continua terminal. O
manifest correspondente é schema 17. Os testes host passaram em 91 checks,
incluindo uma reprodução de comprimento novo com NUL antigo, e as 45 fixtures
negativas continuaram rejeitadas.

A campanha nova em `artifacts/build/clock-lockfree-fix/full-fixed` passou os
seis perfis e 95/95 comandos framed: 6 em Q35/TCG/SMP1, 6 em
Q35/TCG/SMP2, 28 em Q35/KVM/SMP4, 6 em Q35/KVM/SMP8, 6 em PC/TCG/SMP4 e 43 em
Q35/KVM/SMP24. O soak completou cinco checkpoints. O ledger contém 620 amostras
`UNSTABLE`, zero `DIVERGED`, zero retorno não nulo nos positivos e nenhum
reenvio. O negativo de timer-reference retornou 1 pelo motivo esperado, e os
quatro perfis de produção passaram.

O ELF selftest certificado tem SHA-256
`c4e4bfce330d93036015716c11e145be43eedc0048d4ecd12aa616d483e089bc` e sua
imagem FAT tem `daa08c63b1d19fd0096240839105a748894dada439f1c869e9a744db27b3b977`.
O ELF de produção tem
`30767f43fe099115b8f402ae1c1ec7335895a9f502bd9425937fb782a6340596` e sua
imagem FAT tem `82637534ae968cb3965d708a3cf9871683b36a12f5ce900d0caac59489f173cf`.
A validação bare-metal permanece separada e diferida.

## Etapa 1 F7: spinlock FIFO com progresso individual

O primitivo ordinário do kernel é agora um ticket lock FIFO. O estado de
32 bits `tickets` mantém o ticket servido nos 16 bits baixos e o próximo
ticket nos 16 bits altos. A emissão usa incremento atômico; o waiter compara
seu ticket por igualdade modular e executa `pause`; a liberação avança apenas
o owner por compare-exchange, preservando concorrentemente o next. Assim, a
volta de 65535 para zero não muda a ordem enquanto o número de waiters vivos
for menor que o espaço do contador. Uma asserção de compilação prova que o
limite de CPUs e contextos de IRQ representável pelo kernel cabe nessa largura.

`spinlock_t` conserva `locked` como estado binário de diagnóstico e acrescenta
o estado de tickets. Os dois campos zerados significam livre, portanto `{0}` e
`.bss` continuam inicializações válidas. `spin_trylock` só faz compare-exchange
quando owner e next coincidem; uma falha não espera e não consome posição.
A aquisição publica semântica acquire, a liberação release e as variantes
irqsave restauram exatamente o bit IF recebido. Aquisição recursiva, inclusive
por NMI interrompendo o dono do mesmo lock, permanece proibida pelo contrato do
chamador.

Somente com `SELFTEST=1`, um observador atômico pode acompanhar um lock alvo e
registrar a distância modular vista no momento da emissão do ticket. O comando
`locktest progress <cpus> <iterations>` cria contenção real, confirma presença
simultânea dos workers, cobertura de todas as CPUs, exclusão, restauração do
IF, contagem causal, drenagem do lock e reap dos handles. Ele não existe no
registro, nas strings nem nos símbolos da imagem de produção.

O gate `scripts/test-lock-progress.sh all` cobre host normal/UBSan, wrap,
`trylock`, runtime Q35/KVM/SMP8, runtime funcional Q35/KVM/SMP24, produção e
fixtures injusta/não exclusiva. O replay é
`python3 scripts/verify-lock-progress.py all <evidence-root>`. Na campanha F7,
SMP8 completou 72.000 aquisições com distância máxima 7/7; SMP24 completou
24.000 com 23/23. Ambos tiveram todas as CPUs participantes e zero violação
funcional. A justiça de SMP8 é autoritativa; SMP24, com 24 vCPUs em host de 12
CPUs schedulable, tem autoridade apenas funcional.

Qspinlock, espera paravirtualizada, adaptação NUMA e detecção geral de
recursão/NMI não fazem parte desta etapa. Consumidores não foram modificados
para acomodar o novo primitivo.

## Etapa 1 F6: rejeição de entradas inválidas nos alocadores

O heap faz aritmética verificada antes de alinhar ou somar overhead. Cada
região acrescentada ao heap é registrada, cada bloco conserva magic e estado,
e o header de uma alocação alinhada liga canário, ponteiro bruto, ponteiro do
usuário e alinhamento. Toda leitura desse header durante `kfree` ocorre sob o
lock e somente depois que endereço e bloco foram provados pertencentes a uma
região conhecida. Ponteiros externos, interiores e double-free são recusados
com warning e contador, sem mutação. A coalescência invalida headers removidos
e o censo de integridade recompõe bytes totais, usados e blocos.

O PMM separa ocupação de reserva em dois bitmaps. Memória não convencional,
o primeiro MiB, o kernel e o armazenamento dos bitmaps permanecem reservados,
mesmo quando ocupados. `pmm_free_frame` valida nulo, alinhamento, span, reserva
e estado antes de mudar o bitmap ou `g_free_frames`.

As operações de range e as buscas do bitmap trabalham em palavras de 64 bits,
com cauda mascarada quando o número de bits não é múltiplo de 64. A escolha
continua first-fit, portanto o frame escolhido é o mesmo da busca por bit.
Contadores de palavras examinadas existem apenas para instrumentação de
debug/selftest.

O gate `scripts/test-alloc-hardening.sh all` compara a implementação real do
bitmap com uma referência hospedada, executa recusas causais no guest, verifica
produção e fixtures negativas e permite replay offline por
`scripts/verify-alloc-hardening.py`. O comando `alloctest` não existe na
produção.

Buddy, zonas, DMA32, NUMA, SLUB, next-fit, huge pages e mudanças de paginação
ficam deliberadamente para a Etapa 2.

## Etapa 1 F8.1: semântica C freestanding explícita

O perfil comum de C do kernel declara, ainda em `-O0`, que views de tipos
incompatíveis não obedecem strict aliasing, que o binário não depende de um
runtime de stack protector e que o endereço zero faz parte do modelo do
kernel. Os flags correspondentes são `-fno-strict-aliasing`,
`-fno-stack-protector` e `-fno-delete-null-pointer-checks`; eles pertencem ao
perfil comum e não a um objeto isolado.

Não existe walker de backtrace pela cadeia de `RBP` dentro do kernel. Existe,
porém, um consumidor externo autoritativo: o observador de comandos de
produção percorre o frame do caller para provar o status exato do handler.
Por isso `-fno-omit-frame-pointer` integra o perfil C comum e vale para todos os
objetos do kernel.

## Etapa 1 F8.2: fronteiras de entrada C e direction flag

O BSP limpa DF imediatamente depois de desabilitar interrupções e ajusta a
stack antes do salto para `kernel_main_high`; a primeira instrução C observa
`RSP % 16 == 8`, o estado que a ABI SysV teria depois de um `call`. O bootstrap
de AP já limpa DF na entrada de 16 bits. Para não alterar o blob de low memory
protegido pela F4, a transição final usa `ap_kernel_entry_asm` em `.text`: o
bridge limpa DF novamente, alinha a stack, reserva o slot equivalente ao
endereço de retorno e salta para `ap_kernel_entry`. O blob continua com 376
bytes, offsets de patch e bytes idênticos aos anteriores.

Todo stub externo salva o contexto original, limpa DF e somente então chama o
dispatcher C. `iretq` continua restaurando integralmente o RFLAGS interrompido;
assim, limpar DF para C não muda o estado retomado. Sob
`HOBBYOS_DEBUG_ASSERT`, os bridges registram alinhamento/DF do BSP e de cada AP,
e o stub registra o DF efetivamente visto na fronteira assembly/C. O comando
SELFTEST `profiletest abi` injeta DF=1 antes de `int $35`, prova DF=0 no
dispatcher e DF=1 depois do `iretq`, e executa `cld` antes de devolver controle
ao restante do kernel.

Esses registros e o comando não existem como interface de produção. Unwinding,
red zones e uma ABI de userspace continuam fora desta etapa.

## Etapa 1 F8.3: perfil de warnings fechado

Todo objeto C do kernel é compilado com `-Wall`, `-Wextra`, `-Wundef`,
`-Wmissing-prototypes`, `-Wvla`, `-Wframe-larger-than=2048` e `-Werror`. O
limite de frame continua 2048 bytes e nenhum `-Wno-*` global foi introduzido.
O único override por objeto é `-Wno-missing-prototypes` em `graphics.o`, pois
`graphics.c/.h` são protegidos pelo contrato visual e contêm duas definições
públicas legadas sem declaração. A exceção é conferida pela linha real do
compilador e não cobre nenhuma outra categoria ou unidade.

As correções de produto desta subfase são mecânicas: protótipos completos,
funções internas tornadas `static`, remoção de funções e variáveis
comprovadamente sem chamador, casos explícitos para enums e preservação de
qualificadores. A tabela HID deixou de sobrepor inicializadores designados,
mas seus 256 bytes compilados permanecem idênticos ao candidato F8.2.

Os acessos a membros packed do xHCI agora derivam registradores MMIO por
base mais `offsetof`, depois de `_Static_assert` de alinhamento natural. Isso
evita formar um ponteiro C para membro potencialmente desalinhado sem mudar o
endereço, a largura, o acesso `volatile`, a ordem ou os readbacks. Funções
mortas sem barreiras foram removidas. Quatro helpers dormentes que contêm
fences congeladas permanecem marcados `unused`: remover seus corpos violaria
o contrato de autoria. O inventário das 48 linhas com `mfence`, `lfence` ou
`sfence` é byte a byte igual ao baseline e tem SHA-256
`ffbc1f4c163b7a4b60593b84871aa400fd0a2d1df9282c52bc9fb387403aacc2`.

As unidades fora do escopo alteradas mecanicamente foram `timer/hpet`,
`smp/smp_boot`, `drivers/keyboard`, `drivers/pci`, `drivers/ps2`,
`drivers/timer.h`, `drivers/usb/xhci`, `graphics/console.h`,
`graphics/terminal`, `memory/gdt`, `memory/paging`, `core/idt`, shell e a
apresentação de TASKMAN somente para consumir um parâmetro já sem uso. Não
houve alteração de política ou comportamento. `graphics.c/.h` e
`task_format.c/.h` permaneceram byte-idênticos. O bootloader perdeu um helper
e uma variável locais sem uso para que a imagem de produção inteira também
não emita diagnósticos.

A otimização permanece `-O0` nesta fronteira. Correções de semântica que
somente apareçam com `-O2` pertencem à F8.4.

## Etapa 1 F8.4: otimização comum em `-O2`

Todo objeto C do kernel, nos perfis debug, SELFTEST e produção, usa `-O2`.
Não existe override para `-O0` ou `-O1`. `libc/memory.c` conserva o mesmo
nível de otimização e recebe somente
`-fno-tree-loop-distribute-patterns`, para impedir que GCC reconheça a
implementação dos próprios primitivos como uma chamada recursiva de libc.
O gate confere a linha real do compilador e rejeita esse flag em qualquer
outro objeto.

As quebras reveladas pela otimização foram corrigidas no contrato de origem:
o reload da GDT usa label numérico local que continua válido com inlining; o
snapshot seqlock de `schedtest` só sai depois de inicializar e estabilizar as
duas leituras; e o relatório grande de `taskmantest` foi dividido por uma
fronteira `noinline`, deixando o maior frame em 1984 bytes. A fronteira bruta
de CPUID e a função de fixup de #GP permanecem `noinline` porque são pontos de
auditoria externos da F5, não por incompatibilidade funcional com `-O2`.

Estado MMIO não pode ser inferido como memória comum. O ponteiro dos capability
registers xHCI agora conserva `volatile`; sem isso, GCC estreitava a leitura de
`HccParams1` para os 16 bits altos, fazendo o guest observar zero portas USB.
A correção restaura uma leitura de 32 bits e não altera nenhuma das 48 linhas
de fence protegidas. O scheduler também explicita o invariante fatal de que
`thread_block` precisa de uma task corrente, em vez de permitir uma
desreferência indefinida que o analisador de `-O2` detectou.

Os quatro símbolos privados usados pelo observador de layout de entrada da
shell são declarados `no_reorder`; isso preserva o schema legado sem mudar o
conteúdo ou a política da shell. O selftest de `%s` nulo chama o formatter por
sua fronteira sem atributo de formato, pois testa deliberadamente a semântica
`(null)` da libc do kernel e não a libc hospedada.

`shell_dispatch_command_line` permanece `noipa` porque os gates de panic e
regressão armam um breakpoint nessa fronteira antes de entregar comandos à
imagem de produção. O atributo conserva o símbolo e a observabilidade exatos
contra inlining e clonagem interprocedural; parsing, dispatch, handlers e
status continuam os mesmos. O perfil global conserva frame pointers porque o
observador acompanha o retorno dessa fronteira pela cadeia de `RBP`.
`shell_receive_char` preserva o frame intermediário e devolve o status do
handler; os consumidores normais podem ignorá-lo, sem mudar a política de
input, o roteamento ou o cleanup da shell.

Recibos estruturados emitidos por uma task de selftest concorrente também
precisam formar uma linha indivisível. O detalhe `TIMER_CANCEL` é formatado em
buffer limitado e entregue ao serial numa única chamada; erro de capacidade
vira FAIL explícito, em vez de produzir schema truncado ou intercalável.
Esse contrato foi exercitado por 20 ciclos focados SMP4/SMP24 e por um soak
SMP24 de 892379 ms; a quantidade de detalhes precisa ser exatamente igual à
de resultados estruturados, e o replay offline rejeita qualquer linha
ausente ou intercalada.

O epílogo de preempção de hard IRQ não entra na fila do ticket lock global do
scheduler. O timestamp já obtido pelo LAPIC chega ao scheduler e accounting,
decisão e eventual troca continuam sob uma única aquisição. Se
`spin_trylock` encontra contenção, o pedido de reschedule é republicado para
a próxima fronteira; o caminho voluntário continua bloqueante e FIFO. Isso
evita que uma vCPU preemptada enquanto detém o próximo ticket congele todas as
demais com IF=0, sem alterar quantum, prioridade, runqueue ou handoff.

Pelo mesmo motivo, estatísticas de IRQ não possuem mais um lock global no
tick de 1 ms. Cada slot de CPU atualiza atomicamente sua linha alinhada, e uma
linha de fallback cobre o intervalo anterior ao mapeamento de topologia.
Leitores agregam as linhas; nomes de API e saída pública permanecem iguais.
Assim, contabilização observacional não pode impedir progresso de xHCI/DPC em
SMP24. Reset e leitura podem observar interrupções concorrentes, como qualquer
snapshot de contadores vivos, mas nunca valores parcialmente escritos.

A calibração LAPIC conserva contador one-shot, divisor 16, janela mínima de
10 ms e período final de 1000 us. A leitura de `TCCR` em cada extremidade fica
entre duas leituras HPET; uma extremidade cuja janela exceda 500 us é
descartada como contaminada por desescalonamento do host, com no máximo 32
tentativas locais e falha explícita ao esgotá-las. O cálculo usa a diferença
dos dois contadores e os pontos médios dos brackets, em vez de contar desde a
programação de `TICR` até um timestamp capturado depois. Isso elimina a janela
assimétrica que podia inflar somente o numerador em KVM oversubscribed. Não há
mudança de topologia, clocksource, clockevent, threshold, timeout ou política
de APIC; o modelo hospedado rejeita bracket largo e contador não decrescente.

O screenshot UEFI do gate permanente de interrupções usa um breakpoint de
hardware temporário em `_start`: QEMU inicia pausado, GDB libera o firmware,
para todas as vCPUs na primeira instrução do kernel e pede o `screendump` pelo
monitor antes de continuar. Polling de marker serial não delimita o frame,
pois o kernel em `-O2` pode alcançar o clear antes do host. O log ainda exige o
marker UEFI de handoff e o hit no endereço do ELF. O verificador, os pixels
exigidos, o timeout e todos os limiares permanecem inalterados.

As mudanças mecânicas em componentes fora do escopo foram: qualificação MMIO
em `drivers/usb/xhci`, invariante fatal e epílogo não enfileirável em
`core/scheduler`, contabilização por CPU em `core/irq_stats`, estabilização do
selftest em `shell/commands/cmd_schedtest`, redução de frame em
`cmd_taskmantest` e preservação do schema privado em `shell.c`. Nenhuma
política de scheduler, USB, TASKMAN ou input mudou. `graphics.c/.h` e
`task_format.c/.h` permaneceram byte-idênticos. Cópia em largura de palavra,
seleção ERMS/FSRM e o benchmark de 8 MiB pertencem à F8.5.

## Etapa 1 F8.5: primitivas de memória em largura de palavra

`memcpy`, `memset` e `memmove` alinham o destino por bytes, transferem o miolo
em palavras de 64 bits por registradores gerais e tratam a cauda por bytes. A
origem pode continuar desalinhada: o tipo interno de palavra declara
explicitamente alinhamento 1 e aliasing permitido ao GCC. Na direção reversa,
`memmove` parte do fim, alinha esse limite e carrega cada palavra antes de
armazená-la, preservando sobreposição mesmo quando a distância é menor que
oito bytes.

O perfil continua `-mgeneral-regs-only`; o disassembly do gate exige operações
`QWORD` nas três primitivas e rejeita registradores XMM, YMM ou ZMM. A proteção
`-fno-tree-loop-distribute-patterns` permanece restrita a `memory.c`, evitando
que o compilador transforme os laços da própria libc em chamadas recursivas.
ERMS/FSRM não foi selecionado nesta etapa, pois é uma otimização opcional; o
caminho portátil de 64 bits funciona desde o início do boot e não depende de
detecção de CPU.

O gate hospedado compila o fonte real com símbolos renomeados e o compara com
referência byte a byte em todos os tamanhos de 0 a 4097, alinhamentos 0 a 15 e
sobreposições nas duas direções. O comando `profiletest memory`, presente
somente com `SELFTEST=1`, repete uma matriz reduzida no kernel, verifica guards
e integridade do heap e mede sete cópias de 8 MiB com TSC serializado. O marker
único publica mediana e checksum; a imagem de produção não registra nem
contém o comando.

## Etapa 1 F8.6: medição do perfil final

A comparação de boot usa exclusivamente `monotonic_ms` entre os breadcrumbs
`CPU_RELEASE` e `CORE_COMPLETE` em três VMs Q35/KVM/SMP4 independentes. Todas
as amostras partem de cópias do mesmo candidato congelado; não há descarte nem
retry seletivo. A mediana final de 167 ms ficou abaixo dos 207 ms do baseline
F0. Essa janela não mede firmware, splash ou prontidão da shell.

Tamanho é medido por `size -A` no ELF de produção, e reprodutibilidade por
SHA-256 do ELF e dos payloads, não por igualdade do container FAT. Warnings são
contados no log que compila sob `-Werror`, e stack pelo maior registro `.su`
mantendo o teto de 2048 bytes. O benchmark TSC é comparável apenas dentro do
mesmo perfil Q35/KVM/SMP4 e não constitui uma promessa de desempenho em
hardware físico ou em outra microarquitetura.

## Etapa 1 BM: fronteira de evidência física

Os comandos públicos `mem` e `cpu` preservam sua apresentação no console e
emitem, ao concluir, um recibo serial machine-readable atômico. A linha é
formatada em buffer limitado e entregue em uma única chamada, impedindo
intercalação entre CPUs. `[MEM][SUMMARY]`
publica o censo de PMM e heap e recalcula suas invariantes sob os validadores
dos próprios alocadores. `[CPU][SUMMARY]` publica identidade, família/modelo,
folhas CPUID, topologia descoberta, long mode e informação híbrida. Estado
inconsistente faz o handler retornar erro; texto visual não é usado como
oráculo.

O pacote BM contém imagens distintas de produção e SELFTEST, os ELF e EFI
correspondentes, hashes do container e dos cinco payloads internos e um
manifesto raiz. O verificador offline reabre todo esse material e também os
logs seriais; ele não inicia QEMU nem recompila o kernel. A imagem de produção
não contém comandos ou markers SELFTEST. A imagem diagnóstica os mantém, mas
usa `SELFTEST_AUTORUN=0` para que a sequência física seja comandada e capturada
explicitamente.

QEMU prova o formato dos recibos e o replay, mas não substitui o i9-12900K
para 24 threads heterogêneas, RAM real acima de 4 GiB, xHCI físico e MTRRs
reais. Execução bare metal e validação AMD permanecem fora da autoridade do
agente e, no caso AMD, fora desta etapa por indisponibilidade de máquina.
