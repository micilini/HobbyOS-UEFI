# TMV1-CL-11-FIX13 — execução manual do gate KVM em host limpo

## Status

`READY_FOR_OPERATOR_RUN`

Este runner prepara uma coleta manual e isolada do gate original `accounttest lapic-rate 2000 5`. Ele executa exatamente cinco boots independentes em KVM, SMP=8 e MEM=2G, sem afinidade, `taskset`, prioridade negativa ou realtime. Não executa build, TCG, SMP=4, janela `5000×5`, regressões, negatives ou soak.

## Antes de executar

Prepare `kernel.elf` e `hobbyos.img` antes de limpar o host. O runner recusa uma imagem ausente e não constrói nada automaticamente durante a janela de medição.

Não use `sudo`. Root não melhora o scheduling KVM, muda `HOME` e as configurações de Codex/Git, pode criar artefatos pertencentes a root e pode esconder um problema de associação ao grupo `kvm` sem corrigir a conta normal. Se `/dev/kvm` não for gravável, corrija a associação do usuário normal ao grupo `kvm`, encerre a sessão se necessário e tente novamente como esse usuário.

Encerre completamente o Codex antes de iniciar. O runner procura processos `codex` e `codex-code-mode` do mesmo usuário e recusa a execução enquanto algum estiver ativo. Isso retira da janela de medição tanto o processo do agente quanto a atividade de geração e aprovações interativas.

Feche também estas aplicações pesadas:

- Chrome ou Chromium;
- Firefox;
- Slack;
- VS Code ou Code Insiders;
- Steam;
- Discord.

O runner lista somente PID, `comm` e CPU% dos processos encontrados e não mata nenhum processo. `ALLOW_BUSY_HOST=1` existe apenas para diagnóstico: qualquer coleta feita com esse override será classificada como `NON_CERTIFIABLE_BUSY_RUN`, mesmo que passe 5/5.

O preflight exige `/dev/kvm` existente, legível e gravável, QEMU 8.2.2, `nproc >= 8` e pelo menos oito CPUs em `Cpus_allowed_list`. Ele registra governor, frequência, estado do `intel_pstate`, turbo, thermal zones e `sensors` quando disponíveis, mas não altera o host e não pede privilégios.

## Execução

Com o Codex encerrado, no diretório raiz do projeto e como o usuário normal, execute uma única vez:

```bash
scripts/run-lapic-kvm-clean-host.sh
```

O runner para qualquer QEMU anterior do workspace, exige namespace `.qemu` inativo, aguarda 60 segundos fixos para estabilização e então faz cinco boots. Cada boot executa, uma única vez e nesta ordem:

```text
accounttest lapic-config
accounttest lapic-liveness 500
accounttest lapic-rate 2000 5
accounttest check
```

Não repita uma run falha, não descarte outlier e não substitua artefatos. Para proteger a coleta, o runner recusa iniciar se já existir qualquer arquivo com o prefixo `cl11fix13-clean-kvm`. Não mude threshold, afinidade, prioridade, SMP, memória ou quantidade de boots.

## Resultados possíveis

- `READY_FOR_FINAL_CERTIFICATION`: os quatro gates passaram 5/5, sem guest fault ou falha de transporte, e os hashes congelados permaneceram iguais.
- `HOST_STILL_UNSUITABLE`: config, liveness e check passaram, não houve guest fault, mas ao menos uma run do gate de taxa falhou. Não altere o kernel nem execute a regressão integral.
- `NON_CERTIFIABLE_BUSY_RUN`: `ALLOW_BUSY_HOST=1` foi usado. O resultado não certifica, mesmo com 5/5.
- `KERNEL_BEHAVIOR_CHANGED`: config, liveness, IRQs por CPU, check, guest, transporte ou freeze exigem auditoria técnica. Pare e não avance para a regressão integral.

A sentinela `[LAPIC_CLEAN_HOST][COLLECTION] PASS runs=5` comprova somente que as cinco coletas fixas terminaram. O veredito está na linha `[LAPIC_CLEAN_HOST][RESULT] ...`.

## Artefatos a enviar

Envie o resumo e todos os arquivos com o prefixo FIX13:

- `artifacts/build/cl11fix13-clean-kvm-summary.tsv`;
- `artifacts/build/cl11fix13-clean-kvm-summary.log`;
- `artifacts/build/cl11fix13-clean-kvm-run1.log` até `run5.log`;
- os cinco pares `runN-host-before.env` e `runN-host-after.env`;
- os cinco `runN.launch.env`;
- `cl11fix13-clean-kvm-frozen-before.sha256` e `cl11fix13-clean-kvm-frozen-after.sha256`.

Depois da coleta, crie um ZIP sem remover os originais:

```bash
zip -j cl11fix13-clean-kvm-artifacts.zip artifacts/build/cl11fix13-clean-kvm-*
```

Retorne com o ZIP e com o conteúdo do `cl11fix13-clean-kvm-summary.log`.

## Próximo passo

Se o resultado for `READY_FOR_FINAL_CERTIFICATION`, devolva a evidência para que a certificação CL-11 seja retomada separadamente. Se for `HOST_STILL_UNSUITABLE`, preserve a coleta e procure uma janela ou host ainda mais limpo, sem alterar o kernel. Se for `KERNEL_BEHAVIOR_CHANGED`, pare para auditoria técnica. Não iniciar TMV1-CL-12 nesta etapa.
