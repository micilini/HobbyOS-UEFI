# Baseline de build e boot

## Estado da coleta

```text
COLLECTION_STATUS: BASELINE_PARTIAL
SCOPE_STATUS: DOCUMENTATION_ONLY
SOURCE_INTEGRITY_STATUS: PASS
QEMU_STATUS: BOOT_SAMPLES_VALID_SHELL_CAPTURE_RECORDED
QEMU_VALID_SAMPLES: 3/3
QEMU_BOOT_MEDIAN_MS: 186
BAREMETAL_STATUS: PARTIAL_CAPTURE_RECORDED
BAREMETAL_BOOTS_OBSERVED_IN_SUBMISSION: 1
BAREMETAL_VALID_TIMESTAMP_SAMPLES: 1/3
BAREMETAL_VALID_SAMPLES: 1/3
BAREMETAL_BOOT_SAMPLE_MS: 3267
BAREMETAL_BOOT_MEDIAN_MS: NOT_MEASURED
BAREMETAL_SHELL_CAPTURE_STATUS: COMPLETE_REVIEWED_TRANSCRIPTION_RECORDED
EVIDENCE_TRANSPORT: INLINE_MARKDOWN
SHELL_TRANSCRIPTION_SOURCE: PREVIOUS_PHOTO_REVIEW_EMBEDDED_TEXT
PHOTO_LOCAL_VERIFICATION: NOT_PERFORMED_TEXT_ONLY
CAPTURE_SOURCE_KIND: USER_SUPPLIED_TERMINAL_TRANSCRIPT
CANDIDATE_BINDING_STATUS: OPERATOR_CONFIRMATION_PENDING
GATE_PORTABILITY_STATUS: HARNESS_BASELINE_MISMATCH
REVIEW_STATUS: AWAITING_REVIEW
```

Há três medições válidas da janela de boot em QEMU, saídas reais de `mem` e
`cpu` em duas VMs e uma captura física recebida, com janela de 3267 ms.
A saída física dos dois comandos está registrada por transcrição revisada
da foto, transportada como texto. Faltam dois boots físicos independentes
e a confirmação da associação do boot recebido ao candidato de referência.
As contagens físicas acima descrevem integridade temporal observada, não
autenticam o ELF. Os dois harnesses completos pararam no preflight.
Este registro não certifica interrupções, clockevent, hardware físico nem
aceite pelo mantenedor.

## Identidade e integridade

| Campo | Valor |
|---|---|
| Checkout | `/home/william-lima/Área de trabalho/HobbyOS-UEFI` |
| Identificador temporal da coleta | `20260904-220652-0300`, obtido do relógio local |
| Fuso | `America/Sao_Paulo`, UTC−03:00; registros ISO 8601 |
| Início do registro operacional | 2026-09-04T22:08:47.871882-03:00 |
| Branch anterior | `Kernel-Version` |
| Branch criada do HEAD atual | `feat/foundation-hardening` |
| Commit de entrada | `998cb001f19cc3a7a9218f6d47069d8b57b006f6` |
| Tree de entrada | `3de343a6ffb692c42bb30e38ad793a1e8551e339` |
| Worktree e index iniciais | Limpos; status, diff e diff staged vazios |
| ZIP recebido, nome informado | `hobbyos(20260905-005651).zip` |
| Arquivo local confrontado | `hobbyos.zip`, na raiz do checkout |
| SHA-256 do ZIP, medido | `f5a8e513a338e4594f77d6759009b2f8503a3329a6113bad2893dd838257486c` |
| Inventário ZIP, medido | 387 arquivos regulares; 419 entradas; 4.296.042 bytes descompactados |
| Confronto arquivo a arquivo | 387/387 idênticos; divergentes 0, ausentes 0, adicionais versionados 0 |
| Documento externo, SHA-256 medido | `b94d4efc1fd3d63ec3b2045d5c4e83b571dd56305b6f729978b16c7b438ecdd2` |
| `diagnostic.md` | Ausente no ZIP e nos arquivos versionados; findings não auditados não são confirmados |

O `AGENTS.md` real foi lido integralmente antes de alterações de estado.
O ZIP foi lido sem extração sobre o checkout; não contém histórico Git,
`kernel.elf` nem `hobbyos.img`. O HEAD foi obtido do Git real.

Evidências sob `artifacts/build/foundation-baseline/20260904-220652-0300/`
(abreviado **E**): [confronto com o ZIP](../../artifacts/build/foundation-baseline/20260904-220652-0300/source-zip-comparison.json),
[manifesto inicial](../../artifacts/build/foundation-baseline/20260904-220652-0300/source-before.json),
[manifesto após coleta](../../artifacts/build/foundation-baseline/20260904-220652-0300/source-after-collection.json) e
[comparação de integridade](../../artifacts/build/foundation-baseline/20260904-220652-0300/integrity-after-collection.json).
Os 387 arquivos de entrada conservaram conteúdo, modo e identificação Git.
A única inclusão versionada é este relatório. Identidade do commit entregue,
parent, tree, bundle e ZIP pertencem ao recibo externo pós-commit.

A integração textual da captura física tem identificador
`20260905-003339-0300`, obtido do relógio local, e base documental
`ae3539d765f7185b130ecb265177a0394917dca6`. Seus
[registros de entrada](../../artifacts/build/foundation-baseline/20260905-003339-0300/baremetal/run-01/preflight.json)
e [manifesto de fontes](../../artifacts/build/foundation-baseline/20260905-003339-0300/baremetal/run-01/sources-before.json)
identificam 388 arquivos versionados correspondentes à base e worktree/index
limpos. Somente este relatório recebe atualização documental; as medições
de build, warnings, seções e QEMU abaixo permanecem as da coleta original.

