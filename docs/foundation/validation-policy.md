# Política de validação da fundação

O desenvolvimento usa QEMU para build, injeção causal, regressão e observação
externa das ações do guest. Aprovação de desenvolvimento significa que o
candidato identificado satisfez os gates virtuais aplicáveis; ela não equivale
a validação física e não transforma resultados de QEMU em evidência de
hardware.

A validação física fica concentrada no fechamento. Ela deve completar as
amostras pendentes do candidato inicial e exercitar no candidato final os
cenários aplicáveis, inclusive panic, reset, desligamento e halt. Amostras do
kernel inicial e do kernel final permanecem em conjuntos separados e não
podem compor a mesma mediana.

O [baseline preservado](baseline.md) continua parcial: há uma janela física
observada de 3267 ms, não há mediana física e a associação operacional dessa
amostra ao candidato aguarda confirmação. O adiamento muda o momento da
coleta; não altera essas contagens nem os hashes já registrados.

Durante o desenvolvimento, os gates correntes são:

- `make kernel-check` e `make stack-check`;
- `scripts/test-arch-contract.sh all` para ordenação dos acessores MMIO,
  publicação coerente do trace por CPU, CPUID por subleaf, recuperação
  restrita de MSR, interface de portas e elisão da instrumentação em
  produção;
- `scripts/test-assert-record.sh all` para formar cada registro dinâmico do
  selftest de asserções antes da saída, provar capacidade e uma única emissão e
  preservar a rejeição de registros intercalados;
- `scripts/test-taskman-stats-record.sh all` para construir o registro de
  estatísticas de TASKMAN com capacidade limitada, preservar seus 52 campos e
  provar uma única emissão normal sob atividade concorrente;
- `scripts/test-section-layout.sh all` para classificar as seções de entrada,
  conferir fronteiras e páginas do ELF, comparar o trampoline com os mesmos
  objetos e rejeitar seções órfãs inesperadas;
- `scripts/test-assert-list.sh all` para polaridade e contexto das asserções,
  integridade de listas antes da mutação, ligação fatal ao panic e eliminação
  do custo no candidato de produção;
- `scripts/test-libc-format.sh all` para capacidade, comprimento necessário,
  erros, extremos inteiros, compatibilidade dos diagnósticos e selftest no
  guest;
- `scripts/test-panic-lock-contention.sh all` para o caminho terminal de
  emergência;
- `scripts/test-foundation-regression.sh all` para boot, interrupções,
  clockevent, timers e consumidores normais;
- os comandos `irq check`, `irq controllers`, `irq routes` e `irq boot`, com
  seus resultados estruturados preservados.

O modo `all` do gate de layout recebe `SECTION_CANDIDATE_MANIFEST` com os
cinco papéis obrigatórios e seus recibos. `SECTION_BASE_COMMIT` aponta para a
âncora pré-F4 `1edeb89d6fcd4e1339dd55cffc33ec521eb1849f`, cujo linker ainda
contém o catch-all usado no par antes/depois. O default `HEAD` só é apropriado
durante a implementação ainda não commitada da F4; no fechamento ele já
representa corretamente o lado sem catch-all.

As asserções ficam habilitadas por padrão nos perfis de desenvolvimento e
desabilitadas explicitamente em produção. A instrumentação destrutiva do gate
exige, separadamente, selftest e a flag do teste; nenhuma dessas opções isolada
arma corrupção. O gate focal cobre Q35/SMP4 em TCG e KVM, contexto precoce em
SMP1/TCG e identidade/contenção em SMP24/KVM. A observação de halt e dos bytes
da fixture é externa ao kernel sob teste.

O candidato de regressão habilita os selftests sem autorun e usa o protocolo
framed já existente. Cada comando precisa de sequência, checksum, BEGIN, END e
status exato do handler dentro de uma transação própria. Linhas posteriores,
texto humano e êxito no envio de teclas não completam a transação. A imagem de
produção não recebe esse protocolo; nela, quando o retorno não está disponível
na serial, o debugger observa a entrada e o retorno reais do comando sem chamar
o handler nem escrever seu resultado.

