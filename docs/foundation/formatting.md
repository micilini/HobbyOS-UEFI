# Formatação limitada por capacidade

## Contrato entregue

`ksnprintf` e `kvsnprintf` escrevem em uma região cuja capacidade inclui o
terminador. Com `size > 0`, o destino válido recebe no máximo `size - 1` bytes
de payload e sempre termina com NUL. Com `size == 0`, nenhuma escrita nem
aritmética de armazenamento é feita sobre `dst`, e `dst == NULL` é aceito. O
retorno de sucesso é o comprimento que seria necessário sem o NUL; portanto,
`ret >= 0 && (size_t)ret >= size` indica truncamento.

As funções retornam `-1` para `fmt == NULL`, para `dst == NULL` com capacidade
positiva, para sintaxe não suportada, para largura não representável ou quando
o comprimento necessário ultrapassa `INT_MAX`. Se o destino for válido e tiver
capacidade positiva, um erro deixa `dst[0] == '\0'`. Não há `errno` global.
Um `%c` pode produzir um byte zero que ainda conta no comprimento.

O contrato pressupõe destino gravável em toda a extensão informada, formato e
strings legíveis e ausência de sobreposição entre destino, formato e entradas.
A capacidade não autentica endereços. A API é um subconjunto local e não
declara conformidade integral com ISO C ou POSIX.

| Conversão | Tipo variádico e representação |
|---|---|
| `%%` | percentual literal |
| `%s` | `const char *`; NULL produz `(null)` |
| `%c` | `int` promovido |
| `%d`, `%u`, `%x`, `%X` | `int` ou `unsigned int` |
| `%lld` | `long long`, inclusive `LLONG_MIN` |
| `%llu`, `%llx`, `%llX` | `unsigned long long` |
| `%p` | `void *`; `0x` e `2 * sizeof(uintptr_t)` dígitos minúsculos |

A largura é decimal literal e mínima. O preenchimento padrão usa espaços;
`0` desloca zeros para depois do sinal ou do prefixo em inteiros e ponteiros.
Em strings e caracteres, `0` continua significando espaços. Precisão, `*`,
argumentos posicionais, ponto flutuante, locale, caracteres largos, `%n`, as
flags `#`, `+`, espaço e `-`, e modificadores fora de `ll` são rejeitados sem
consumir o argumento da conversão rejeitada. Um percentual final isolado é
erro.

`kvsnprintf` usa `va_copy` e `va_end`, de modo que não consome a lista recebida
pelo chamador. As declarações usam o atributo `format(printf, ...)` quando o
compilador o oferece; o parser ainda impõe o subconjunto menor em runtime.

## Limites internos

O estado `format_writer` separa bytes necessários de bytes armazenados. Não há
heap, locks, console, serial, relógio, scheduler, estado global mutável, locale,
recursão ou buffer compartilhado. A conversão numérica usa um array automático
fixo de 64 bytes. Magnitudes negativas são obtidas por subtração no domínio
sem sinal, sem avaliar `-INT_MIN` ou `-LLONG_MIN`.

O contador rejeita qualquer soma acima de `INT_MAX`. Padding descartado é
contado de uma vez e só itera sobre o espaço efetivamente gravável; assim,
largura `INT_MAX` com destino zero ou pequeno termina em tempo limitado. O
legado `itoa(int, char *)` preserva a ABI e agora converte `INT_MIN` sem
overflow; seu chamador ainda precisa fornecer espaço suficiente. A saída de
`k_int_to_hex` permaneceu inalterada.

## Consumidores auditados

| Arquivo | Estado anterior | Destino desta alteração |
|---|---|---|
| `core/kernel_init.c` | dois appenders sem capacidade montavam o breadcrumb | uma chamada limitada monta a linha completa; um `serial_write_all` preserva o marker atômico |
| `memory/pmem.c` | helper hexadecimal local e linhas emitidas em fragmentos | linhas dinâmicas usam `ksnprintf`, caixa/largura/prefixos históricos e valores preservados |
| `memory/heap.c` | dois conversores locais sem chamadas | helpers mortos removidos; algoritmo e diagnósticos vivos intactos |
| `memory/gdt.c` | conversor hexadecimal local sem chamadas | helper morto removido; descritores e textos constantes intactos |
| `memory/paging.c` | somente textos constantes no escopo auditado | arquivo byte-idêntico; nenhuma chamada artificial ao formatter |

O maior nome de etapa vivo, `PCI_SCAN_COMPLETE`, combinado com dois valores
`UINT64_MAX`, requer 113 bytes de payload; o buffer de 176 bytes comporta a
linha e o NUL. O caminho de erro não publica breadcrumb parcial: emite
`[BOOT][PROGRESS] FORMAT_ERROR` como texto constante e usa a política de falha
de boot existente. O selftest focal reduz a capacidade para demonstrar a
detecção de truncamento antes da inicialização do PMM.

As linhas PMM mantêm a representação `0x` seguida por 16 dígitos maiúsculos,
inclusive os prefixos duplicados já existentes em dois diagnósticos. Reservas,
bitmap, contadores, locks e cálculos de span não mudaram.

## Utilitários retirados

A busca em fontes C, headers, assembly e Makefile não encontrou consumidores
compilados de `k_memset`, `k_memcpy` ou `k_delay`. Os dois arquivos de
`kernel/src/utils/` foram excluídos, a entrada correspondente saiu de
`KERNEL_SRCS`, e somente os includes sem uso de `console.c` e `terminal.c`
foram removidos. As utilities independentes do bootloader e seu objeto no link
permanecem. O catálogo do README agora atribui `memcmp` a `memory.h`, remove o
`strncpy` inexistente e lista a nova API limitada.