## Ambiente e perfis

| Componente | Ambiente efetivamente usado |
|---|---|
| Host | Linux x86_64; Intel(R) Core(TM) 7 150U |
| CPUs schedulable | 12, afinidade 0–11; sem alteração de afinidade |
| Memória do host | 33.320.792.064 bytes totais; 29.005.336.576 disponíveis no inventário |
| GCC efetivo | 13.3.0, Ubuntu 13.3.0-6ubuntu2~24.04.1, `/usr/bin/gcc` |
| GNU binutils / Make | 2.42 (pacote 2.42-4ubuntu2.8) / 4.3 |
| Python | 3.12.3 |
| QEMU | 8.2.2; pacote 1:8.2.2+ds-0ubuntu1.16 |
| GNU-EFI | 3.0.15-1build1; headers `/usr/include/efi`, bibliotecas `/usr/lib` |
| mtools | 4.0.43-1build1 |
| Firmware | `/usr/share/ovmf/OVMF.fd`; pacote OVMF 2024.02-2ubuntu0.7 |
| SHA-256 do firmware | `603ce35bac668391d946fd0a0cb2e255ea5569db9d35530252e468bbbb8563c3` |
| KVM | `/dev/kvm` acessível para leitura/escrita; KVM efetivamente lançado |
| Perfil QEMU | `MACHINE=q35 SMP=4 MEM=2G ACCEL=kvm`; CPU `host`; 1 socket, 4 cores, 1 thread/core |
| Firmware no lançamento | `OVMF_FD=/usr/share/ovmf/OVMF.fd`, modo `-bios` |
| TCG_THREAD | `multi` no ambiente/launch.env; não aplicável à execução KVM |
| Dispositivos | Sem rede; NEC xHCI, MSI ligado/MSI-X desligado, teclado USB; serial/debugcon/trace em arquivos |

Defaults efetivos: `SELFTEST=0`, `SELFTEST_AUTORUN=0`,
`KERNEL_EXTRA_CFLAGS=`, `JOBS=2`, `ARCH=x86_64`, `CC=gcc`, `LD=ld`
e `OBJCOPY=objcopy`. Não havia variáveis relevantes herdadas. O coletor
remove overrides de build/QEMU e flags de Make dos comandos e fixa
`LC_ALL=C`, `TZ=America/Sao_Paulo`; a lista consta em `E/collect.py` e
o ambiente relevante inicial em `E/collection.json`. Não houve instalação
de dependências nem override de toolchain.

O build normal conservou flags freestanding, `-Wall` e os três `-Werror=`
existentes, sem opção `-O` explícita. O stack check acrescentou
`-fstack-usage`; warnings usaram o hook abaixo. Produção fez outro clean
build com `SELFTEST=0 SELFTEST_AUTORUN=0 KERNEL_EXTRA_CFLAGS=`.
O limite de stack permaneceu em 2048 bytes e o LAPIC em 1000 us.

Não foi identificado perfil específico de baseline local; foram adotados
Q35, quatro vCPUs e 2 GiB do Makefile, com KVM selecionado antes da série.
As 12 CPUs disponíveis excedem as quatro vCPUs, sem oversubscription por
contagem. Isso não certifica cadência LAPIC ou guest de 24 vCPUs.
O teste de taxa LAPIC não foi executado; seu threshold 700 permaneceu intacto.

Inventários: [pacotes](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/package-versions.log),
[CPU](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/host-cpu.log), [memória](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/host-memory.log),
[host e perfil](../../artifacts/build/foundation-baseline/20260904-220652-0300/host.json). Os `capture.json` guardam argv real
de QEMU, afinidade, carga do host, PID de ownership e identidade do candidato.
O Linux efetivo está em `E/commands/host-kernel.log`.

## Gates e cobertura

| Comando | Bruto | Exit code | Classificação | Cobertura executada | Evidência |
|---|---|---:|---|---|---|
| `make kernel-check` | PASS | 0 | PASS | Clean build; 90 C + 4 assembly | [bruto](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/kernel-check.log) / [registro](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/kernel-check.json) |
| `make stack-check` | PASS | 0 | PASS | Clean build com -fstack-usage; limite 2048 bytes | [bruto](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/stack-check.log) / [registro](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/stack-check.json) |
| `scripts/test-interrupt-bringup.sh all` | FAIL | 1 | HARNESS_BASELINE_MISMATCH | PREFLIGHT_ONLY; runtime NOT_EXECUTED | [bruto](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/interrupt-all.log) / [registro](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/interrupt-all.json) |
| `scripts/test-timer-clockevent.sh all` | FAIL | 1 | HARNESS_BASELINE_MISMATCH | PREFLIGHT_ONLY; runtime NOT_EXECUTED | [bruto](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/timer-all.log) / [registro](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/timer-all.json) |

`make deps-check`: PASS, exit 0, escopo `all`, com toolchain, imagem e
QEMU disponíveis ([log](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/deps-check.log)).
`make production-image`: PASS, exit 0
([log](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/production-image.log)), incluindo:

```text
[BOOT][PRODUCTION_TEST_POLICY] PASS autorun=0 tasktest=0 manual_diagnostics=1 breadcrumbs=5
```