`[TASKMANTEST][STATS]` é válido somente quando uma linha completa contém o
prefixo e todos os campos dentro da mesma transação. O produtor monta a linha
inteira antes da saída e a entrega por uma chamada de serial; falha de
armazenamento ou formatação produz um registro de erro distinto e retorno não
zero. Um registro dividido, ainda que tenha BEGIN, END e retorno zero, continua
rejeitado e não pode ser unido durante o replay.

Os registros dinâmicos `PREPARED`, `EXECUTE`, `CONTEXT_READY` e `COMPLETE` do
selftest de asserções seguem o mesmo princípio de indivisibilidade. Cada marker
válido precisa conter seus campos completos em uma linha produzida por uma
única chamada normal à serial. A LF inicial de delimitação pertence à mesma
mensagem. Falha de construção produz `RECORD_ERROR`, marca o estado do ensaio e
não autoriza um `PASS` posterior. Estado observado por GDB não substitui um
campo serial incompleto.

Os verificadores são fechados por padrão. Eles cruzam perfil, candidato, ELF,
imagem, linha de lançamento, status do handler e evidência bruta. Snapshots por
CPU exigem slots exatos e resultados assíncronos exigem o mesmo run ID. Ações
terminais de panic exigem diagnóstico aplicável e observação externa causal na
mesma VM. Ausência, duplicação, resultado tardio ou evidência de outro binário
causam rejeição.

O gate de arquitetura separa acesso volátil, barreira do compilador, fence de
CPU e conclusão de escrita posted. As variantes MMIO existentes conservam a
ordenação forte; as variantes `relaxed` não substituem fences exigidas por
drivers. O trace existe somente com `HOBBYOS_DEBUG_ASSERT=1`, usa registros por
slot de CPU e leitura com orçamento finito. Produção conserva os acessos e as
fences, sem stores, chamadas ou identificação de CPU para trace.

Uma operação rastreada mantém o pin de CPU desde antes da identificação local
até depois da publicação do resultado, restaurando exatamente o IF recebido.
O snapshot da CPU corrente aplica a mesma regra durante resolução e leitura.
O cache de resolução compara o identificador arquitetural completo e trata
colisões sem substituir a identidade de outra CPU; indisponibilidade de
topologia ou reentrada continua sendo resultado limitado, sem espera.

O replay de arquitetura deriva as conclusões dos seriais, arquivos GDB, QMP e
launch obrigatórios. Endereços das células, associação slot/CPU, sites e
destinos de #GP, valores CPUID/MSR, budgets, continuidade e cleanup precisam
coincidir com esses brutos. Campos derivados em JSON não substituem o arquivo
de origem. Uma leitura em rendezvous com escritores parados comprova
identidade estável; concorrência exige também progresso entre amostras enquanto
os escritores permanecem ativos.

A recuperação de MSR aceita somente #GP de kernel, com error code zero e RIP
exatamente cadastrado na seção `.cpu_msr_fixup`. Qualquer outra exceção segue
o panic normal. A tabela, os opcodes protegidos, os destinos e a continuidade
do guest são conferidos em conjunto; um retorno booleano isolado não prova a
execução da instrução nem a exceção.

No negativo de #GP alheio, a cardinalidade autoritativa vem de um registro do
próprio handler, compilado somente com `HOBBYOS_ARCH_TEST` e atualizado depois
de a tabela MSR recusar o frame. O gate exige exatamente uma entrada do kernel,
RIP igual ao site proposital, error code zero e CS de kernel, além de exatamente
um frame, vetor e pedido de halt no serial, ausência de retorno e dois
snapshots terminais com IF=0. O stub GDB conserva todas as notificações brutas
do hardware breakpoint. QEMU TCG pode entregar mais de uma notificação para a
mesma parada; nenhuma é descartada, e todas precisam formar ordinais contíguos
e repetir exatamente site, error code e CS. Uma segunda entrada registrada
pelo kernel continua sendo falha, mesmo quando os eventos GDB são idênticos.
O candidato de produção não contém esse estado nem seu recorder.

O Makefile não torna o linker script uma dependência direta de `kernel.elf` e
permanece inalterado. Por isso, o gate de layout força builds limpos e relinks
identificados. Famílias esperadas são classificadas explicitamente; metadados
sem consumidor de runtime têm descarte declarado. Uma órfã desconhecida deve
continuar visível no ELF e no mapa, e o gate a reprova. Nas linkagens de
auditoria, `--orphan-handling=warn` registra também o diagnóstico do linker.
Essa política do gate não implica que uma invocação incremental isolada de
`make image` rejeite órfãs automaticamente.

