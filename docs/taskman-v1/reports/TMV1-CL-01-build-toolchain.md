# TMV1-CL-01 — Build, toolchain e QEMU automatizável

## Identificação

- Fase/data: TMV1-CL-01, 30/07/2026
- Raiz: `/home/william-lima/Área de trabalho/HobbyOS-UEFI`
- Branch: `feat/taskman`
- Commit-base: `a4e52edf63126d395dfd069ec07ddd22699c9bda`
- Worktree inicial: limpo (`## feat/taskman`); nenhum modificado ou não rastreado preexistente
- Escopo: somente baseline mecânico de build/toolchain e harness QEMU; scheduler/TASKMAN/lifecycle/input funcional não alterados

## Problemas confirmados

O build permissivo baseline retornou 0, mas confirmou os cinco bloqueadores: `panic.c` sem `graphics.h`, `heap.c` convertendo implicitamente `void *` em `uint64_t`, `keyboard.c` sem `console.h`, `shell.c` sem `serial.h` e `xhci_configure_device()` ausente de `xhci.h`. Também foram confirmados ausência de preflight/depfiles/canal headless, KVM/`-cpu host` hardcoded e OVMF hardcoded.

## Implementação

- Os quatro headers públicos foram incluídos/publicados e o heap passou a manter `void *`, sem alteração semântica.
- `makefile`: `gnu11`, flags freestanding preservadas, `-fno-pie`, três `-Werror` seletivos, C/ASM separados, `LDFLAGS_KERNEL` efetivo, depfiles `-MMD -MP`, clean determinístico, `image`, `kernel-check`, preflight e targets QEMU.
- `scripts/kernel-check.sh`: Bash/pipefail, clean build paralelo, terminal+log e SHA-256.
- `scripts/check-deps.sh`: escopos kernel/image/qemu/all, inventário `[OK]/[MISSING]/[OPTIONAL]/[INFO]` e overrides.
- `scripts/qemu_hmp.py`: socket Unix com timeout, command/key/text/quit, `a-z`, `0-9`, espaço e Enter.
- `scripts/qemu-agent.sh`, `qemu-run.sh`, `wait-for-log.sh`: TCG/KVM/auto, firmware, PID/socket/logs, stale handling e stop HMP→TERM→KILL validado por instância.
- `AGENTS.md`, `.gitignore` e `docs/development/build-and-qemu.md`: contrato operacional persistente.

## Toolchain

Minimum version tested in TMV1-CL-01:

- GCC 13.3.0; GNU ld/objcopy 2.42; GNU Make 4.3
- QEMU 8.2.2; Python 3.12.3
- GNU-EFI: `/usr/include/efi`, `/usr/lib` (CRT/linker script/libs)
- OVMF combinado: `/usr/share/ovmf/OVMF.fd` (`-bios`)
- QEMU: `/usr/bin/qemu-system-x86_64`; `/dev/kvm` presente e acessível

## Testes

| ID | Comando | Resultado | Log/observação |
|---|---|---|---|
| T01 | `make clean; make -j2 kernel.elf` | PASS permissivo | Retorno 0; cinco warnings bloqueadores confirmados antes do gate |
| T02 | `make clean; make kernel.elf` | PASS | Gate seletivo ativo; warnings legados preservados |
| T03 | `make kernel-check JOBS=2` | PASS | `artifacts/build/kernel-check-j2.log` |
| T04 | `make kernel-check JOBS=12` | PASS | `artifacts/build/kernel-check-j12.log` |
| T05 | comparação SHA-256 | PASS | hashes idênticos |
| T06 | `make kernel.elf EFIINC=/definitely/missing EFILIB=/definitely/missing` | PASS | `/tmp/tmv1-noefi.log` |
| T07 | `make deps-check` (escopos exercitados) | PASS | kernel, imagem e QEMU disponíveis |
| T08 | `make deps-check SCOPE=image EFIINC=/definitely/missing EFILIB=/definitely/missing` | PASS | falhou com retorno não zero e lista explícita |
| T09 | cópia `/tmp`, remoção de include, `make -j2 kernel.elf` | PASS | retorno 2 por `-Werror=implicit-function-declaration`; cópia removida |
| T10 | touch/restauração de `graphics.h`, make incremental | PASS | `panic.o` recompilado; `/tmp/tmv1-dep-incremental.log` |
| T11 | `make clean; make image` | PASS | imagem EFI criada |
| T12 | agent TCG + status + `info status` | PASS | processo vivo, HMP running e três logs criados |
| T13 | texto `ps` + Enter; tecla `esc` | PASS | HMP aceitou sem bloqueio |
| T14 | stop + status | PASS | HMP quit; pid/socket removidos; logs preservados |
| T15 | agent `SMP=4 ACCEL=kvm`; `ACCEL=auto` | PASS | KVM passou; auto escolheu KVM |
| T16 | status/diff/check e stage | PASS | somente arquivos TMV1-CL-01 |

## Artefatos

- `kernel.elf` j2: `765ba2db1ae1dd5c722b7716541b00fec8e74f0b2de0a561408f974fa05619b2`
- `kernel.elf` j12: `765ba2db1ae1dd5c722b7716541b00fec8e74f0b2de0a561408f974fa05619b2`
- Igualdade: PASS
- `hobbyos.img`: `9d62238b3005a90009570b3aff75c323effffb376f7d98f939506bae368a01c7`
- Build logs: `artifacts/build/kernel-check-j2.log`, `artifacts/build/kernel-check-j12.log`
- QEMU logs: `.qemu/qemu-serial.log`, `.qemu/qemu-debugcon.log`, `.qemu/qemu-trace.log`

## Dívidas não bloqueadoras

Permanecem visíveis warnings legados de XHCI (packed members, unused, switch), demais unused, `.note.GNU-stack`, LOAD RWX e `guest_errors` de MMIO. Não foram corrigidos/silenciados por pertencerem a dívida ou fases posteriores.

## Status

APPROVED WITH NON-BLOCKING NOTES