O stack check examinou 90 arquivos de uso de stack: `violations=0`.
Maior frame reportado: 1968 bytes, `timer_ref_test_ex`,
`kernel/src/shell/commands/cmd_reaptest.c:284`. Isso não mede o pico
acumulado de uma cadeia de chamadas em runtime. Os `.su` foram preservados.

As invocações foram individuais e em série. O log reutilizável
`artifacts/build/kernel-check-j2.log`, junto com o ELF, foi copiado após cada
execução para `E/preserved/{kernel-check,stack-check,warnings}/`, antes
do clean seguinte. O coletor captura stdout/stderr diretamente, verifica
fechamento/fsync e registra o retorno real do processo, sem `tee`.
Os `tee` internos dos scripts existentes não foram modificados; seus estágios
internos não receberam nova instrumentação. A política de produção foi
preservada também a partir de seu diretório canônico.

| Registro | Início | Fim | Exit code |
|---|---|---|---:|
| deps-check | 2026-09-04T22:09:35.889027-03:00 | 2026-09-04T22:09:35.963812-03:00 | 0 |
| kernel-check | 2026-09-04T22:09:36.013171-03:00 | 2026-09-04T22:09:37.785367-03:00 | 0 |
| stack-check | 2026-09-04T22:09:59.299805-03:00 | 2026-09-04T22:10:01.230272-03:00 | 0 |
| interrupt-all | 2026-09-04T22:10:27.774373-03:00 | 2026-09-04T22:10:27.800416-03:00 | 1 |
| timer-all | 2026-09-04T22:10:33.274720-03:00 | 2026-09-04T22:10:33.316538-03:00 | 1 |
| warnings | 2026-09-04T22:10:52.862024-03:00 | 2026-09-04T22:10:54.734235-03:00 | 0 |
| production-image | 2026-09-04T22:11:08.116309-03:00 | 2026-09-04T22:11:12.605574-03:00 | 0 |

Cada `E/commands/*.json` contém comando/argv completo, diretório, perfil,
início/fim, limite de espera, código de saída, classificação e hash do bruto.

### Preflight vinculado ao histórico

Os dois `all` originais produziram logs vazios e exit 1. Esses brutos e os
respectivos `preflight.log` foram preservados antes de qualquer nova invocação.
Uma captura diagnóstica adicional de cada `bash -x ... all`, também exit 1,
mostrou a primeira condição rejeitada:

```text
[[ feat/foundation-hardening == feat/taskman ]]
```

Âncoras locais: `scripts/test-interrupt-bringup.sh:344` e
`scripts/test-timer-clockevent.sh:792`.
[Trace de interrupt](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/interrupt-trace.log) e
[trace de timer](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/timer-trace.log) preservam a falha.

A leitura confirmou HEADs incompatíveis entre si:
`602532db881f5e328a78efdb52b0c012f35b4065` para interrupt e
`523f367c9cd8652e709c0ffc7af28f6433d1213c` para timer, além de tree,
parent, documento e manifestos históricos. O timer fixa diretórios internos,
inclusive `artifacts/build/ap-runtime-rendezvous`. Nenhum override,
manifesto fabricado, troca de branch ou bypass foi usado. As condições
posteriores ao nome da branch não chegaram a ser executadas.

Para ambos: `resultado_bruto=FAIL`,
`classificacao=HARNESS_BASELINE_MISMATCH`,
`escopo_executado=PREFLIGHT_ONLY`, `validacao_runtime=NOT_EXECUTED`.
Um bruto vazio não representa sucesso. Os scripts tinham modo versionado
`100755`; não houve erro de permissão. A portabilidade desses gates exige
decisão do mantenedor antes de exigir os mesmos `all` verdes em alterações
futuras. Nenhum contrato de teste foi alterado.

## Inventário de warnings

Coleta independente:

```bash
make kernel-check KERNEL_EXTRA_CFLAGS="-Wextra -Wshadow -Wundef -Wmissing-prototypes -Wframe-larger-than=2048 -Wvla"
```

PASS, exit 0, clean build confirmado: 90 compilações C com todas as seis
opções acrescentadas e quatro compilações assembly. Cobertura concluída, sem
erro fatal. [Bruto](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/warnings.log) e
[inventário por diagnóstico e comando GCC](../../artifacts/build/foundation-baseline/20260904-220652-0300/warnings.json).

| Categoria emitida pelo compilador | Ocorrências | Locais únicos |
|---|---:|---:|
| `-Waddress-of-packed-member` | 32 | 32 |
| `-Wdiscarded-qualifiers` | 1 | 1 |
| `-Wmissing-prototypes` | 55 | 55 |
| `-Woverride-init` | 4 | 4 |
| `-Wswitch` | 2 | 1 |
| `-Wunused-but-set-variable` | 6 | 6 |
| `-Wunused-function` | 15 | 15 |
| `-Wunused-parameter` | 4 | 4 |
| `-Wunused-variable` | 3 | 3 |
| **Total do compilador** | **122** | **121** |

Local único: `(caminho, linha, coluna, categoria)`. Duas mensagens
`-Wswitch` no mesmo local contam como duas ocorrências e um local.
Notas de contexto e comandos GCC não entram; categorias de `-Wall`
também entram. Não houve warning do compilador sem categoria, warning não
reconhecido pelo parser ou erro fatal. Não foram emitidos diagnósticos
`-Wshadow`, `-Wundef`, `-Wframe-larger-than=` ou `-Wvla`.

Separadamente, o linker emitiu duas ocorrências: falta de `.note.GNU-stack`
em `entry.o`, implicando stack executável, e segmento LOAD com RWX.
A nota de depreciação do linker não foi contada como warning.
Os maiores grupos são protótipos ausentes (55) e endereço de membro packed
(32); seus possíveis efeitos em runtime não foram auditados nesta coleta.