Os retornos já coletados dos harnesses históricos
`scripts/test-interrupt-bringup.sh all` e
`scripts/test-timer-clockevent.sh all` permanecem preservados. Esses scripts
fixam commits, manifestos e contratos anteriores, entre eles HPET Timer0 ativo,
incompatíveis com a política corrente de HPET somente como clocksource, Timer0
quiescente e BSP LAPIC como clockevent global. Quando forem invocados, uma
parada no preflight é registrada como incompatibilidade de baseline; ela não é
convertida em sucesso nem usada como resultado da regressão corrente.

Os harnesses focais históricos de modal/heap e de apresentação também
preservam seus preflights e evidências retrospectivas. Quando somente esses
requisitos congelados impedem a execução, a cobertura corrente é registrada
separadamente: o gate de formatação executa as séries modal/heap com transporte
framed, limites e reconciliação de recursos, e observa TASKMAN nas geometrias
WIDE e COMPACT com screendump real e saída por ESC. Essa equivalência não
altera nem aprova retroativamente o retorno bruto do harness histórico.

Todo resultado referencia o ELF e a imagem exercitados, registra configuração,
retorno do host e cleanup e mantém separado o resultado bruto de cadência de
sua autoridade. Em oversubscription, o threshold de 700 permanece inalterado;
a observação sem autoridade exige conclusão causal do comando e um controle
KVM não oversubscribed válido. A capacidade é calculada a partir das CPUs que o
processo pode efetivamente usar, não do nome do perfil.

Uma mudança numa unidade selecionada exclusivamente por uma flag de teste pode
reutilizar gates de outros perfis somente quando a unidade estiver ausente da
linha de link, o ELF reconstruído aplicável ou os payloads relevantes forem
idênticos e o verificador completo for reexecutado offline sobre a evidência
original. O mapa de reuso distingue teste novo, replay offline e binário
reutilizado; horário novo ou nome de pasta não prova equivalência.

O pacote enviado para revisão contém o snapshot versionado e a evidência
textual focal. Binários, imagens e campanhas completas continuam preservados no
workspace, sem inclusão automática no ZIP leve. Assim, integridade de fontes e
protocolo textual podem ser revistas no pacote, enquanto replay de ELF, FAT e
runtime permanece explicitamente indisponível a partir desse ZIP.

O orçamento de parede de um observador de panic precisa cobrir o limite finito
do caminho sintético exercitado no guest. O cálculo parte do número máximo de
polls e de uma medição causal de progresso; não pode ser aumentado depois de
tentativas até escolher uma amostra favorável. Um caso positivo termina assim
que a ação terminal é observada. O negativo de não conclusão conserva um
orçamento menor e independente, para continuar detectando espera sem limite.

Igualdade exata entre dois snapshots globais de heap só certifica o lifetime
sob teste quando os endpoints cobrem todos os usuários relevantes do alocador
ou quando as alocações pertencentes ao lifetime são identificadas
separadamente. Estabilidade do reaper e repetição de totais não tornam timers,
DPCs ou outros usuários assíncronos quiescentes. Na ausência dessa
comparabilidade, o resultado permanece bloqueado; não se aceita tolerância,
compensação, snapshot escolhido posteriormente ou retorno zero de outro
comando como substituto.

O consumidor modal do gate de formatação estabelece seu endpoint por recursos
controlados. Ele captura os handles com geração dos workers de stress, exige
progresso nos mesmos handles e, depois da solicitação de parada, aguarda com
limite o desaparecimento de cada identidade e o fim de `free_inflight`. Os
deltas e estados de modal, sessão, rota e modelo de TASKMAN precisam confirmar
o mesmo lifetime. Ausência de progresso, handle reciclado, retenção, drenagem
pendente ou expiração reprovam a transação.

O delta global de heap desse cenário é preservado como dado bruto e marcado
como não comparável enquanto serviços independentes usam o mesmo alocador. O
PASS tem alcance `TARGET_IDENTITIES`: certifica o lifetime dos recursos
capturados e do modal exercitado, sem declarar ausência de vazamento global.
Uma retenção identificada não pode ser ocultada por outra liberação do mesmo
tamanho. Os registros de checkpoint são limitados, completos em uma chamada de
serial e reconstruídos pelo verificador dentro das transações framed.

