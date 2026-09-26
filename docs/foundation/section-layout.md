# Layout de seções do kernel

## Contrato

O linker publica `_text_start`, `_text_end`, `_rodata_start`, `_rodata_end` e
`_data_start`. Os símbolos de fim são limites exclusivos do conteúdo. `.rodata`
e `.data` começam em endereços múltiplos de 4096; assim, os bytes de `.text`,
`.rodata` e `.data` não ocupam uma mesma página. `.data` e `.bss` continuam
podendo compartilhar uma página quando seus alinhamentos de entrada permitirem.

Esse contrato organiza o ELF. Ele não instala permissões de página, não aplica
RO, NX ou W^X e não altera a paginação do kernel. Flags de seção, flags de
`PT_LOAD` e permissões de PTE continuam sendo observações distintas.

## Inventário de entrada

A coleta final ligou 93 objetos, na mesma ordem, em cada perfil. O inventário
completo registra 2.582 entradas de seção entre os dois conjuntos de objetos em
[input-sections.json](../../artifacts/build/section-layout/20260906-014348-0300/runtime/layout-final-3/input-inventory/input-sections.json).
As famílias com payload de runtime são bootstrap (`.boot_text`, `.boot_data`),
código (`.text*`), trampoline (`.smp_trampoline_blob`), dados somente leitura
(`.rodata*`), dados mutáveis (`.data*`) e zero-fill (`.bss*` e `COMMON`).
Relocações são consumidas na linkagem. `.eh_frame*`, `.note*` e `.comment*` não
têm consumidor no runtime atual e são descartadas por regras nominais.

O mapa anterior da árvore atual mostra que o catch-all `.extra` recebia
`.comment`, `.note.GNU-stack`, `.note.gnu.property` e `.eh_frame`, além de
entradas sintéticas ou de relocação sem payload final. A regra `*(.*)` foi
retirada; nenhuma regra universal a substitui. Os mapas pareados estão em
[link-map](../../artifacts/build/section-layout/20260906-014348-0300/runtime/layout-final-3/link-map/).

O ELF do baseline inicial prova que sua seção de saída `.extra` tinha 65.192
bytes, mas não há mapa nem objetos históricos preservados para atribuir suas
entradas. A composição acima vem do mapa corrente e não é apresentada como
decomposição histórica do baseline.

## Fronteiras, páginas e carga

Os pares abaixo usam os mesmos objetos e ordem antes/depois; muda somente o
script. Endereços e tamanhos estão em hexadecimal.

| Perfil | Região | Antes: início/tamanho | Depois: início/tamanho |
|---|---|---|---|
| debug ligado | `.text` | `0xffffffff8200c000` / `0x81ade` | igual |
| debug ligado | `.trampoline` | `0xffffffff8208e000` / `0x178` | igual |
| debug ligado | `.rodata` | `0xffffffff8208f000` / `0x1384b` | igual |
| debug ligado | `.data` | `0xffffffff820a2860` / `0x42c` | `0xffffffff820a3000` / `0x42c` |
| debug ligado | `.bss` | `0xffffffff820a3000` / `0xe718a4` | `0xffffffff820a4000` / `0xe718a4` |
| debug ligado | `.extra` | `0xffffffff82f148a8` / `0x100d0` | ausente |
| produção | `.text` | `0xffffffff8200c000` / `0x80756` | igual |
| produção | `.trampoline` | `0xffffffff8208d000` / `0x178` | igual |
| produção | `.rodata` | `0xffffffff8208e000` / `0x1304b` | igual |
| produção | `.data` | `0xffffffff820a1060` / `0x42c` | `0xffffffff820a2000` / `0x42c` |
| produção | `.bss` | `0xffffffff820a2000` / `0xe718a4` | `0xffffffff820a3000` / `0xe718a4` |
| produção | `.extra` | `0xffffffff82f138a8` / `0xffd8` | ausente |

No perfil debug ligado, o padding entre fim de `.rodata` e início de `.data` é
1.973 bytes; em produção, 4.021 bytes. O padding entre `.data` e `.bss` é 3.028
bytes nos dois perfis. Esses valores são separados de payload, BSS e metadados;
o tamanho bruto do ELF não é tratado como medida de RAM ou desempenho.

O ELF de produção mantém entry `0x02000000`, base física `0x02000000`, delta
higher-half e envelope carregável até `0x02f148a4`. Há quatro `PT_LOAD`: bootstrap
RX; `.text` mais trampoline RX; `.rodata` R; e `.data` mais `.bss` RW. O loader
continua reservando e zerando o envelope físico antes de copiar cada payload.
`.boot_data` conserva os 45.056 bytes e a ausência pré-existente de `SHF_ALLOC`;
essa propriedade não foi criada nem alterada aqui.