## ELF e imagem de referência

Perfil: `make production-image`, política de produção PASS. Cópias feitas
antes do QEMU, somente leitura no host, em `E/reference/`.

| Artefato | Bytes do arquivo | SHA-256 |
|---|---:|---|
| kernel.elf | 15977048 | `a6c51ad711d3baf1702159de7a872daa752537142d7e8ae7de704b26e4cbe3a5` |
| BOOTX64.EFI | 70785 | `3f0665e6a85825b4821d36f37c2f3d9f7e1f63c37b6fac726158ee66d7133b66` |
| hobbyos.img | 67108864 | `33c7da4fbe845ec97a7d5bf9c7ac90cd3205944342dc2c4d6b0cb7d795f2b2ba` |

Saída completa de `size -A kernel.elf`, em bytes:

```text
kernel.elf  :
section           size                   addr
.boot_text         296               33554432
.boot_data       45056               33558528
.text           514614   18446744071595671552
.trampoline        376   18446744071596187648
.rodata          76683   18446744071596191744
.data             1052   18446744071596268448
.bss          15142628   18446744071596269568
.extra           65192   18446744071611412200
Total         15845897
```

A soma de seções é 15.845.897 bytes; o ELF tem 15.977.048 bytes.
`.bss` é NOBITS e representa 15.142.628 bytes de memória, sem payload de
arquivo próprio. Offsets, alinhamentos, lacunas e tabelas do ELF impedem
tratar a soma como tamanho do arquivo. Nenhuma seção foi omitida.
Capturas completas: [stat](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/elf-stat.log),
[size -A](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/elf-size.log),
[objdump -h](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/elf-objdump.log),
[readelf -SW](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/elf-readelf.log).

Os cinco payloads foram extraídos por `mcopy` e comparados com `cmp`
antes do primeiro boot:

| Payload FAT | Bytes | SHA-256 extraído e correspondente no host | Comparação |
|---|---:|---|---|
| `/kernel.elf` | 15977048 | `a6c51ad711d3baf1702159de7a872daa752537142d7e8ae7de704b26e4cbe3a5` | MATCH |
| `/EFI/BOOT/BOOTX64.EFI` | 70785 | `3f0665e6a85825b4821d36f37c2f3d9f7e1f63c37b6fac726158ee66d7133b66` | MATCH |
| `/EFI/fonts/zap-light16.psf` | 5312 | `5296332bcf01bc21a2177cc17c01944ba688420f188ca61d0d42acffda2c942b` | MATCH |
| `/EFI/images/logo.bmp` | 1080054 | `a89522f57573bc4fb3e7ff9ab25fa2a8e2f7c3b2ec157350389133df5480707a` | MATCH |
| `/startup.nsh` | 279 | `3b6565db1dc01469b24c077a77f3c28a5ce596556d0585bf017069252cff5e35` | MATCH |

[Manifesto de referência](../../artifacts/build/foundation-baseline/20260904-220652-0300/reference.json). O kernel extraído
corresponde ao ELF declarado; essa prova não depende só de `launch.env`.

O primeiro boot usou a imagem da raiz. Durante os boots apareceu `NvVars`
no FAT, alterando o hash bruto das imagens de trabalho. Após cada VM os cinco
payloads originais foram novamente extraídos e permaneceram idênticos.
As tentativas seguintes começaram com cópias exatas da imagem congelada,
selecionadas por `HOBBYOS_IMAGE`. Não houve rebuild entre boots.
`launch.env`, inventários FAT e hashes posteriores estão em cada amostra.

A imagem posterior ao boot não é o candidato físico de referência.
O candidato concreto é `E/reference/hobbyos.img`, SHA-256 completo na tabela
acima. As cópias congeladas não mudaram e nenhum release histórico foi substituído.

## Medições de boot

Janela medida somente por subtração de `monotonic_ms`:
`CPU_RELEASE → CORE_COMPLETE`. Não mede power-on/UEFI, boot total,
splash ou tempo até a shell. `clockevent_ticks` permanece no bruto e não
participa do cálculo.

| Ambiente | Tentativa | CPU_RELEASE (ms) | CORE_COMPLETE (ms) | Delta (ms) | Validade | Evidência |
|---|---|---:|---:|---:|---|---|
| Q35/KVM, 4 vCPUs | run-01 | 222 | 403 | 181 | VALID | [serial](../../artifacts/build/foundation-baseline/20260904-220652-0300/qemu/run-01/qemu-serial.log) |
| Q35/KVM, 4 vCPUs | run-02 | 228 | 414 | 186 | VALID | [serial](../../artifacts/build/foundation-baseline/20260904-220652-0300/qemu/run-02/qemu-serial.log) |
| Q35/KVM, 4 vCPUs | run-03 | 273 | 473 | 200 | VALID | [serial](../../artifacts/build/foundation-baseline/20260904-220652-0300/qemu/run-03/qemu-serial.log) |
| Físico, i9-12900K reportado, 24 slots | run-01 recebido | 3412 | 6679 | 3267 | TIMESTAMP_VALID; vínculo ao candidato pendente | [serial transcrito](../../artifacts/build/foundation-baseline/20260905-003339-0300/baremetal/run-01/serial-transcript.txt) |