O gate focal hospedado é `scripts/test-modal-checkpoints.sh all`; seu replay
offline é `python3 scripts/verify-modal-checkpoint-evidence.py all
<evidence-root>`. Ele não inicia as VMs do gate de formatação e não substitui
as duas execuções SMP4 e três SMP8 exigidas para o consumidor real.

A checagem estática desse gate ancora a classificação de retenção na
estrutura de relatório introduzida pela F8.4: falha de liberação continua
produzindo `target-retained` quando `report.retained` é diferente de zero e
`async-drain` caso contrário. A mudança de variáveis locais para campos de
`report` reduz o frame; não altera a decisão nem os fixtures negativos.
O replay liga dinamicamente base, commit e tree registrados no preflight,
exige ancestralidade Git e compara byte a byte cada fonte preservado com o
blob do commit candidato. Assim a promoção do candidato não depende de um
hash de commit histórico embutido no verificador e não reduz a garantia de
proveniência.

`irq boot` usa um único budget de 5000 ms para adquirir o conjunto de slots
descobertos. A preparação do armazenamento ocorre antes desse budget, e a
publicação ocorre somente depois que a aquisição termina ou registra sua
primeira falha. O budget não é reiniciado por slot e não inclui o custo de
imprimir amostras anteriores. Cada amostra ainda precisa estar fora de IRQ e
passar pelos critérios existentes; o conjunto representa snapshots
individualmente estáveis, sem alegar simultaneidade global. Armazenamento,
topologia, timeout ou formatação inválidos retornam falha e não permitem um
resultado parcial promovido a PASS.

O replay focal dessa coleta é `python3 scripts/verify-smp-observation.py all
<evidence-root>`. Ele reconstrói BEGIN/END, status, slots, contadores e tempos
dos seriais, e também confere as duas observações de prontidão. Os testes host
são executados por `scripts/test-irq-snapshot-observation.sh`; o subcomando de
replay não compila nem inicia VM.

Nos cenários de panic Q35/KVM/SMP24, a prontidão tem deadline absoluto de 120
segundos e checkpoint de observação aos 45 segundos. A chegada a
`RUNTIME_READY` permite avançar, mas o observador de boot isolado conserva a
janela até o checkpoint para registrar a comparação com o contrato anterior.
Um trigger nunca é armado antes da prontidão. Esse budget não se soma aos
limites pós-trigger: casos TCG comuns, adversarial e perfis KVM conservam seus
limites próprios. `query-status=running` e shutdown iniciado pelo QMP do host
não substituem o marker de prontidão nem uma ação terminal do guest.

O runner da regressão framed aguarda também o marker exato `TEST_READY` antes
do primeiro comando. Essa guarda não recupera nem aceita uma linha rejeitada:
cada transação continua exigindo CRC, ACCEPT, BEGIN e END únicos. Rejeição
sintática antes do dispatch é falha de transporte, preserva os bytes originais
e impede que o perfil e o soak sejam declarados completos.

O relógio monotônico não pode manter um lock global com proprietário enquanto
lê HPET no caminho de tick LAPIC. O estado bruto por CPU é acessado com pinning
por `irq_save`, e o valor global é publicado como máximo atômico linearizável.
O baseline global é capturado antes da transação MMIO para que duas leituras
sobrepostas não produzam um falso lag apenas pela ordem de conclusão. Contadores
de diagnóstico são atômicos; o ring de anomalias usa aquisição não bloqueante e
não tem autoridade superior aos contadores agregados.

A regressão desse contrato exige Q35/KVM/SMP24 sem reduzir CPUs nem mudar o
período de 1000 us. Depois de carga concorrente do relógio, `CLOCK_SMP` precisa
terminar sem regressões ou lag, e `irq check` precisa obter todos os snapshots
com `unexpected=0` e `imbalance=0`. O candidato de produção executa o mesmo
`irq check` com retorno real do handler observado. O gate estático rejeita a
reintrodução de `spin_lock`, `spin_lock_irqsave`, `spin_cpu_relax` ou
`g_clock_lock` em `clock_monotonic_ns()` e exige compare-exchange na publicação.

