# TMV1-CL-11-FIX14 — auto-clean e benchmark KVM destacado

## Objetivo e status

Esta automação agenda um worker destacado que espera todos os processos Codex do
usuário desaparecerem, fecha somente aplicações de uma allowlist exata, estabiliza o
host e executa uma única vez o runner FIX13. Ela não altera o kernel, não constrói a
imagem, não executa regressões adicionais e não inicia a CL-12.

Status de preparação: `READY_TO_PREVIEW_AND_ARM`. Depois da sentinela
`[LAPIC_AUTOCLEAN][ARMED] PASS`, o status passa a
`ARMED_WAITING_FOR_CODEX_EXIT`.

## Preview, risco e aprovação

Execute como o usuário normal, nunca como root:

```bash
scripts/launch-lapic-kvm-auto-clean.sh --preview
```

O preview completo fica em
`artifacts/build/cl11fix14-autoclean-process-preview.tsv`. Antes de aprovar, salve
todo trabalho não salvo. As aplicações allowlisted recebem primeiro `SIGTERM` e,
caso continuem vivas após até 30 segundos, recebem `SIGKILL`. Isso pode perder
abas, mensagens, documentos ou estado que a aplicação ainda não tenha persistido.

Depois de conferir a lista e aprovar uma única vez, arme:

```bash
scripts/launch-lapic-kvm-auto-clean.sh --arm
```

O preview é uma fotografia dos PIDs encontrados. PIDs-filhos podem mudar antes da
saída do Codex; a autorização permanece limitada aos nomes `comm` da allowlist
congelada, e cada identidade é revalidada imediatamente antes de qualquer sinal.

## Aplicações encerradas e allowlist

Somente processos do mesmo UID cujo `comm` coincida exatamente com um destes
nomes podem ser encerrados:

```text
chrome
chromium
chromium-browse
brave
brave-browser
firefox
slack
code
code-insiders
codium
VSCodium
Discord
discord
steam
steamwebhelper
spotify
teams-for-linux
msedge
opera
vivaldi-bin
```

Para cada PID, o worker revalida `/proc/<pid>/status` e `/proc/<pid>/comm` antes
de cada sinal. Outros processos podem aparecer como `OBSERVE_ONLY`, mas nunca são
encerrados automaticamente.

## Processos protegidos

Shells, terminais, sessão gráfica, SSH, systemd, D-Bus, rede e áudio são
protegidos. Isso inclui `bash`, `zsh`, `fish`, `sh`, `sshd`, `ssh`,
`gnome-terminal`, `gnome-terminal-server`, `konsole`, `xterm`, `systemd`,
`dbus-daemon`, `dbus-broker`, `Xorg`, `Xwayland`, `gnome-shell`, `plasmashell`,
`NetworkManager`, `pipewire`, `wireplumber`, `pulseaudio`, `login` e `agetty`.
O worker também nunca envia sinal ao Codex; ele apenas espera até 15 minutos por
zero processos `codex` e `codex-code-mode` do mesmo usuário.

## Execução destacada e `/exit`

O método preferido é uma unit transitória `systemd-run --user` com `Type=exec`,
`CollectMode=inactive-or-failed` e working directory na raiz do projeto. Quando a
sessão systemd do usuário não está funcional, o fallback é `setsid` + `nohup`,
com stdin em `/dev/null`. O worker escreve evidência no workspace e não depende
somente do journal.

Após o armamento, a única ação manual é:

```text
/exit
```

O benchmark não começa enquanto existir um processo Codex do usuário.

## Logs e pacote final

Os principais arquivos são:

- `artifacts/build/cl11fix14-autoclean-worker.log`;
- `artifacts/build/cl11fix14-autoclean-processes-before.tsv`;
- `artifacts/build/cl11fix14-autoclean-processes-after.tsv`;
- `artifacts/build/cl11fix14-autoclean-closed.tsv`;
- `artifacts/build/cl11fix14-autoclean-host.env`;
- `artifacts/build/cl11fix13-operator-console.log`;
- `artifacts/build/cl11fix14-autoclean-result.txt`;
- `artifacts/build/cl11fix14-autoclean-results.tar.gz`.

O pacote inclui a coleta FIX13, os arquivos FIX14, os três scripts, os documentos
FIX13/FIX14 e `AGENTS.md`. Não inclui `.git` nem arquivos externos ao projeto.

## Resultados

Os resultados possíveis são `READY_FOR_FINAL_CERTIFICATION`,
`HOST_STILL_UNSUITABLE`, `NON_CERTIFIABLE_BUSY_RUN`, `KERNEL_BEHAVIOR_CHANGED`,
`CODEX_DID_NOT_EXIT` e `AUTOCLEAN_FAILED`. O runner executa exatamente os cinco
boots FIX13, sem repetição, afinidade, prioridade especial ou threshold relaxado.

## Depois da execução

Consulte `cl11fix14-autoclean-closed.tsv` para ver as aplicações encerradas e
reabra manualmente somente as necessárias pelo menu da sessão ou pelo comando que
você normalmente usa. A automação não reinicia aplicações.

## Git

A FIX14 não cria commit, não faz push e mantém o roadmap fora do stage. Nenhuma
mudança de kernel faz parte desta missão. O estado final esperado desta preparação
é `ARMED_WAITING_FOR_CODEX_EXIT`; a classificação do benchmark será escrita pelo
worker depois da saída do Codex.