Em cada serial QEMU os dois markers são únicos, completos, numéricos e ordenados,
nas linhas 257 e 341, offsets 13825 e 18112 bytes. Não houve regressão dos
timestamps de progresso. Valores originais e validação:
[boot-summary.json](../../artifacts/build/foundation-baseline/20260904-220652-0300/boot-summary.json) e
`E/qemu/run-*/boot-markers.txt`.

Durações ordenadas: 181, 186, 200 ms. Mediana QEMU: **186 ms**, com três
amostras válidas do mesmo perfil. Foram iniciadas exatamente três VMs.
Nenhuma tentativa foi descartada, substituída ou repetida; nenhuma duração é zero.

| Tentativa | SHA-256 do serial bruto |
|---|---|
| run-01 | `d55effd7d6127bb300be536b9ab2e51a072d47bf55f4df87a5cdf80e5fd95316` |
| run-02 | `51e1275ea9c9725d6f548489c37aa0f90230b812c4a9f26f986d38c8c2f5697b` |
| run-03 | `b1330f2fe61ea7c8eb266b98db397e16ec3117bfcb9c3eb0dfb733a8a52c14f5` |

| Tentativa | Início da sessão no host | Fim da captura/encerramento no host |
|---|---|---|
| run-01 | 2026-09-04T22:13:52.069624-03:00 | 2026-09-04T22:18:44.117331-03:00 |
| run-02 | 2026-09-04T22:19:24.562162-03:00 | 2026-09-04T22:19:32.163025-03:00 |
| run-03 | 2026-09-04T22:19:53.011968-03:00 | 2026-09-04T22:20:00.174053-03:00 |

Os horários de host são auxiliares, fora da mediana. A primeira sessão durou
mais por ajustes na ferramenta depois da janela medida. Prontidão teve
limite de 180 s e polling de 200 ms; a observação da conclusão de cada comando
teve até 45 s. Foram verificados progresso de core, shell, runtime e sinais
de fault, além do conteúdo novo e prompt no console nas capturas disponíveis.

As três VMs publicaram `SHELL_READY` na linha 391 e
`RUNTIME_READY cpus=4/4` na linha 402. Não apareceram os markers de
panic/fault pesquisados nos seriais. Cada trace, porém, contém oito
rejeições de acesso nas regiões `0xFED40000`, `0xFED40030`,
`0xFED40014` e `pc.bios` (escrita em `0x10`).
A causa dessas mensagens não foi auditada. Os traces têm 592 bytes e SHA-256
`d06a211f1c9b52e4bddc40c82046f6e9eeb01a26fea93f3df94f1f8300091101`.
Os debugcons foram capturados e estão vazios (0 bytes).
Ausência de um marker de fault não foi usada como certificação de runtime.

Na amostra física recebida, `CPU_RELEASE` está na linha 570, offset 25152,
com `monotonic_ms=3412 clockevent_ticks=5`; `CORE_COMPLETE` está na linha
935, offset 42993, com `monotonic_ms=6679 clockevent_ticks=2018`.
Linhas começam em 1, offsets em bytes começam em 0; nesses dois registros
o offset da linha coincide com o início do marker. Ambos são únicos,
completos, numéricos e ordenados. Os registros de progresso extraíveis não
apresentam regressão de `monotonic_ms`.

O serial físico tem 45818 bytes, 990 linhas por `splitlines()`, UTF-8/LF,
SHA-256 `6bfa2fb96d998ac4cf7d1874e3781f024481a67eed2780914a9d7bafd0c3dceb`.
Há um início UEFI e uma entrada de kernel. As duas ocorrências de `PCI_BEGIN`
e de `PCI_SCAN_COMPLETE` pertencem ao mesmo boot e não criam amostras.
O [sumário físico](../../artifacts/build/foundation-baseline/20260905-003339-0300/baremetal/run-01/boot-summary.json)
preserva valores, linhas completas e offsets de linha/marker, inclusive
progresso precedido por fragmentos intercalados.

`6679 - 3412 = 3267 ms` é uma única observação física, não uma mediana.
`BAREMETAL_BOOT_MEDIAN_MS: NOT_MEASURED`; faltam duas amostras.
`run-01` identifica a primeira amostra física recebida neste conjunto;
a quantidade de tentativas não enviadas é `UNKNOWN`. O horário físico do
boot e o limite de observação adotado não foram informados. O timestamp do
diretório identifica a integração, não o boot. O alvo físico de 24 slots
e o perfil QEMU de quatro vCPUs são populações distintas; esses valores
não demonstram regressão ou ganho de desempenho entre ambientes.

## Saídas da shell

A coleta local refutou a alegação de espelhamento do console normal em serial:
`console_write`, em `kernel/src/graphics/console.c:440`, escreve no
console; `console_write_debug` envia serial. `mem`, `cpu` e o prompt
normal não produziram bytes adicionais no serial.

O observador inicial exigia o prompt no serial. Foi encerrado com exit 143;
a mesma VM foi mantida para recuperação. Uma recuperação recusou o hash FAT
alterado por `NvVars`; a seguinte tentou HMP com caminho relativo que o
QEMU não abriu. O transporte retornou 0 apesar do erro textual; o coletor
detectou a ausência do dump, retornou 1 e encerrou a VM. `mem` e `cpu`
não foram executados em `run-01`. O intervalo de boot já estava completo
e é válido. Evidências: [eventos do coletor](../../artifacts/build/foundation-baseline/20260904-220652-0300/collector-events.json),
[correção do observador](../../artifacts/build/foundation-baseline/20260904-220652-0300/observer-correction.json),
[erro HMP original](../../artifacts/build/foundation-baseline/20260904-220652-0300/commands/run-01-ready-state.log).
Os horários exatos de uma recuperação não foram capturados; essa lacuna
está declarada no registro, sem timestamps estimados.