O transporte framed mantém um socket QMP privado e forma o envelope imutável
antes da primeira tecla. Cada caractere é um único pedido QMP com key-down e
key-up; antes do caractere seguinte, a observação física lê o span da shell,
lê o estado xHCI e relê o mesmo span da shell. Somente duas imagens de shell
idênticas podem confirmar o prefixo e a liberação. Uma imagem diferente é
`UNSTABLE` e autoriza apenas nova observação dentro do mesmo limite, nunca novo
pulso; bytes divergentes em duas leituras estáveis encerram a transação com
falha. O frame completo deve ficar estável antes do Enter, e um observador GDB
separado confirma os mesmos bytes na entrada do dispatcher. Os limites são
fixos e não autorizam reenvio de um comando cujo dispatch seja incerto.

O replay reconstrói as leituras físicas a partir do ledger QMP, recompõe o
mapeamento dos símbolos pelos segmentos do ELF e confere a observação bruta do
dispatcher. Integridade de entrada e retorno do handler são estados distintos:
um frame pode chegar e ser despachado corretamente enquanto o handler retorna
erro. Da mesma forma, ACK de QMP, eco visual ou ausência de marker não substitui
a correlação integral entre frame, buffer, dispatch, ACCEPT, BEGIN e END.

## Gate de progresso de spinlock

`scripts/test-lock-progress.sh` oferece os modos `host`, `build`, `runtime`,
`production`, `fixtures`, `verify` e `all`. Cada execução persiste comandos,
status, hashes, candidato, launch, serial, debugcon, trace e transações framed.
O modo `verify` é offline: não recompila e não inicia VM. Ele reabre os
arquivos copiados para a evidência, valida seus hashes e deriva o resultado do
registro terminal, em vez de confiar em um resumo favorável.

O perfil Q35/KVM/SMP8 é a autoridade de justiça porque oito vCPUs cabem nas 12
CPUs schedulable do host. Ele exige uma aquisição por iteração e worker,
contenção observada, todas as oito CPUs, concorrência simultânea, zero
violação de exclusão ou IF e distância máxima menor ou igual a sete. O perfil
SMP24 executa o mesmo contrato com limite 23, mas sua medição de justiça não
é autoritativa por oversubscription; fault, panic, deadlock, corrupção,
status não nulo ou cobertura funcional incompleta continuam reprovando.

O teste hospedado usa o fonte real do lock e exige wrap do contador, falha de
`trylock` sem mudança de estado, exclusão, progresso e restauração de IF. As
fixtures negativas mantêm registros sintaticamente válidos: uma excede a
distância FIFO e outra declara violação de exclusão; ambas precisam ser
rejeitadas por causa específica. Produção precisa passar a política existente e
não conter `locktest` em strings ou símbolos.

## Gate de endurecimento dos alocadores

`scripts/test-alloc-hardening.sh` oferece `host`, `build`, `runtime`,
`production`, `fixtures`, `verify` e `all`. O replay
`scripts/verify-alloc-hardening.py all <evidence-root>` é offline: valida
manifesto, hashes, retornos, serial e registros sem recompilar nem iniciar VM.

O host compila o fonte real de `bitmap.c`, em modo normal e UBSan, e o compara
com uma referência por bit para tamanhos 1, 63, 64, 65, 4095, 4096 e 4097,
incluindo bitmap vazio, cheio, fragmentado, ranges e cauda parcial. Primeira
posição e esgotamento precisam coincidir exatamente.

O runtime exige um frame transacional completo, status zero do handler e o
marker terminal único. São obrigatórias duas recusas de overflow do heap, uma
de ponteiro interior, uma de ponteiro externo, duas de double-free e uma para
cada classe do PMM: nulo, desalinhado, fora do span, reservado e já livre.
Cada recusa deve produzir seu contador e `KWARN_ON`, sem alterar a operação
protegida. Integridade de heap e PMM é recontada ao final. Com debug ativo,
a busca do PMM deve registrar chamada e iterações por palavra.

Snapshots globais do heap não atribuem causalidade quando outros CPUs e
serviços podem alocar entre endpoints. O selftest usa o retorno do mesmo
caminho interno de `kfree`, visível somente em `SELFTEST=1`, e combina recusa,
contador específico e integridade. O censo global continua registrado como
diagnóstico, mas não substitui essa prova causal.