## Trampoline

Nos dois pares, `_trampoline_start`, `_trampoline_end`, os 376 bytes do blob e
seu SHA-256
`bc95935ba23b30aff63cfd16c28d4c65dc13fd8fbdf9e8f099671845305c1189`
são idênticos. Os offsets preservados são: entry 0, GDT 288, GDTR 320, CR3 328,
CR4 336, EFER 344, stack 352, entry pointer 360, flag x2APIC 368 e base 372.
O fim continua apontando para o byte seguinte ao blob, antes do padding de
página. Os resultados pareados completos estão em
[debug-on](../../artifacts/build/section-layout/20260906-014348-0300/runtime/layout-final-3/pairs/debug-on/result.json)
e [debug-off](../../artifacts/build/section-layout/20260906-014348-0300/runtime/layout-final-3/pairs/debug-off/result.json).

## Política de órfãs e controles

Uma fixture assembly adicionou 16 bytes não vazios à seção `.layout_probe`.
GNU ld 2.42, invocado com `--orphan-handling=warn` somente na auditoria,
preservou-a sob o próprio nome e emitiu o aviso. O verificador encontrou o
payload no ELF e recusou o candidato como seção inesperada. Fonte, objeto, mapa,
ELF e comando estão em
[orphan-probe](../../artifacts/build/section-layout/20260906-014348-0300/runtime/layout-final-3/orphan-probe/).
O Makefile permanece inalterado; por isso, a rejeição automática de órfãs é
contrato do gate explícito, não de uma invocação incremental isolada de `make`.

O verificador aceitou o controle válido e rejeitou 15 mutações isoladas:
fronteira ausente ou fora da seção, fim inclusivo, página compartilhada,
alinhamento incorreto, trampoline movido/estendido/corrompido, patch fora do
blob, órfã absorvida/descartada, `PT_LOAD` inválido, outra identidade, binário
truncado e mapa incompatível. O recibo está em
[fixtures/results.json](../../artifacts/build/section-layout/20260906-014348-0300/runtime/layout-final-3/fixtures/results.json).

## Builds e regressão

| Gate | Resultado | Evidência |
|---|---|---|
| `make kernel-check` | PASS | [kernel-check.log](../../artifacts/build/section-layout/20260906-014348-0300/regression/kernel-check.log) |
| `make stack-check` | PASS, máximo 1808/2048 bytes | [stack-check.log](../../artifacts/build/section-layout/20260906-014348-0300/regression/stack-check.log) |
| `scripts/test-section-layout.sh all` | PASS; dois pares, cinco candidatos, órfã rejeitada | [section-gate.log](../../artifacts/build/section-layout/20260906-014348-0300/layout-summary/section-gate.log) |
| `scripts/test-assert-list.sh all` | PASS; 25 execuções | [assert-gate.log](../../artifacts/build/section-layout/20260906-014348-0300/regression/assert-gate.log) |
| `scripts/test-libc-format.sh all` | PASS; host, guest, modal/heap e WIDE/COMPACT | [format-gate.log](../../artifacts/build/section-layout/20260906-014348-0300/regression/format-gate.log) |
| `scripts/test-panic-lock-contention.sh all` | PASS; 38 positivos e um adversarial | [panic-gate.log](../../artifacts/build/section-layout/20260906-014348-0300/regression/panic-gate.log) |
| `scripts/test-foundation-regression.sh all` | FAIL focal no registro de TASKMAN SMP24 | [foundation-gate.log](../../artifacts/build/section-layout/20260906-014348-0300/regression/foundation-gate.log) |

Os seis perfis fizeram boot e o soak SMP24 completou cinco checkpoints. Cinco
perfis passaram a verificação completa. No SMP24, `taskmantest stats` retornou
zero e emitiu todos os campos, mas o serial quebrou o registro entre
`page_changes=` e seu valor `0`, nas linhas 1391–1392. O verificador recusou a
linha incompleta com `[TASKMANTEST][STATS] count is 0, expected 1`. Essa
atomicidade de diagnóstico pertence a fonte protegido nesta rodada. A execução
não foi repetida sem correção; a classificação e os bytes exatos estão em
[runtime-regression-classification.json](../../artifacts/build/section-layout/20260906-014348-0300/layout-summary/runtime-regression-classification.json).

