# Asserções de runtime e integridade de listas

## Contrato das asserções

`KBUG_ON(cond)` e `KWARN_ON(cond)` disparam quando `cond` é verdadeira. No
perfil habilitado, a expressão é avaliada uma vez; uma condição falsa não
produz registro. `KWARN_ON` registra a violação e retorna. `KBUG_ON` entra no
caminho real de panic e não retorna. As macros usam `do { ... } while (0)`, de
modo que permanecem seguras em construções `if/else`.

O Makefile define `DEBUG_ASSERT=1` no desenvolvimento normal e valida que seu
valor seja 0 ou 1. O perfil `DEBUG_ASSERT=0` elimina a avaliação da expressão,
a coleta de contexto e a chamada aos helpers; a expansão desabilitada não usa
`sizeof`, `unreachable` nem função vazia. `make production-image` fixa
explicitamente `DEBUG_ASSERT=0`, `ASSERT_TEST=0`, `FORMAT_TEST=0`,
`SELFTEST=0`, `SELFTEST_AUTORUN=0` e flags extras vazias.

`ASSERT_TEST=1` é independente do perfil de desenvolvimento e exige tanto
`SELFTEST=1` quanto `DEBUG_ASSERT=1`. A flag habilita somente fixtures e hooks
do candidato destrutivo. `DEBUG_ASSERT=1` ou `SELFTEST=1` isoladamente não arma
teste, corrupção ou panic.

O callsite fornece `__FILE__`, `__LINE__` e a expansão textual de `cond`. Os
registros estruturados usam os prefixes `[ASSERT][WARN]` e `[ASSERT][BUG]`.
Arquivo e expressão são codificados em hexadecimal, evitando que espaços,
aspas ou escapes criem campos falsos. Cada texto traz comprimento, validade e
indicação de truncamento. Os limites são 96 bytes para arquivo e 160 para
expressão; o aviso completo usa um buffer automático de 1024 bytes. Falha ou
truncamento da formatação do aviso emite um texto constante limitado.

Os demais campos são `line`, `cpu_id`, `cpu_valid`, `cpu_slot`, `slot_valid`,
`task_id`, `task_generation`, `task_valid` e, no aviso, `if_enabled`. O APIC ID
vem de CPUID local, sem MMIO e sem lock. O slot só é publicado quando a
topologia completa contém esse APIC ID. A identidade de task copia apenas ID e
geração durante um intervalo protegido por `irq_save`; nenhum `task_t *` fica
retido. Antes da topologia ou do scheduler, os campos de validade permanecem
zero em vez de fabricar identidades.

## Dependências e integração com panic

| Caminho | Dependências alcançáveis | Consequência |
|---|---|---|
| condição falsa | avaliação local da expressão | nenhuma saída; continua |
| aviso | IRQ local, CPUID, leitura da topologia, task corrente, `ksnprintf`, serial normal | uma linha e restauração exata de IF |
| bug | eleição atômica do owner, serial de emergência e ação limitada já auditadas | contexto depois do ownership; não retorna |
| reentrada do bug | protocolo de profundidade existente | segunda entrada mínima; terceira silenciosa em halt |

O helper fatal não imprime antes de chamar o núcleo interno de panic. Primeiro
ele desabilita interrupções, assume ownership e publica o modo de emergência;
só então captura os campos opcionais e os emite pelo canal de emergência. A
API pública `kpanic` não mudou e panics comuns continuam sem depender do novo
contexto. A captura fatal não chama `scheduler_cpu_pin_validate`, não adquire o
lock do scheduler e não usa console, heap, clock, timer ou alocação.

O aviso usa a serial normal e restaura os flags do chamador. Seu contrato exige
um contexto em que a serial normal esteja disponível e no qual o chamador não
retenha o próprio lock da serial; não há promessa de uso recursivo no driver ou
em NMI. O teste precoce roda depois da IDT e antes do PMM, com IF desabilitado,
e confirma explicitamente task desconhecida. O panic fatal conserva a
independência de locks mesmo sob os locks reais de heap e scheduler.

## Validação antes da mutação