Produção precisa provar ausência de registro, string e símbolo de
`alloctest`. As fixtures de double-free silencioso e divergência da busca por
palavra precisam ser rejeitadas separadamente. Como o resultado do gate é
funcional, fault, panic, corrupção, deadlock ou status não nulo são sempre
autoritativos, inclusive sob oversubscription.

## Perfil C da F8.1

O log de `kernel-check` é a autoridade sobre a linha real do compilador, não
apenas a atribuição textual do Makefile. Todos os objetos C do kernel devem
receber `-O0`, `-fno-strict-aliasing`, `-fno-stack-protector` e
`-fno-delete-null-pointer-checks` nesta subfase. O bootloader não participa
desta política e flags diagnósticos em `KERNEL_EXTRA_CFLAGS` não podem remover
os quatro flags comuns.

A necessidade de preservar frame pointers é decidida por um consumidor real.
O observador autoritativo de comandos de produção percorre a cadeia de `RBP`
para observar o status retornado pelo handler; portanto toda linha C do kernel
precisa conter também `-fno-omit-frame-pointer`. O gate confere o log real do
compilador e não admite exceção por objeto.

## Gate de ABI de entrada e direction flag da F8.2

`scripts/test-build-profile.sh` oferece os modos `static`, `build`, `runtime`,
`production`, `fixtures`, `verify` e `all`. O replay
`python3 scripts/verify-build-profile.py all <evidence-root>` é offline:
reabre o manifesto de fontes, logs/status de build, hashes, serial, transação
framed, política de produção e fixtures sem compilar nem iniciar VM.

O runtime autoritativo desta subfase é Q35/KVM/SMP4 com
`HOBBYOS_DEBUG_ASSERT=1` e `SELFTEST=1`. Um único marker terminal deve provar
alinhamento 8 do BSP e de todos os APs, DF zero nessas entradas, ao menos uma
entrada IRQ, zero entrada C de IRQ com DF setado e um probe causal no vetor 35.
O probe parte de DF=1; o stub precisa apresentar DF=0 ao C, `iretq` precisa
restaurar DF=1 e o comando precisa terminar novamente com DF=0. Retorno não
nulo, AP ausente, fault, panic ou cleanup incompleto reprovam.

Build e produção executam também o verificador F4 sobre seus ELFs. O tamanho de
376 bytes do trampoline e seus offsets continuam invariantes; a F8.2 não
autoriza atualizar esse oráculo. Produção deve ainda provar que `profiletest`
não aparece no registro, em strings nem em símbolos. As fixtures negativas de
stack BSP desalinhada e DF observado em C precisam ser rejeitadas por causas
distintas.

## Warnings obrigatórios da F8.3

O gate de perfil também reabre os logs das builds SELFTEST comum, SELFTEST com
format/assert/arch habilitados e produção. Toda linha GCC de objeto C do kernel
precisa conter `-O0`, `-fno-strict-aliasing`, `-fno-stack-protector`,
`-fno-delete-null-pointer-checks`, `-fno-omit-frame-pointer`, `-Wextra`, `-Wundef`,
`-Wmissing-prototypes`, `-Wvla`, `-Wframe-larger-than=2048` e `-Werror`.
Qualquer diagnóstico `warning:` ou `error:` reprova o log.

O verificador rejeita todo `-Wno-*`, exceto exatamente
`-Wno-missing-prototypes` na linha de `kernel/src/graphics/graphics.c`. Essa
exceção existe somente porque o arquivo e seu header estão sob manutenção
visual protegida; ela não pode se propagar para outro objeto nem ganhar outra
categoria. O manifesto da evidência inclui todas as unidades ajustadas, o
Makefile e os verificadores.

O gate de arquitetura congela o contrato do xHCI pelo conteúdo que a regra de
autoria realmente protege: as 48 linhas de fences explícitas. O coletor exige
igualdade byte a byte com o commit-base e também o hash independente
`ffbc1f4c163b7a4b60593b84871aa400fd0a2d1df9282c52bc9fb387403aacc2`.
Alterar, remover, acrescentar ou reordenar uma dessas linhas reprova o gate;
mudanças mecânicas necessárias para warnings fora delas continuam auditáveis.

## Perfil otimizado da F8.4

