# TMV1-CL-11-FIX12 — triagem LAPIC KVM focada

## Status

`BLOCKED_BY_EXTERNAL_ENVIRONMENT`

Esta missão foi somente uma triagem focada do gate KVM `accounttest lapic-rate 2000 5`. Ela não certifica a CL-11, não autoriza mudança de kernel e não inicia a CL-12.

## 1. Base e worktree

- Branch: `feat/taskman`.
- HEAD/base: `88645901d9393f4796af84cecd9e13d3d6671c70` (`feat(modal): add owned sessions and independent modal UI workers`).
- A entrada encontrou as mudanças legítimas FIX1–FIX11 ainda não commitadas.
- `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` permaneceu não rastreado e não foi staged.
- Nenhum reset, clean, checkout, restore, stash, commit, push, rebase, merge ou tag foi executado.

## 2. Freeze de fontes

| Arquivo | SHA-256 before | SHA-256 after | Resultado |
|---|---|---|---|
| `kernel/src/apic/lapic.c` | `c21e884b4d9b5a1fdfd3577823a3c79da1f0bde44315f540c8c06f931795b906` | igual | preservado |
| `kernel/src/apic/lapic.h` | `ec1f546164127a272d088a500e8e52343d4f4992b9c0aba1daffb0a357a4f8b7` | igual | preservado |
| `kernel/src/shell/commands/cmd_accounttest.c` | `7b50bdfa861a099e2cad8a3ddcfbd4dc7dffff60e568a505aac09eb289473469` | igual | preservado |
| `scripts/test-lapic-rate.sh` | `caab25e89b3dc04e5ea32092663265483a780dd727c2d80a31ae08dd9c9458a9` | igual | preservado |
| `kernel/src/core/clock.c` | `d5b98fe3a496c11cbb2e622ce3e6997a17ad9cd9b800882d6f1c34a9de511c9c` | igual | preservado |
| `kernel/src/timer/hpet.c` | `d9c53652ec218cbe0c8ceca47af7cfa00158b3d5af13e970eb8f48ca538c5945` | igual | preservado |

`artifacts/build/cl11fix12-frozen-before.sha256` e `artifacts/build/cl11fix12-frozen-after.sha256` são byte a byte iguais. `cmd_accounttest.c` já estava modificado na entrada como parte do worktree FIX1–FIX11; o freeze prova que a missão FIX12 não o alterou. Nenhum arquivo do kernel foi alterado nesta missão.

## 3. Build smoke único

| Gate | Resultado |
|---|---|
| `make stack-check` | PASS; limite 2048 bytes, maior frame 1984 bytes, 0 violações |
| `make kernel-check JOBS=2` | PASS; `kernel.elf` SHA-256 `c95a69a8a29f7a92b131d7de90a6fa93e78081c792aac459b7aff4ef798a7a09` |
| `make image` | PASS |
| `nm -u kernel.elf` | PASS; saída vazia |

Não houve build jN nem verificação de reprodutibilidade, reservados para a certificação final.

## 4. Falha anterior

O ponto de partida foi a falha KVM SMP=8 com `worst_median_x1000=690`, abaixo do limite imutável `700` por 10/1000. O threshold não foi reduzido, o gate não foi substituído pela média global, por SMP=4 ou pela janela longa, e nenhuma run foi repetida até passar.

## 5. Matriz fixa KVM SMP=8

Ambiente comum: QEMU 8.2.2, KVM, SMP=8, MEM=2G. Foram executados exatamente cinco boots independentes. Cada boot executou `lapic-config`, `lapic-liveness 500`, `lapic-rate 2000 5`, `lapic-rate 5000 5` e `accounttest check`.

| Run | Config | Liveness | 2000×5 worst/min/max | Gate original | 5000×5 worst/min/max | Long | Check | Fault |
|---:|---|---|---|---|---|---|---|---|
| 1 | PASS | PASS | 616 / 581 / 819 | FAIL | 691 / 624 / 791 | FAIL | PASS | não |
| 2 | PASS | PASS | 681 / 650 / 801 | FAIL | 694 / 629 / 769 | FAIL | PASS | não |
| 3 | PASS | PASS | 712 / 620 / 866 | PASS | 691 / 642 / 773 | FAIL | PASS | não |
| 4 | PASS | PASS | 669 / 635 / 823 | FAIL | 660 / 627 / 749 | FAIL | PASS | não |
| 5 | PASS | PASS | 695 / 541 / 828 | FAIL | 656 / 647 / 763 | FAIL | PASS | não |