Nas duas tentativas seguintes, `make qemu-agent-monitor` usou caminhos
absolutos para `memsave` e `screendump -f png`. O texto veio dos bytes
reais de `ConsoleCell` da VM: símbolos do ELF medido, célula de 12 bytes,
caractere no offset 0, stride de 512 células e linhas definidas pelo cursor.
Somente espaços/células NUL não usados no fim das linhas foram retirados
do texto derivado. Dumps originais e PNGs estão preservados. O conteúdo
não foi reconstruído a partir dos handlers ou do hardware conhecido.
A imagem de `run-02` também foi inspecionada visualmente.

| Amostra | mem | cpu | Exit code dos envios no host | Exit code do handler |
|---|---|---|---|---|
| run-01 | NOT_EXECUTED | NOT_EXECUTED | Não houve envio | NOT_MEASURED |
| run-02 | Saída completa observada | Saída completa observada | 0 / 0 | NOT_EXPOSED_BY_INTERACTIVE_SHELL |
| run-03 | Saída completa observada | Saída completa observada | 0 / 0 | NOT_EXPOSED_BY_INTERACTIVE_SHELL |
| Físico, run-01 recebido | Transcrição integral revisada | Transcrição integral revisada | NOT_RECORDED | NOT_EXPOSED_BY_INTERACTIVE_SHELL |

No QEMU, os envios foram separados: `make qemu-agent-type TEXT=mem ENTER=1`,
captura completa e retorno ao prompt, depois
`make qemu-agent-type TEXT=cpu ENTER=1`.
A shell interativa descarta o retorno do handler; nenhum status estruturado
de kernel foi inferido do retorno HMP. Conteúdo e conclusão foram observados.

Saída integral representativa de `mem`, QEMU `run-02`
([texto](../../artifacts/build/foundation-baseline/20260904-220652-0300/qemu/run-02/mem-output.txt),
[console capturado](../../artifacts/build/foundation-baseline/20260904-220652-0300/qemu/run-02/mem-1.png)):

```text
Memory Statistics

[PMM]
  Total: 4278190080 bytes (4080.00 MiB) | 1044480 pages
  Free : 2041221120 bytes (1946.66 MiB) | 498345 pages
  Used : 2236968960 bytes (2133.33 MiB) | 546135 pages

[HEAP]
  Total: 33554432 bytes (32.00 MiB)
  Used : 536400 bytes (0.51 MiB)
  Free : 33018032 bytes (31.48 MiB)
  Blocks total: 60 | free: 2
  Largest free block: 33016016 bytes (31.48 MiB)
```

Saída integral representativa de `cpu`, QEMU `run-02`
([texto](../../artifacts/build/foundation-baseline/20260904-220652-0300/qemu/run-02/cpu-output.txt),
[console capturado](../../artifacts/build/foundation-baseline/20260904-220652-0300/qemu/run-02/cpu-1.png)):

```text
CPU / Platform
Vendor: GenuineIntel
Brand : Intel(R) Core(TM) 7 150U
CPUID max basic: 0x0x0000000000000020 | max ext: 0x0x0000000080000008
Family: 6 | Model: 186 | Stepping: 3
Features: LM(x86_64) SSE SSE2 SSE3 SSSE3 SSE4.1 SSE4.2 AES FMA AVX AVX2 BMI1 BMI2 1GPages HTT
```

O total PMM de 4080 MiB e o prefixo `0x0x` são saídas reais, preservadas.
O total PMM não prova RAM configurada: a configuração é `MEM=2G`.
A semântica desse diagnóstico não foi corrigida nem auditada.

### Transcrição física de mem e cpu

Fonte: [shell-transcription.txt](../../artifacts/build/foundation-baseline/20260905-003339-0300/baremetal/run-01/shell-transcription.txt),
707 bytes, SHA-256
`218d5444fc8477f2b69730528e47df1637240931912a63336b4e81f0f4ec5099`.
É o texto integral derivado da foto na revisão anterior, com espaçamento
normalizado nessa revisão. Números, unidades, features e `0x0x` permanecem
como recebidos. A revisão da foto registrou saída completa e retorno ao
prompt; o trecho de ajuda cortado no topo não pertence aos dois comandos.
Não houve novo OCR nem inspeção local de pixels. A fotografia binária não
foi recebida nesta integração textual, não é exigida e não integra o pacote.

```text
HobbyOS> mem
Memory Statistics

[PMM]
  Total: 19327352832 bytes (18432.00 MiB) | 4718592 pages
  Free : 16770453504 bytes (15993.55 MiB) | 4094349 pages
  Used : 2556899328 bytes (2438.44 MiB) | 624243 pages

[HEAP]
  Total: 33554432 bytes (32.00 MiB)
  Used : 3886528 bytes (3.70 MiB)
  Free : 29667904 bytes (28.29 MiB)
  Blocks total: 331 | free: 4
  Largest free block: 29656048 bytes (28.28 MiB)

HobbyOS> cpu
CPU / Platform
Vendor: GenuineIntel
Brand : 12th Gen Intel(R) Core(TM) i9-12900K
CPUID max basic: 0x0x0000000000000020 | max ext: 0x0x0000000080000008
Family: 6 | Model: 151 | Stepping: 2
Features: LM(x86_64) SSE SSE2 SSE3 SSSE3 SSE4.1 SSE4.2 AES FMA AVX AVX2 BMI1 BMI2 1GPages HTT
HobbyOS>
```