A partir da F8.4, a regra de `-O0` das fronteiras F8.1–F8.3 é substituída por
`-O2` em toda linha GCC de objeto C do kernel, tanto em debug/SELFTEST quanto
em produção. O replay de `test-build-profile.sh` exige exatamente um flag de
otimização e ele precisa ser `-O2`; `-O0`, `-O1`, `-O3`, `-Os`, `-Og` e
overrides por objeto reprovam.

`kernel/src/libc/memory.c` recebe
`-fno-tree-loop-distribute-patterns` para não sintetizar chamadas aos
primitivos que ele próprio define. Essa exceção não muda o nível de otimização
e precisa aparecer exatamente nessa unidade e em nenhuma outra. O
`stack-check` continua com limite 2048; `violations=0` e o maior frame real são
registrados em cada campanha.

Uma campanha válida reabre build comum, build com todos os testes condicionais,
layout F4, runtime ABI BSP/AP/IRQ, produção e fixtures. Três execuções
consecutivas do gate focal precisam produzir o mesmo ELF. Arquitetura, locks,
alocadores, formatação, panic, asserções, interrupções, timers e regressão
geral continuam gates independentes; passar o gate focal não os substitui.

Falhas de progresso do input ou de qualquer comando em SMP24 continuam
funcionais e autoritativas, mesmo com host oversubscribed. Para diagnosticar
uma falha já observada, `test-interrupt-bringup.sh` aceita
`HOBBYOS_IRQ_GDB_DIAGNOSTICS=1`: GDB fica passivamente disponível e um único
snapshot de pilhas/locks é capturado somente depois do primeiro timeout. A
opção não repete comando, não amplia timeout e não muda a classificação. Uma
assinatura intermitente só é encerrada depois de três execuções consecutivas
verdes do conjunto sensível correspondente.

A calibração do LAPIC precisa medir tempo e ticks entre as mesmas duas
extremidades. Cada leitura de `TCCR` é bracketed por HPET e um bracket acima de
500 us é recusado; existem no máximo 32 tentativas por extremidade. O selftest
do modelo exige o resultado exato de uma amostra limpa e a rejeição de uma
extremidade contaminada e de um contador não decrescente. As estáticas de
interrupções e timers exigem a amostragem delimitada e rejeitam a antiga
subtração de uma leitura tardia de `TCCR` a partir de `UINT32_MAX`. Janela de
10 ms, divisor, período de 1000 us e todos os oráculos permanecem inalterados.
O gate de timer também ancora o arquivo LAPIC completo por SHA-256; uma futura
mudança, mesmo commitada, exige promoção explícita dessa âncora.

Cada VM do gate de timer possui um transcript de comandos limitado ao próprio
cenário. O negativo de ordem declarada, que inicia QEMU diretamente para
preservar sua instrumentação exclusiva, zera `commands.tsv` depois de parar a
VM anterior e antes de iniciar a nova. O replay continua exigindo exatamente
um `synctest timer-order`; comandos preservados de uma execução anterior não
podem compor evidência do candidato corrente.

O gate de timer conserva manifests do scheduler antes, durante e depois da
campanha. Como a F8.4 alterou legitimamente o consumidor de hard IRQ, a parede
contra diferença cega do HEAD foi substituída por um oráculo semântico: o
caminho voluntário deve continuar bloqueante/FIFO; o epílogo de IRQ deve ser
não enfileirável, receber o timestamp do LAPIC e republicar o pedido quando o
`trylock` falha. Regressão para `schedule_impl(0)` em IRQ reprova a estática.

A captura visual anterior ao early clear inicia o QEMU pausado, instala um
breakpoint de hardware temporário em `_start` e captura pelo monitor do GDB
enquanto todas as vCPUs estão paradas na primeira instrução do kernel. O log
precisa provar tanto o hit no endereço do ELF quanto o marker UEFI de handoff;
um marker serial sozinho não é uma fronteira temporal suficiente porque o
kernel otimizado pode limpar o framebuffer antes do polling do host. O frame
early ainda depende de `[GRAPHICS][EARLY_CLEAR] PASS`, e
`verify-early-framebuffer.py` conserva sem alteração os limiares de diferença,
predominância do fundo e avanço até a shell.