Totais: config 5/5, liveness 5/5, gate original 1/5, janela longa 0/5, `accounttest check` 5/5 e zero guest fault. Toda liveness informou `all_advanced=1` e `zero_irq_cpus=0`; os gaps máximos foram aproximadamente 30,05 ms, 30,25 ms, 40,75 ms, 29,74 ms e 30,12 ms.

A sentinela `[LAPIC_KVM_FOCUSED][COLLECTION] PASS runs=5` confirma conclusão da coleta, não aprovação do LAPIC.

## 6. Medianas por CPU

| Run | Janela | CPU0 | CPU1 | CPU2 | CPU3 | CPU4 | CPU5 | CPU6 | CPU7 | Pior |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2000×5 | 616 | 688 | 711 | 717 | 710 | 751 | 703 | 804 | 616 |
| 1 | 5000×5 | 691 | 719 | 693 | 711 | 717 | 727 | 714 | 720 | 691 |
| 2 | 2000×5 | 681 | 731 | 747 | 693 | 756 | 719 | 722 | 727 | 681 |
| 2 | 5000×5 | 728 | 694 | 710 | 726 | 697 | 698 | 726 | 710 | 694 |
| 3 | 2000×5 | 734 | 712 | 799 | 761 | 713 | 785 | 752 | 782 | 712 |
| 3 | 5000×5 | 691 | 711 | 742 | 707 | 722 | 703 | 714 | 723 | 691 |
| 4 | 2000×5 | 715 | 695 | 715 | 669 | 706 | 725 | 727 | 747 | 669 |
| 4 | 5000×5 | 664 | 676 | 690 | 677 | 679 | 660 | 684 | 710 | 660 |
| 5 | 2000×5 | 736 | 722 | 695 | 749 | 751 | 768 | 734 | 701 | 695 |
| 5 | 5000×5 | 656 | 696 | 710 | 697 | 695 | 720 | 711 | 727 | 656 |

As linhas por round e por CPU permanecem integralmente nos logs de cada run.

## 7. Evidência do host

O processo tinha 12 CPUs disponíveis (`0-11`), `/dev/kvm` existente e read/write, `cpuset.cpus.effective=0-11`; `cpu.max` não estava disponível neste layout de cgroup. A PSI CPU `full` ficou em 0, enquanto a PSI `some` e o load subiram ao longo da matriz.

| Run | load1/load5/load15 antes | PSI some avg10 | PSI full avg10 | QEMU CPU observado depois |
|---:|---|---:|---:|---:|
| 1 | 2,14 / 2,08 / 2,52 | 0,00 | 0,00 | 595% |
| 2 | 6,56 / 3,55 / 3,01 | 0,13 | 0,00 | 576% |
| 3 | 8,46 / 4,88 / 3,52 | 0,20 | 0,00 | 554% |
| 4 | 8,31 / 5,65 / 3,90 | 0,50 | 0,00 | 601% |
| 5 | 8,08 / 6,17 / 4,23 | 0,97 | 0,00 | 608% |

Havia carga concorrente de desktop e ferramenta: `chrome`, `codex`, `Xorg`, `gnome-shell`, `gnome-terminal` e workers do kernel. Foi observado um processo Chrome em 58,7% antes da run 4 e 66,1% depois da run 3. Os snapshots completos de CPU, memória e processos estão nos arquivos `*-host-before.env` e `*-host-after.env`.

## 8. Janela longa

A janela `5000×5` foi mantida apenas como diagnóstico. Ela falhou nos cinco boots SMP=8 padrão, com piores medianas `691, 694, 691, 660, 656`. Isso não substitui nem relaxa o gate `2000×5` e não foi usado como aceite.

## 9. Controles condicionais

### KVM SMP=4

| Run | Config | Liveness | 2000×5 worst | 5000×5 worst | Resultado |
|---:|---|---|---:|---:|---|
| 1 | PASS | PASS | 994 | 993 | ambos PASS |
| 2 | PASS | PASS | 995 | 995 | ambos PASS |
| 3 | PASS | PASS | 994 | 993 | ambos PASS |

O controle SMP=4 passou de forma consistente em 3/3 boots, inclusive sob load1 antes entre 4,36 e 8,56.

### KVM SMP=8 com afinidade