As verificações aritméticas do texto recebido conferem:

```text
16770453504 + 2556899328 = 19327352832
4094349 + 624243 = 4718592
4718592 * 4096 = 19327352832
4094349 * 4096 = 16770453504
624243 * 4096 = 2556899328
3886528 + 29667904 = 33554432
```

Isso confere somente consistência aritmética da transcrição, não correção
dos alocadores, ausência de corrupção ou disponibilidade integral da RAM.
`18432.00 MiB` é o total exibido pelo PMM, não uma medição da RAM instalada.
Os MiB exibidos não foram recalculados para substituir a representação da tela.
Não há exit code do handler disponível; prompt retornado não fornece esse campo.
As saídas físicas não estão no serial: sua fonte é a transcrição da foto.
No QEMU, `run-02` e `run-03` já demonstram a saída dos comandos naquele
ambiente; a ausência histórica em `run-01` não exige outra VM.

## Captura física recebida

### Origem e perfil observado

O material foi enviado pelo mantenedor em resposta ao pedido de coleta.
O serial e a foto foram enviados juntos à revisão anterior; esse vínculo é
operacional, não um identificador autenticado de sessão compartilhado pelos
arquivos. O transporte desta integração foi `INLINE_MARKDOWN`, acessado
como `SESSION_CONTENT`: serial completo e texto derivado da imagem.
Os dois textos materializados correspondem aos tamanhos e SHA-256 declarados.
O hash do serial prova igualdade dos bytes transportados, não autentica UART,
horário físico, mídia ou ELF em execução. Não foi preservado um arquivo integral
da instrução de sessão e nenhum hash desse documento foi atribuído.

Proveniência: [capture-provenance.json](../../artifacts/build/foundation-baseline/20260905-003339-0300/baremetal/run-01/capture-provenance.json).
O serial é `USER_SUPPLIED_TERMINAL_TRANSCRIPT`, originalmente identificado
como `Pasted text(20260905-030136).txt`. Seu cabeçalho informa picocom v3.1,
`/dev/ttyUSB0`, 115200 baud, oito bits, sem paridade, um stop bit e sem
controle de fluxo; informa também `logfile is : none`.
Não é apresentado como arquivo produzido por logging do picocom.
O comando de abertura do terminal contido no texto é histórico, não foi executado.

| Campo | Observação / limite |
|---|---|
| CPU reportada pelo alvo | `12th Gen Intel(R) Core(TM) i9-12900K`; família 6, modelo 151, stepping 2 |
| CPUs no resumo recebido | Linha 640: `expected=24 ready=24 failed=0 waiting=0 deadline_us=10000000 abort=0` |
| Perfil reportado | Linha 985: `PASS autorun=0 selftests=0`; compatível com produção, sem autenticar o binário |
| Clockevent reportado | BSP LAPIC, período 1000 us; HPET como clocksource, Timer0 quiescente |
| Placa, BIOS e RAM instalada | `NOT_PROVIDED` |
| Operador nominal e alterações de configuração | `NOT_PROVIDED` |
| Host que abriu o terminal | O prompt contém `Inspiron-14-5440`; não identifica a máquina-alvo |
| Fotografia de origem | `20260905_000404.jpg`, somente referência de proveniência; pixels não fornecidos localmente |
| Conferência da fotografia nesta integração | `NOT_PERFORMED_TEXT_ONLY`; `PHOTO_REQUIRED_FOR_THIS_INTEGRATION: NO` |

### Prontidão e observações do serial

Cada grupo abaixo contém 24 registros com slots únicos de 0 a 23:

| Registro | Ocorrências | Slots únicos |
|---|---:|---|
| `[SMP][CPU] ONLINE` | 24 | 0–23 |
| `[IRQ][CPU_TIMER_VERIFIED] PASS` | 24 | 0–23 |
| `[SCHED][BOOTSTRAP_HANDOFF] PASS` | 24 | 0–23 |
| `[IRQ][CPU_READY] PASS` | 24 | 0–23 |

São observações deste boot, não execução dos `all`, teste de cadência,
soak ou certificação sob todas as cargas. Os 1000 us são período reportado,
não nova medição de precisão. O serial alcança `SHELL_READY PASS`
nas linhas 972–973 (`monotonic_ms=10563`), `RUNTIME_READY PASS cpus=24/24`
na linha 983 e o progresso de runtime na linha 986 (`10658 ms`).
O marker de main loop está na linha 987 (`10671 ms`), seguido por
`Entering Main Loop` na linha 988. Esses marcos não substituem a janela medida.

| Observação | Evidência no serial recebido | Limitação |
|---|---|---|
| Warning PMM | Linha 308: `[PMM] Warning: Frame already used: 0x0000000000010000` | Uma ocorrência; causa não diagnosticada |
| Falhas de comando XHCI | Cinco registros `[XHCI] CMD FAILED. CC=4`, linhas 758, 894, 912, 933 e 954 | Código preservado, sem inferência de causa-raiz |
| Falhas de endereço subsequentes | Cinco `-> Addr Fail.`, linhas 759, 895, 913, 934 e 955 | Sequência original preservada |
| Intercalação serial | Fragmentos de USB/hubs/PCI na mesma linha | IDs/dispositivos não reconstruídos nem reordenados |
| Prefixo CPUID duplicado | `0x0x...` na transcrição física da shell | Representação preservada, sem correção |