As inserções validam, antes do primeiro store, ponteiros obrigatórios,
identidade entre o novo nó e seus vizinhos, reciprocidade
`prev->next == next`/`next->prev == prev` e o estado desanexado do novo nó. Os
dois estados históricos são aceitos: `(self,self)` depois de `list_init` e
`(NULL,NULL)` depois de `list_del`. `prev == next` continua válido para lista
vazia. Os wrappers validam `head` antes de avaliar `head->next` ou `head->prev`.

`list_del` valida o nó e seus dois links, depois exige
`entry->prev->next == entry` e `entry->next->prev == entry`. Apenas depois
disso executa a mutação anterior e deixa o nó em `(NULL,NULL)`. Double-delete,
par parcialmente nulo, dupla inserção e reciprocidade quebrada terminam antes
de qualquer store da operação rejeitada. A remoção estruturalmente válida de
um nó autoencadeado permanece aceita.

`queue.h` permanece byte-idêntico e herda as verificações por `list_add_tail`
e `list_del`. A auditoria encontrou consumidores em heap, scheduler,
runqueues/registry, timers, DPC e filas de espera. A transferência de timer
`list_del(&node->node)` seguida por `list_add_tail(..., &g_claimed_list)` usa o
estado `(NULL,NULL)` e passou na regressão. Nenhum consumidor, lock, layout ou
algoritmo de produção foi alterado.

Essas checagens pressupõem ponteiros reais, mapeados e legíveis. Um teste de
NULL ou reciprocidade não comprova que uma página arbitrária está mapeada. O
chamador continua responsável pela sincronização, inicialização e tempo de
vida; as listas não adquirem locks nem tentam reparar concorrência.

## Testes focais

A evidência final está em
[`20260906-000827-0300`](../../artifacts/build/runtime-assertions/20260906-000827-0300/).
O candidato instrumentado tem `kernel.elf` SHA-256
`2cafdf63823ff5871aa5bbd4dd0b2c626c635cda18e1c9eb17623958e8259bd5`
e imagem SHA-256
`349b9461aa501104d4a59fd3e979b0915ea16b83db01f7ce99761567187da429`.

| Prova | Cobertura | Resultado |
|---|---|---|
| Host normal | macros, listas, queue, snapshot sem store e 2048 sequências determinísticas (`seed=0x6173736572746c69`) | PASS, 19.550 asserções |
| Host UBSan/ASan | os mesmos headers reais e operações | PASS nos dois |
| Host desabilitado | sem avaliação de efeitos e sem referência aos helpers no objeto | PASS, 19.505 asserções |
| Controle adversarial | remove a reciprocidade do vizinho anterior em uma cópia do header | falha detectada, retorno 1 |
| Q35/SMP4/TCG | aviso, falso, lista válida, três violações, bug, heap lock e profundidades 2/3 | 10/10 PASS |
| Q35/SMP4/KVM | o mesmo conjunto central | 10/10 PASS |
| Q35/SMP1/TCG | contexto precoce, owner sem peers e bug terminal | PASS |
| Q35/SMP24/KVM | aviso/identidade, lista válida, scheduler lock e reentrada | 4/4 PASS |

Cada caso destrutivo usa uma fixture privada. O controlador salva seus 96 bytes
por GDB antes e depois da tentativa; dupla inserção e reciprocidade quebrada
mantiveram hashes iguais. Nos casos fatais, dois snapshots GDB separados por
execução liberada mostram todas as vCPUs em RIPs terminais, IF=0 e profundidade
1, 2 ou 3 conforme o cenário. QMP/GDB estavam conectados antes do disparo e o
cleanup ocorreu somente depois da classificação.

O caso SMP24 com lock do scheduler registrou aquisição real e `locked=1` no
snapshot posterior, ainda assim chegou ao halt nas 24 vCPUs. Os contextos de
runtime foram cruzados com topologia e task corrente lidas independentemente
por GDB enquanto o chamador permanecia pinado. O aviso confirmou continuação,
uma avaliação e IF=1 antes/depois; o teste precoce confirmou IF=0 antes/depois,
três avaliações e task inválida.

Uma campanha anterior, preservada em
[`20260905-235749-0300`](../../artifacts/build/runtime-assertions/20260905-235749-0300/),
executou os 25 casos, mas foi rejeitada porque, em SMP24, a task migrou entre o
snapshot inicial do host e a coleta do callsite. Esse resultado não foi
promovido a PASS. O teste final acrescenta uma barreira somente instrumentada
durante o intervalo pinado da captura, permitindo ao GDB observar CPU, slot e
handle no instante causal. A campanha final usa outro ELF e VMs novas.