A cadência concluiu causalmente: Q35/KVM/SMP4 passou como controle autoritativo
com 12 CPUs schedulable para quatro vCPUs; SMP24 teve FAIL bruto e permanece
não autoritativo sob 24 vCPUs nas mesmas 12 CPUs. Esse limite não é usado para
dispensar a falha distinta de registro. Os quatro perfis de diagnóstico de
produção passaram, e o panic simples da imagem normal teve ação externa
observada. Os cinco candidatos classificados pelo gate de layout e seus hashes
estão em [campaign.json](../../artifacts/build/section-layout/20260906-014348-0300/runtime/layout-final-3/campaign.json).

Nenhum fonte de runtime, loader, paginação, bootstrap, UI ou gate anterior foi
alterado. A validação física continua diferida para o fechamento e o baseline
físico anterior permanece parcial, com uma amostra e sem mediana.

## Fechamento do registro de estatísticas

O layout e os pares de trampoline acima permanecem validados. O linker conserva
o SHA-256
`3f200f9235dee68ecc00c72d6964a287604f35441a4ac906b7034fa2fb722fd8`.
A revalidação sobre os cinco novos candidatos aceitou dois pares, 2.582
entradas de seção e a rejeição da órfã real. Os resultados estão em
[section-layout](../../artifacts/build/diagnostic-records/20260906-123417--0300/regression/section-layout/).

A transação histórica 28 continua rejeitada. O serial de SHA-256
`df334cd069aaed1e9a948a4a4aca4cab83a857f97a41e21ff72fa220f986731d`
tem `page_changes=` na linha 1391 e seu valor no início da linha 1392; o recibo
de SHA-256
`6ddbe250984c69cd9d2abe2359dab0058c1bc5a0e758925dd0d7ccf45bb19cb7`
registra retorno zero do handler. BEGIN, END e retorno preservam a causalidade
do despacho, mas não tornam as duas linhas um registro completo. A cópia
imutável e a rejeição pelo verificador anterior estão em
[historical](../../artifacts/build/diagnostic-records/20260906-123417--0300/historical/).
O emissor anterior fazia dezenas de chamadas independentes à serial; o probe
hospedado confirma que esse protocolo permitia intercalação. Os dados antigos
não identificam qual escritor produziu o CRLF observado.

O emissor atual obtém um snapshot de `taskman_stats_t` e uma leitura separada
de `auto_exit_pending`, monta os 52 campos na ordem existente e só então chama
`serial_write_all` uma vez. A capacidade fixa de 1.953 bytes cobre uma LF
inicial, 921 bytes literais, 51 inteiros de até 20 dígitos, o maior nome de
modo, a LF final e o NUL. O armazenamento é privado por invocação, alocado
fora dos locks do snapshot e da serial e liberado antes do retorno. Falha de
alocação ou de formatação emite somente `STATS_ERROR` e faz o subcomando
retornar erro; nenhum prefixo `STATS` parcial é publicado.

O gate focal executou nove casos hospedados nos perfis normal, UBSan e ASan,
dez negativos independentes do verificador e seis VMs KVM independentes: três
Q35/SMP4 e três Q35/SMP24. Cada VM produziu um registro com os 52 campos dentro
da transação framed e retorno exato zero, durante atividade concorrente de
workers com cleanup observado. A evidência e o replay estão em
[diagnostic-records](../../artifacts/build/diagnostic-records/20260906-123417--0300/).

A regressão corrente passou nos seis perfis. No SMP24, a nova transação de
`taskmantest stats` ficou completa e o handler retornou zero; o perfil de
produção SMP4 observou o retorno real pelo debugger. O controle SMP4 de
cadência passou com autoridade; o SMP24 concluiu com FAIL bruto e continua
não autoritativo por executar 24 vCPUs sobre 12 CPUs schedulable. Formatação,
modal/heap, WIDE/COMPACT e panic passaram sobre os candidatos novos.

O gate de asserções encontrou uma falha distinta e fora do escopo deste
fechamento: em `q35-kvm-smp24-valid-list-none`, o marker protegido
`[ASSERT_TEST][EXECUTE] scenario=3` foi intercalado por um breadcrumb de boot.
As 25 execuções planejadas foram preservadas, sem repetir a amostra; 24 foram
aceitas e o verificador recusou essa execução como `RUN_STATUS_INVALID`. O
resultado bruto e os bytes do serial estão em
[assertions](../../artifacts/build/diagnostic-records/20260906-123417--0300/regression/assertions/).
Essa falha impede declarar a campanha agregada e o replay integralmente verdes;
ela não reclassifica o registro de TASKMAN nem autoriza alterar o gate ou o
emissor de asserções nesta mudança focal.

Nenhuma permissão RO, NX ou W^X foi instalada. A validação física continua
diferida para o fechamento, sem alterar o baseline parcial existente.