O boot alcança shell/runtime apesar desses registros; não é descrito como
boot sem erros. A janela temporal íntegra permanece disponível separadamente
da saúde dos subsistemas. Não foram diagnosticados XHCI, USB, PMM ou formatação.
A busca literal por `PANIC`, `panic`, `FAULT`, `EXCEPTION` e `REGRESSION`
encontrou zero ocorrências neste texto; isso não é teste de ausência de fault.

### Associação ao candidato

O [recibo anterior de preparação da mídia](../../artifacts/build/foundation-baseline/20260904-234853-0300/media/delivery-receipt.json)
e a [conferência após remontagem](../../artifacts/build/foundation-baseline/20260904-234853-0300/media/readback-payloads.json)
registram o SanDisk Cruzer Blade, serial `03023316071824131756`,
partição FAT32 `HOBBYOS`, UUID `3562-1E25`. Foi substituído somente o
`kernel.elf`; os cinco payloads conferiram com a referência congelada.
O kernel na mídia teve SHA-256
`a6c51ad711d3baf1702159de7a872daa752537142d7e8ae7de704b26e4cbe3a5`.
Os registros de desmontagem e desligamento da mídia tiveram exit 0.

Esse recibo prova a preparação da mídia naquele momento, não que o boot
recebido tenha usado aquela mídia sem alteração. Nem o serial nem o texto
da foto fornecem SHA-256 do ELF ou identificador de implantação.
`CANDIDATE_BINDING_STATUS: OPERATOR_CONFIRMATION_PENDING`.
Uma confirmação explícita do operador poderá estabelecer o vínculo
operacional, sem ser chamada de atestação criptográfica. A observação temporal
fica preservada enquanto essa associação não é confirmada.

### Coleta restante

Faltam **dois seriais de boots físicos independentes**, associados ao mesmo
alvo, candidato e perfil. A transcrição revisada já atende ao registro
documental completo de `mem` e `cpu` no ambiente físico apresentado;
não é necessário reenviar ou refazer a foto para esta integração.

1. Confirmar qual mídia/candidato gerou a amostra recebida; identificar
   operador, configuração real e associação das duas amostras restantes.
   A referência continua sendo `E/reference/hobbyos.img` e seus cinco
   payloads de `E/reference.json`, sem recompilar ou usar imagens pós-QEMU.
2. Salvar serial integral desde antes de cada boot, por método realmente
   disponível, em 115200 baud, 8N1, sem controle de fluxo. Usar arquivos
   separados, registrar data/fuso, limite de observação, interrupções e hashes.
3. Manter CPUs, E-cores, Hyper-Threading, BIOS e período LAPIC sem alterações
   diagnósticas. Preservar tentativas lentas, panic, fault, CPU não pronta ou
   parada terminal; não substituir por uma seleção favorável ou configuração reduzida.
4. Calcular a mediana de referência somente após três janelas válidas
   associadas ao mesmo candidato/perfil, sem misturar físico com QEMU.

Esta integração não executou builds, gates, QEMU, novos boots físicos,
acesso a dispositivos, escrita de mídia ou mudanças de BIOS. Os registros
anteriores de preparação da mídia e de runtime são evidências históricas.

## Limitações e continuidade

Os fontes, bootloader, assets, build, testes, linker, contratos e documentação
histórica mantiveram identidade com a entrada, inclusive `graphics.c/.h`,
`task_format.c/.h`, scheduler/switch, input/modal, LAPIC/HPET e topologia.
TASKMAN V2 conserva o registro existente `AUTHORIZED_NOT_STARTED`.

Na coleta QEMU original, os diretórios históricos em `/tmp` foram inventariados e preservados antes
dos harnesses; seus 17 arquivos mantiveram os hashes. Não havia VM ativa,
PID ou socket anterior. As três VMs próprias foram paradas pelo alvo existente;
processos, PID files e sockets estão ausentes. O lock dos harnesses foi
respeitado. [Ownership final](../../artifacts/build/foundation-baseline/20260904-220652-0300/ownership-after.json).

A coleta permanece parcial: há uma janela física observada, duas ainda
ausentes e associação ao candidato pendente de confirmação. A cobertura de
shell no QEMU é sustentada pelas duas últimas VMs; a física, pelo texto
derivado da foto já revisada. Persistem incompatibilidade dos dois `all`,
warnings, trace QEMU e os achados PMM/XHCI da captura física, sem causa
auditada. Nenhum número demonstra melhora de performance sem comparação
posterior. A portabilidade dos gates exige decisão separada; esta integração
não concede dispensa de regressão para alterações futuras.

Os [manifestos da integração](../../artifacts/build/foundation-baseline/20260905-003339-0300/baremetal/run-01/sources-precommit.json)
permitem conferir que apenas este relatório mudou, preservando conteúdo
e modos dos demais 387 arquivos. Builds/gates são
`NOT_RERUN_DOCUMENTATION_ONLY`; novos boots QEMU: 0.
A próxima ação é revisão documental e recebimento das duas amostras físicas
restantes. Commit documental e ZIP registram evidência, não aceite completo
ou autorização para implementar mudanças de kernel.

### Nota de continuidade da validação

A [política corrente](validation-policy.md) concentra a validação física no
fechamento e usa QEMU durante o desenvolvimento. Esta mudança de momento não
altera a coleta acima: permanece uma amostra física de 3267 ms entre três
necessárias, sem mediana e com associação ao candidato ainda não confirmada.
A referência inicial deve continuar congelada para completar e comparar o
baseline no fechamento, sem misturar suas amostras com as do kernel final.