O gate de listas conserva ainda uma âncora exata de `interrupts.c`. Ela
representa a composição dos contratos F5, F8.2 e F8.4 com o recorder de #GP
da campanha final: exige explicitamente o lookup da tabela MSR, hooks de
arquitetura, a fronteira #GP `noinline`, o argumento `entry_df`, seu registro
pelo build profile, a entrega do timestamp do LAPIC ao epílogo de preempção
sem nova leitura e exatamente duas chamadas condicionais ao recorder de #GP
não tratado. A âncora corresponde ao fonte que eliminou o convoy SMP24 e
tornou autoritativa a cardinalidade do negativo de arquitetura. Assim, sua
promoção não transforma uma divergência futura em permissão: qualquer byte,
token ou cardinalidade diferente continua reprovando.

## Cópia larga da F8.5

O modo `build` de `scripts/test-build-profile.sh` compila a implementação real
de `memory.c` com nomes privados e `-fno-builtin`, e executa uma referência
hospedada byte a byte. São obrigatórios tamanhos 0 a 4097, todos os pares de
alinhamento de origem/destino 0 a 15, três valores de `memset` e sobreposição
de `memmove` nas duas direções. Retorno incorreto, guard alterado ou qualquer
byte divergente encerra o gate.

O mesmo build persiste o disassembly de `memory.o`. Cada uma das funções
`memcpy`, `memset` e `memmove` precisa conter operação em palavra de 64 bits;
qualquer registrador XMM/YMM/ZMM reprova. A linha do compilador continua
precisando de `-O2`, `-mgeneral-regs-only` e do guard de reconhecimento de
loops restrito a esse objeto.

No runtime Q35/KVM/SMP4, `profiletest abi` e `profiletest memory` são duas
transações framed independentes, ambas com status zero. O segundo comando
cobre 67.728 casos, incluindo 18.816 overlaps por direção, e publica uma
única linha com falhas zero, heap íntegro, sete amostras e mediana TSC de uma
cópia de 8 MiB. O verificador exige cobertura exata, ciclos e checksum não
nulos. Fixtures com divergência de memória ou mediana zero precisam ser
rejeitadas, além das fixtures anteriores de ABI/DF. O replay `all` permanece
offline e a produção precisa provar ausência de `profiletest`.

## Medição comparativa da F8.6

A campanha de boot final exige exatamente três execuções Q35/KVM/SMP4 do
mesmo candidato congelado. Cada serial precisa conter exatamente um
`CPU_RELEASE` e um `CORE_COMPLETE`, numéricos, ordenados e sem panic/fault. A
amostra é a subtração de `monotonic_ms`; tempos do host não participam. Nenhuma
execução pode ser descartada por ser lenta, e a mediana é o valor central das
três amostras.

Seções são comparadas com `size -A` sobre o ELF de produção. A contagem de
warnings vem do mesmo log que mostra as flags reais e precisa ser zero sob
`-Werror`; o maior frame vem de `stack-check`, cujo limite permanece 2048.
Benchmarks TSC antes/depois usam o mesmo perfil, tamanho, número de amostras e
checksum. Piora em qualquer métrica temporal exige investigação causal antes
do aceite; variação do hash bruto FAT não é regressão se ELF e payloads forem
os declarados.

## Handoff bare metal da Etapa 1

`scripts/verify-bare-metal.py` tem três operações independentes:
`candidate` valida o pacote e seus payloads; `log production|selftest` reabre
uma captura serial; e `fixtures` prova rejeição dos oráculos negativos. Todas
são offline: não recompilam, não iniciam VM e não alteram a evidência.

Em ambiente `physical`, a autoridade é fixada em 24 CPUs, PMM acima de 4 GiB,
CPU GenuineIntel com CPUID híbrido e tipo de core presente, além dos contratos
funcionais comuns de boot, IRQ, clock, xHCI, memória e CPU. O perfil SELFTEST
acrescenta os resultados exatos de `locktest progress 24 2000` e
`alloctest hardening`; o perfil de produção rejeita qualquer marker desses
testes. Falha funcional, ausência, duplicidade, reordenação ou inconsistência
aritmética reprova sem interpretação manual.

O modo `--environment qemu` existe somente para smoke do schema e do replay:
permite a quantidade de CPUs e a RAM virtuais informadas, sem atribuir a elas
autoridade física. Um PASS QEMU nunca preenche o checklist bare metal. A
captura física é feita pelo mantenedor e qualquer FAIL bruto é preservado;
não há retry automático, mudança de período, redução de CPUs ou relaxamento
de limiar.