## Integridade dos registros do selftest de asserções

A execução histórica `q35-kvm-smp24-valid-list-none` permanece rejeitada. Seu
serial, de SHA-256
`edb24e993237e2cf5c17ebb1067ce09cc5e0cd9c83d21960b929f15ec00b6427`,
tem o início de `EXECUTE` na linha 790, intervalo de bytes `[41467,41577)`, e
o valor `3` isolado na linha 791, intervalo `[41577,41580)`. O breadcrumb que
aparece entre `scenario=` e o valor demonstra a intercalação do registro; não
identifica qual CPU escreveu o breadcrumb. O estado GDB e `COMPLETE` mostram
que a operação terminou, mas não completam o campo ausente. O verificador
preservado continua recusando a execução com `EXECUTE_SCENARIO_INVALID`. A
cópia textual e os offsets estão em
[historical](../../artifacts/build/assertion-records/20260906-151711--0300/historical/).

Os quatro registros dinâmicos do mesmo selftest, `PREPARED`, `EXECUTE`,
`CONTEXT_READY` e `COMPLETE`, agora são construídos por `ksnprintf` antes do
primeiro byte de saída. Cada registro inclui uma LF inicial de delimitação, os
campos e ordem anteriores, a LF final e o NUL, e é entregue por uma única
chamada a `serial_write_all`. Com todos os campos `uint32_t` no máximo, as
capacidades exatas necessárias são 80, 45, 53 e 58 bytes, respectivamente. O
buffer privado de 96 bytes cobre o maior caso sem heap, VLA ou estado de saída
compartilhado. Retorno negativo ou truncamento marca `arm_error`, preserva
resultado falso, emite somente `RECORD_ERROR` e impede que o cenário prossiga
para um resultado favorável.

As fronteiras de contexto não mudaram. `PREPARED` ainda publica o estado com
IF salvo antes de aguardar o observador; `EXECUTE` ocorre depois da restauração;
e `CONTEXT_READY` continua ligado à coleta de contexto do caminho de asserção.
O formato não altera o estado de teste de 144 bytes, a lista, o panic ou o
driver serial. O código permanece selecionado somente por `ASSERT_TEST=1`.

O gate focal compilou o produtor real e o formatter do kernel nos perfis
normal, UBSan e ASan. Cada perfil passou nove casos e 82 verificações, incluindo
zeros, máximos, capacidade exata e insuficiente, canários, uma emissão e um
controle fragmentado com escritor intercalado. O controle válido e nove
negativos de parser passaram; o histórico original continuou rejeitado. Os
resultados estão em
[assertion-records](../../artifacts/build/assertion-records/20260906-152746--0300/).

A matriz nova de asserções passou nas 25 execuções planejadas, nos quatro
perfis e dez cenários, sobre o único ELF instrumentado de SHA-256
`945a6505d310f60982fab2040871b8f93b90c47be21423ff9274745ca0d814e3`.
O caso SMP24 antes problemático publicou registros íntegros e terminou com
`arm_error=0`; o serial novo tem SHA-256
`429aec90d1096dca29f5e0eff7d003621165ab50c0479985164f843a2d199401`.
O verificador de asserções permaneceu byte-idêntico, aceitou os 25 casos e
rejeitou seus 14 controles negativos. A seleção textual da campanha está em
[matrix-text](../../artifacts/build/assertion-records/20260906-151711--0300/matrix-text/).

O verificador de layout aceitou diretamente o novo ELF e a coleta com os cinco
papéis, dois pares, 2.582 entradas e a órfã rejeitada. O linker permanece no
SHA-256 aprovado. Nos perfis sem `ASSERT_TEST`, os ELFs debug ligado e produção
foram reconstruídos e ficaram byte-idênticos aos anteriores, nos SHA-256
`e1bcf4438ac370c24851cc3a923481ae4c6b18ad0d696b0cb143146178c13dfc`
e `92307fe1a1b19c4d741a7cf5c620ee87508e0be566c657633c141e612cebd4cd`.
Os cinco payloads da FAT também são idênticos; somente o container recriado
mudou. Formatação, estatísticas, layout, regressão corrente, produção, controle
negativo de timer e panic foram reexecutados offline sobre seus candidatos
originais e passaram. O mapa de reuso está em
[reuse-map.json](../../artifacts/build/assertion-records/20260906-151711--0300/reuse/reuse-map.json).

O pacote de revisão desta correção contém fontes e evidência textual focal. Os
ELFs, imagens e snapshots binários permanecem no ambiente local, portanto o
pacote leve não oferece replay binário integral. A validação física continua
diferida para o fechamento.
