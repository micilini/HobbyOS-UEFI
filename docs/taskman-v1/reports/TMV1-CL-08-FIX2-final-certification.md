# TMV1-CL-08-FIX2 — certificação final

Correção posterior: consulte `TMV1-CL-08-FIX3-final-certification.md`.

Data: 2026-07-31
Branch: `feat/taskman`
Base: `e9834561d66b1bf55a8aed39d7bcde75c16baf7a`
Status: **REJECTED**

## Estado inicial

O worktree CL-08/FIX foi preservado integralmente. O roadmap preexistente continuou
não rastreado e não foi incluído em stage. Nenhum reset, stash, rebase, amend ou
push foi executado.

## Correções FIX2 aplicadas

- o harness host passou a observar PASS, FAIL, panic, faults e morte do QEMU;
- o negative AGE_ONLY recebeu comando dedicado e blocker ainda ativo;
- as sentinelas negativas do reaper passaram a ser emitidas fora do scheduler lock;
- `free_inflight` passou a fechar `claimed == reaped + free_inflight`;
- CPU slot residual passou a ser estrutural quando a task não está on-CPU/current;
- blocker estrutural passou a registrar `STRUCTURAL_FAULT` e causar panic controlado;
- o check passou a validar `free_inflight == 0` em repouso;
- timer-ref deixou de ler snapshot não inicializado e ganhou cleanup de falha;
- snapshot passou a detectar IDs duplicados entre páginas da mesma geração;
- grace passou a testar os limites lógicos 0/1/3000 sob scheduler lock.

## Bloqueio encontrado

O gate determinístico held-ZOMBIE falhou em SMP=1 TCG. A execução produziu:

```text
[TASK][CREATE] id=7 name=reap-held flags=0x2 class=NORMAL source=kthread
```

e não produziu transição lifecycle nem a sentinela
`[REAPTEST][ZOMBIE_MEM_SETUP]`. O BSP terminou em idle. A mesma imagem executou
`reaptest normal 1` com PASS, isolando a falha no fixture/hold, não no create/exit
normal genérico.

Foram tentados handshake atômico, timer, semáforo e instalação imediata do hold
por ID. A condição permaneceu reproduzível. O QEMU foi parado e os logs foram
preservados em `.qemu/qemu-serial.log`.

## Gates executados

- `make kernel-check JOBS=2`: PASS antes e depois das alterações;
- `make stack-check`: PASS, maior frame 1920 bytes, zero violações;
- `git diff --check`: PASS;
- `reaptest normal 1`, SMP=1 TCG: PASS;
- fixture ZOMBIE, SMP=1 TCG: FAIL por ausência de progresso/sentinela.

## Gates não executados após a falha bloqueante

jN final, hashes reproduzíveis, matriz SMP 1/2/4/8, ZOMBIE MEM~, quatro
negatives, CL-07 final e soak SMP=8 de 180 segundos. Nenhum resultado foi
inferido ou marcado como PASS.

## Git

Nenhum commit foi criado, conforme a regra de não commitar com gate falho.
TMV1-CL-09 e fases posteriores não foram iniciadas.