## Testes focais

A evidência final do formatter está em
[`format-final-3`](../../artifacts/build/libc-format/20260905-201958-0300/runtime/format-final-3/).
O manifesto de fontes fixa os bytes testados, independentemente do commit
criado depois da campanha.

| Prova | Cobertura | Resultado |
|---|---|---|
| Host normal | capacidades, truncamento, conversões, canários, erros e 4096 casos determinísticos (`seed=0x5eedf04a7c9b312d`) | PASS, 4183 asserções |
| Host UBSan | mesma implementação real de `string.c`, inclusive mínimos assinados | PASS |
| Host ASan | regiões e canários da mesma implementação | PASS |
| Guest Q35/SMP1/TCG | 22 casos antes dos alocadores | PASS |
| Guest Q35/SMP4/KVM | os mesmos 22 casos e candidato identificado | PASS |
| Oráculo | um controle válido e oito mutações independentes | PASS; 8/8 rejeitadas |

Os negativos cobrem caso ausente/duplicado, canário, NUL, retorno de
truncamento, identidade de binário, conclusão e confusão com adversarial. O
runner host compila o `string.c` do checkout com símbolos legados isolados por
macros apenas nesse objeto; nenhuma renomeação entra no kernel.

## Consumidores e apresentação

O gate corrente executou duas VMs KVM/SMP4 e três VMs KVM/SMP8 independentes,
cada qual com 1000 ciclos modal, concorrência `smpstress`, handles e gerações
reconciliados, sweep e gate externo de heap sem deriva. Uma tentativa
intermediária conservada mostrou que um predicado host avaliava deriva global dentro
do stress; o handler declara esse valor `GLOBAL_DIAGNOSTIC` e o gate autoritativo
é o par externo `heap-begin`/`heap-end`. A coleta final aplica esse protocolo
existente sem enfraquecer os predicados de ownership, reaper ou resíduos.

TASKMAN foi observado em Q35/KVM/SMP4 nas geometrias WIDE de 118 colunas e
COMPACT de 76 colunas. Os dois casos preservam PPM real, frames completos,
saída por ESC, restauração da geometria e checks de modal/input/task/heap. A
revisão visual humana continua separada.

Os harnesses históricos foram invocados uma vez. O gate modal/heap retornou 1
porque falta seu arquivo retrospectivo congelado
`artifacts/build/cl13fix12-soak8-kvm-failed.log`; o focused visual retornou 1
no preflight fixado em outra branch/commit. Esses retornos brutos permanecem
em [`legacy`](../../artifacts/build/libc-format/20260905-201958-0300/legacy/)
e não foram chamados de PASS.

## Regressão integrada

| Gate | Candidato e alcance | Resultado |
|---|---|---|
| `scripts/test-libc-format.sh all` | host, builds, guest, modal/heap, WIDE/COMPACT e negativos | PASS |
| `scripts/test-panic-lock-contention.sh all` | 38 cenários positivos e um adversarial, com QMP/GDB e imagem normal | PASS |
| `scripts/test-foundation-regression.sh all` | seis perfis, comandos framed/produção, controle negativo e soak SMP24 | `PASS_WITH_NONAUTHORITATIVE_RATE_OBSERVATION` |

O gate de panic recompilou seus candidatos a partir do novo `string.c`; a
implementação de panic e seus quatro fontes permaneceram byte-idênticos. A
regressão usa HPET somente como clocksource, Timer0 quiescente e LAPIC do BSP
como clockevent global de 1000 us. O resultado bruto e a autoridade de cadência
são campos separados. Com 12 CPUs schedulable, o controle KVM/SMP4 retornou
PASS e foi autoritativo; KVM/SMP24 concluiu na transação correta com FAIL
bruto e classificação `NONAUTHORITATIVE_OVERSUBSCRIBED`. Os demais requisitos
funcionais passaram, inclusive cinco checkpoints causais durante pelo menos
300000 ms de soak. A evidência está em
[`foundation-regression`](../../artifacts/build/libc-format/20260905-201958-0300/runtime/foundation-regression/).

## Produção, integridade e limites

`HOBBYOS_FORMAT_TEST` exige `HOBBYOS_SELFTEST` no build. O candidato de
produção não contém os casos, expected strings, estados ou chamadas do
selftest, nem as injeções de panic. Ele contém `ksnprintf` e `kvsnprintf` como
API de produto. O maior frame automático observado foi 1808 bytes, abaixo do
limite de 2048 bytes. O `kernel.elf` de produção tem SHA-256
`7b44ccf02f0ede99e9f001301831ec495373ec66a98766b0cc9b651ae09c9cba` e é
byte-idêntico aos kernels normais exercitados pelos gates de panic e regressão.
Os cinco payloads extraídos também são byte-idênticos aos da imagem exercitada
pela regressão; o hash bruto da FAT recriada pode divergir por seus metadados.
Símbolos, referências indefinidas, disassembly, frames automáticos e payloads
estão conferidos na evidência de build.

Fora das duas remoções de include autorizadas, os fontes de graphics e
terminal permaneceram byte-idênticos. Scheduler, reaper, IRQ, timers, clock,
SMP, drivers, paginação, linker, bootloader, assets, panic e seus verificadores
não foram alterados nesta implementação.

A validação física continua diferida para o fechamento conforme a
[política de validação](validation-policy.md). O baseline preservado continua
com uma de três janelas, 3267 ms na primeira, sem mediana e com associação
operacional pendente; nenhum resultado QEMU modifica esse estado.