O host oferecia 12 CPUs, portanto a run diagnóstica foi executada. A afinidade process-wide foi aplicada a todos os threads QEMU nos CPUs `8,9`; a build do QEMU expôs todos os threads de vCPU com o mesmo `comm=qemu-system-x86`, de modo que o mapeamento individual vCPU→TID não pôde ser identificado (`vcpu_threads_pinned=0`). O artefato registra a afinidade de cada TID e comprova que todos ficaram em `8,9`.

Resultado: config PASS, liveness PASS, `2000×5` PASS com pior mediana 806 e `5000×5` PASS com pior mediana 805. A melhora sobre os cinco piores resultados padrão (`616–712` e `656–694`) é clara, mas essa run não conta como aceite do gate padrão.

### TCG

Não executado, conforme a condição da missão: não houve falha de configuração ou liveness em KVM. A matriz TCG anterior já era válida para a mesma fonte.

## 10. Classificação

Resultado: `BLOCKED_BY_EXTERNAL_ENVIRONMENT`.

Evidência determinante:

- os cinco boots KVM SMP=8 tiveram configuração e liveness PASS, `all_advanced=1`, zero CPU sem IRQ e zero guest fault;
- o gate original falhou em 4/5 sem qualquer alteração do kernel;
- o controle KVM SMP=4 passou de forma consistente em 3/3, com piores medianas próximas de 994–995;
- a afinidade process-wide elevou o KVM SMP=8 para 806/805;
- o host mostrou carga concorrente, load1 até 8,46, PSI CPU `some` crescente e entrega efetiva do QEMU em aproximadamente 5,5–6,1 CPUs durante os boots padrão.

Esses dados correlacionam o desvio ao serviço/escalonamento do host e separam o comportamento da configuração/liveness do LAPIC. Eles não suportam `READY_FOR_FINAL_CERTIFICATION`, pois o gate padrão não passou 5/5, nem `REJECTED_KERNEL_TIMER_SERVICE`, pois SMP=4 foi consistentemente saudável e a afinidade melhorou claramente a mesma imagem SMP=8.

Nenhuma correção de kernel foi implementada ou autorizada.

## 11. O que não foi executado

- CL-01 a CL-11 integral;
- `scripts/test-cl11.sh all` ou `scripts/test-cl10.sh all`;
- regressões CL-07/08/09/10;
- negatives gerais ou CL-11;
- heavy runs ou soak CL-11;
- TCG nesta missão;
- build jN/hash final;
- CL-12 ou fases posteriores.

## 12. Próximo passo

Reexecutar esta triagem fixa em uma janela/host apropriado antes da certificação final. Não alterar o kernel com base nesta missão. A certificação integral CL-11 permanece separada e só deve retomar os gates ainda não alcançados após o gate KVM padrão obter 5/5.

## 13. Git

- Commit criado: não.
- Push realizado: não.
- Stage: nenhum arquivo adicionado pela missão.
- Mudanças da missão: `scripts/test-lapic-kvm-focused.sh`, esta documentação e a adição FIX12 em `AGENTS.md`.
- Mudanças e arquivos FIX1–FIX11 pré-existentes foram preservados.

## 14. Artefatos

- Resumo tabular: `artifacts/build/cl11fix12-kvm-summary.tsv`.
- Resumo de coleta: `artifacts/build/cl11fix12-kvm-summary.log`.
- Controles: `artifacts/build/cl11fix12-kvm-controls-summary.tsv`.
- Runs SMP=8: `artifacts/build/cl11fix12-kvm-run1.log` a `cl11fix12-kvm-run5.log`, com `.launch.env` e host before/after correspondentes.
- SMP=4: `artifacts/build/cl11fix12-kvm-smp4-run1.log` a `cl11fix12-kvm-smp4-run3.log`.
- Afinidade: `artifacts/build/cl11fix12-kvm-affinity-diagnostic.log` e `cl11fix12-kvm-affinity-diagnostic-affinity.env`.
- Freeze: `artifacts/build/cl11fix12-frozen-before.sha256` e `cl11fix12-frozen-after.sha256`.
- Build smoke: `artifacts/build/cl11fix12-stack-check.log`, `cl11fix12-kernel-check.log`, `cl11fix12-image.log` e `cl11fix12-nm-u.log`.

Status final: `BLOCKED_BY_EXTERNAL_ENVIRONMENT`.