O verificador aceitou um controle válido e rejeitou 14 mutações independentes:
caso ausente/duplicado, aviso sem continuação, polaridade invertida, callsite
incompleto, identidade divergente, mutação da fixture, ação apenas anunciada,
evento anterior ao trigger, cleanup do host como terminal, imagem debug
desligada, helper presente em produção, outro ELF e controle adversarial
indevidamente aceito.

## Builds pareados e produção

Os builds pareados usam a mesma árvore, toolchain e configuração comum, variando
somente `DEBUG_ASSERT`. O ELF ligado com asserts tem SHA-256
`c5e58405565f65b7e071010504df0df49ed98d643961b62f03a9b86a89a0ab0a`;
o desligado tem
`844b5cb46e0757aa5fd0bf29b082160eee9210d9033e3b6818b24832836c113b`.
O total de `size -A` é 15.868.777 bytes no primeiro e 15.861.481 no segundo.
A soma somente das seções com `SHF_ALLOC` é, respectivamente, 15.823.721 e
15.816.425 bytes. `.boot_data`, com 45.056 bytes, entra no primeiro total e não
possui `SHF_ALLOC`. A diferença é de 7.296 bytes nos dois critérios. Símbolos,
strings, referências e disassembly dos callsites confirmam os checks no perfil
ligado e sua ausência no desligado.

O kernel produzido por `make production-image` é byte-idêntico ao build
`DEBUG_ASSERT=0`, sem helpers de asserção, expressões de callsite, estado ou hook
de teste. O formatter de produto e o panic ordinário permanecem. A imagem final
tem SHA-256
`586c72b2732cc8356058e746d2790eb996d44cda6dc319d7d14e9aaa7c48b081`;
o ELF dentro da FAT corresponde ao candidato e os outros quatro payloads foram
conferidos. O maior frame automático observado foi 1808 bytes, abaixo do limite
de 2048.

## Regressão integrada

| Gate | Candidato e alcance | Resultado |
|---|---|---|
| `scripts/test-assert-list.sh all` | host, builds, 25 VMs, adversarial, produção e 14 negativos | PASS |
| `scripts/test-libc-format.sh all` | host, dois guests, cinco séries modal/heap, WIDE/COMPACT e oito negativos | PASS |
| `scripts/test-panic-lock-contention.sh all` | 38 positivos, um adversarial, ações externas e imagem normal | PASS |
| `scripts/test-foundation-regression.sh all` | seis perfis, 95 transações, produção, negativo e soak SMP24 | `PASS_WITH_NONAUTHORITATIVE_RATE_OBSERVATION` |
| `make kernel-check` / `make stack-check` | perfil de desenvolvimento e código instrumentado | PASS / PASS |

Na regressão, o candidato framed tem SHA-256
`2f852da73b4bb698b28b5fde595bf2749574a90ffeb9e0f0cac3b82e96454039`.
Todos os requisitos funcionais passaram, inclusive referência de timer,
listas em seus consumidores, TASKMAN e cinco checkpoints causais durante mais
de 300.000 ms. O controle KVM/SMP4, com 12 CPUs schedulable para quatro vCPUs,
registrou PASS (`worst_median_x1000=997`) e é autoritativo. KVM/SMP24
registrou FAIL bruto concluído (`worst_median_x1000=209`) e permanece
`NONAUTHORITATIVE_OVERSUBSCRIBED`; essa classificação não dispensou nenhum
resultado funcional. Na produção, SMP4 teve PASS (`996`) e SMP24 FAIL bruto
(`289`), ambos observados pelo retorno real do handler.

Os arquivos protegidos, inclusive `queue.h`, scheduler, timers, DPC, heap,
libc, IRQ, serial, console, graphics, linker, bootloader, scripts aprovados e
relatórios históricos, permaneceram byte-idênticos. A única correção no
relatório de formatação troca 4179 por 4183 para corresponder aos três logs
host aprovados.

A validação física continua diferida para o fechamento conforme a
[política de validação](validation-policy.md). O baseline preservado continua
com uma de três janelas, 3267 ms na primeira, sem mediana e com associação
operacional pendente; nenhum resultado virtual altera esse estado.
