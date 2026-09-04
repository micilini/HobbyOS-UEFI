.PHONY: all clean image production-image production-test-policy kernel-check deps-check deps-check-kernel deps-check-image deps-check-qemu run qemu-agent-start qemu-agent-status qemu-agent-monitor qemu-agent-type qemu-agent-key qemu-agent-logs qemu-agent-stop qemu-agent-wait test-clock-matrix test-taskman-visual test-interrupt-bringup test-timer-clockevent selftest-kernel stack-check

.DEFAULT_GOAL := all

# Makefile HobbyOS - Modular & Fixed

ARCH ?= x86_64

# Pastas
BOOT_DIR = bootloader
KERNEL_DIR = kernel

# Ferramentas
CC = gcc
LD = ld
OBJCOPY = objcopy

# --- CONFIGURAÇÃO CRÍTICA DO GNU-EFI ---
# Ajuste estes caminhos se o seu Linux for diferente (ex: /usr/lib64/gnuefi)
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null)
EFI_LIB_CANDIDATES := /usr/lib /usr/lib64 /usr/lib/$(MULTIARCH) /usr/lib/gnuefi /usr/local/lib
EFILIB ?= $(firstword $(foreach d,$(EFI_LIB_CANDIDATES),$(if $(wildcard $(d)/crt0-efi-$(ARCH).o),$(d))))
EFIINC ?= $(firstword $(wildcard /usr/include/efi /usr/local/include/efi))

# O arquivo mágico (CRT0) e o Script de Linkagem
CRT0 = $(EFILIB)/crt0-efi-$(ARCH).o
LDSCRIPT = $(EFILIB)/elf_$(ARCH)_efi.lds

# Flags Bootloader
EFIINCS = -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol
CFLAGS_EFI = $(EFIINCS) -std=gnu11 -MMD -MP -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -DEFI_FUNCTION_WRAPPER -Wall
# Note que removemos o script do LDFLAGS aqui para colocar na regra de compilação explicitamente
LDFLAGS_EFI = -nostdlib -znocombreloc -shared -Bsymbolic -L $(EFILIB) -L /usr/lib -lgnuefi -lefi

# Flags Kernel
KERNEL_COMMON_FLAGS = -ffreestanding -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -fno-pic -fno-pie
KERNEL_EXTRA_CFLAGS ?=
SELFTEST ?= 0
SELFTEST_AUTORUN ?= 0
ifeq ($(SELFTEST_AUTORUN),1)
ifneq ($(SELFTEST),1)
$(error SELFTEST_AUTORUN=1 requires SELFTEST=1)
endif
endif
SELFTEST_CFLAGS =
ifeq ($(SELFTEST),1)
SELFTEST_CFLAGS += -DHOBBYOS_SELFTEST=1
endif
ifeq ($(SELFTEST_AUTORUN),1)
SELFTEST_CFLAGS += -DHOBBYOS_SELFTEST_AUTORUN=1
endif
CFLAGS_KERNEL = $(KERNEL_COMMON_FLAGS) -std=gnu11 -MMD -MP -Wall -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion $(KERNEL_EXTRA_CFLAGS) $(SELFTEST_CFLAGS)
ASFLAGS_KERNEL = $(KERNEL_COMMON_FLAGS)
LDFLAGS_KERNEL = -T $(KERNEL_DIR)/link.ld -static -Bsymbolic -nostdlib -z max-page-size=0x1000

# Cores COnfiguration for SMP
SMP ?= 4
QEMU_SMP = $(SMP),sockets=1,cores=$(SMP),threads=1

# Note used to able serial porta on KNUP PCI 0x4000
CFLAGS_EFI += -DHOBBYOS_SERIAL_INCLUDE_KNUP_FALLBACK=1
CFLAGS_KERNEL += -DHOBBYOS_KERNEL_SERIAL_DEV_PORTS=1

all: hobbyos.img

# --- Bootloader ---

# Lista de Objetos do Bootloader (Main + Módulos da pasta src)
BOOT_OBJS = $(BOOT_DIR)/main.o \
			$(BOOT_DIR)/src/serial.o \
            $(BOOT_DIR)/src/utils.o \
            $(BOOT_DIR)/src/file.o \
            $(BOOT_DIR)/src/kernel_loader.o \
			$(BOOT_DIR)/src/mem.o \
			$(BOOT_DIR)/src/gop.o \
			$(BOOT_DIR)/src/acpi.o \
			$(BOOT_DIR)/src/font.o \
			$(BOOT_DIR)/src/bmp.o \
			$(BOOT_DIR)/src/starter.o

# 1. Regra para compilar o main.c
$(BOOT_DIR)/main.o: $(BOOT_DIR)/main.c
	$(CC) $(CFLAGS_EFI) -c $< -o $@

# 2. Regra genérica para compilar qualquer arquivo .c dentro de bootloader/src/
$(BOOT_DIR)/src/%.o: $(BOOT_DIR)/src/%.c
	$(CC) $(CFLAGS_EFI) -c $< -o $@

# 3. Linkagem (Junta main.o + utils.o + file.o + bibliotecas EFI)
bootx64.so: $(BOOT_OBJS)
	$(LD) -T $(LDSCRIPT) $(CRT0) $(BOOT_OBJS) $(LDFLAGS_EFI) -o $@

# 4. Criação do binário final EFI
BOOTX64.EFI: bootx64.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym  -j .rel -j .rela -j .reloc --target=efi-app-$(ARCH) $< $@

# --- Kernel ---
KERNEL_SRCS = $(KERNEL_DIR)/src/core/entry.S \
			  $(KERNEL_DIR)/kernel.c \
              $(KERNEL_DIR)/src/core/kernel_init.c \
              $(KERNEL_DIR)/src/core/panic.c \
              $(KERNEL_DIR)/src/core/clock.c \
              $(KERNEL_DIR)/src/core/task_metrics.c \
			  $(KERNEL_DIR)/src/core/task_format.c \
			  $(KERNEL_DIR)/src/core/selftest.c \
			  $(KERNEL_DIR)/src/core/runtime_ready.c \
			  $(KERNEL_DIR)/src/core/task_lifecycle.c \
              $(KERNEL_DIR)/src/graphics/splash.c \
              $(KERNEL_DIR)/src/utils/utils.c \
              $(KERNEL_DIR)/src/utils/bitmap.c \
              $(KERNEL_DIR)/src/memory/gdt.c \
              $(KERNEL_DIR)/src/memory/pmem.c \
              $(KERNEL_DIR)/src/memory/paging.c \
              $(KERNEL_DIR)/src/memory/heap.c \
              $(KERNEL_DIR)/src/graphics/graphics.c \
              $(KERNEL_DIR)/src/acpi/acpi.c \
              $(KERNEL_DIR)/src/acpi/madt.c \
              $(KERNEL_DIR)/src/apic/legacy_pic.c \
              $(KERNEL_DIR)/src/cpu/cpu.c \
              $(KERNEL_DIR)/src/apic/lapic.c \
              $(KERNEL_DIR)/src/apic/ioapic.c \
              $(KERNEL_DIR)/src/timer/hpet.c \
			  $(KERNEL_DIR)/src/graphics/console.c \
			  $(KERNEL_DIR)/src/graphics/terminal.c \
			  $(KERNEL_DIR)/src/core/idt.c \
			  $(KERNEL_DIR)/src/core/interrupts.c \
			  $(KERNEL_DIR)/src/core/interrupt_context.c \
			  $(KERNEL_DIR)/src/core/irq_bootstrap.c \
			  $(KERNEL_DIR)/src/core/interrupt_stubs.S \
			  $(KERNEL_DIR)/src/libc/memory.c \
			  $(KERNEL_DIR)/src/drivers/ps2.c \
              $(KERNEL_DIR)/src/drivers/keyboard.c \
              $(KERNEL_DIR)/src/libc/string.c \
			  $(KERNEL_DIR)/src/shell/shell.c \
			  $(KERNEL_DIR)/src/shell/diagnostic_result.c \
			  $(KERNEL_DIR)/src/shell/commands/registry.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_help.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_clear.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_version.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_mem.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_panic.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_cpu.c \
		      $(KERNEL_DIR)/src/shell/commands/cmd_pci.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_acpi.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_irq.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_echo.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_usbdiag.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_smpstress.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_ps.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_kill.c \
			  $(KERNEL_DIR)/src/shell/commands/taskman_view.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_taskman.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_taskdiag.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_tasktest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_schedtest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_synctest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_accounttest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_killtest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_reaptest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_inputtest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_modaltest.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_taskmantest.c \
			  $(KERNEL_DIR)/src/core/irq_stats.c \
			  $(KERNEL_DIR)/src/power/power.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_power.c \
			  $(KERNEL_DIR)/src/acpi/sleep.c \
			  $(KERNEL_DIR)/src/drivers/timer.c \
			  $(KERNEL_DIR)/src/drivers/pci.c \
			  $(KERNEL_DIR)/src/drivers/pci_descriptors.c \
			  $(KERNEL_DIR)/src/drivers/watchdog/intel_tco.c \
			  $(KERNEL_DIR)/src/cpu/tss.c \
			  $(KERNEL_DIR)/src/drivers/watchdog/acpi_wdat.c \
			  $(KERNEL_DIR)/src/drivers/watchdog/acpi_wddt.c \
			  $(KERNEL_DIR)/src/drivers/watchdog/acpi_wdrt.c \
			  $(KERNEL_DIR)/src/drivers/usb/xhci/xhci.c \
			  $(KERNEL_DIR)/src/drivers/usb/xhci/usb_hub.c \
			  $(KERNEL_DIR)/src/drivers/usb/xhci/usb_hotplug.c \
			  $(KERNEL_DIR)/src/core/spinlock.c \
			  $(KERNEL_DIR)/src/core/timers.c \
			  $(KERNEL_DIR)/src/core/dpc.c \
			  $(KERNEL_DIR)/src/core/switch.S \
			  $(KERNEL_DIR)/src/core/scheduler.c \
			  $(KERNEL_DIR)/src/core/semaphore.c \
			  $(KERNEL_DIR)/src/drivers/serial.c \
			  $(KERNEL_DIR)/src/smp/smp_boot.c \
			  $(KERNEL_DIR)/src/smp/trampoline.S \
			  $(KERNEL_DIR)/src/smp/smp_topology.c \
			  $(KERNEL_DIR)/src/core/input_queue.c \
			  $(KERNEL_DIR)/src/core/input_router.c \
			  $(KERNEL_DIR)/src/core/modal_session.c \
			  $(KERNEL_DIR)/src/core/modal_ui.c

# Separa quem é .c e quem é .S
KERNEL_C_SRCS = $(filter %.c, $(KERNEL_SRCS))
KERNEL_ASM_SRCS = $(filter %.S, $(KERNEL_SRCS))

# Define a lista de objetos finais (.o) combinando os dois tipos
KERNEL_OBJS = $(KERNEL_C_SRCS:.c=.o) $(KERNEL_ASM_SRCS:.S=.o)
KERNEL_DEPS = $(KERNEL_OBJS:.o=.d)
BOOT_DEPS = $(BOOT_OBJS:.o=.d)

# Regra para compilar .c
$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.c
	$(CC) $(CFLAGS_KERNEL) -c $< -o $@

# NOVA REGRA: Regra para compilar .S (Assembly)
$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.S
	$(CC) $(ASFLAGS_KERNEL) -c $< -o $@

# Linkagem Final do Kernel
kernel.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS_KERNEL) -o $@ $(KERNEL_OBJS)

-include $(KERNEL_DEPS) $(BOOT_DEPS)

# --- Imagem ---
hobbyos.img: BOOTX64.EFI kernel.elf $(BOOT_DIR)/startup.nsh
	dd if=/dev/zero of=$@ bs=1M count=64
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mmd -i $@ ::/EFI/fonts
	mmd -i $@ ::/EFI/images
	mcopy -i $@ BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ kernel.elf ::/kernel.elf
	mcopy -i $@ $(BOOT_DIR)/fonts/zap-light16.psf ::/EFI/fonts/zap-light16.psf
	mcopy -i $@ $(BOOT_DIR)/images/logo.bmp ::/EFI/images/logo.bmp
	mcopy -i $@ $(BOOT_DIR)/startup.nsh ::/startup.nsh

clean:
	# Artefatos do diretório raiz
	rm -f bootx64.so BOOTX64.EFI kernel.elf hobbyos.img

	# Objetos do bootloader (recursivo)
	find $(BOOT_DIR) -type f -name "*.o" -delete
	find $(BOOT_DIR) -type f -name "*.d" -delete

	# Objetos do kernel (recursivo)
	find $(KERNEL_DIR) -type f -name "*.o" -delete
	find $(KERNEL_DIR) -type f -name "*.d" -delete
	find $(KERNEL_DIR) -type f -name "*.su" -delete


QEMU ?= qemu-system-x86_64
ACCEL ?= auto
MACHINE ?= q35
MEM ?= 2G
OVMF_FD ?=
OVMF_CODE ?=
OVMF_VARS ?=
JOBS ?= 2
HMP ?= info status
TEXT ?=
ENTER ?= 0
KEY ?= esc
TIMEOUT ?= 45
PATTERN ?=

image: hobbyos.img

production-image:
	+$(MAKE) clean
	+$(MAKE) hobbyos.img SELFTEST=0 SELFTEST_AUTORUN=0 KERNEL_EXTRA_CFLAGS=
	@bash scripts/verify-production-test-policy.sh

production-test-policy:
	@bash scripts/verify-production-test-policy.sh

kernel-check:
	bash scripts/kernel-check.sh "$(JOBS)" "artifacts/build/kernel-check-j$(JOBS).log"

deps-check:
	SCOPE=$(or $(SCOPE),all) ARCH='$(ARCH)' CC='$(CC)' LD='$(LD)' OBJCOPY='$(OBJCOPY)' QEMU='$(QEMU)' EFIINC='$(EFIINC)' EFILIB='$(EFILIB)' OVMF_FD='$(OVMF_FD)' OVMF_CODE='$(OVMF_CODE)' OVMF_VARS='$(OVMF_VARS)' ACCEL='$(ACCEL)' scripts/check-deps.sh

deps-check-kernel:
	$(MAKE) deps-check SCOPE=kernel

deps-check-image:
	$(MAKE) deps-check SCOPE=image

deps-check-qemu:
	$(MAKE) deps-check SCOPE=qemu

run: hobbyos.img
	@SMP='$(SMP)' ACCEL='$(ACCEL)' MACHINE='$(MACHINE)' MEM='$(MEM)' QEMU='$(QEMU)' OVMF_FD='$(OVMF_FD)' OVMF_CODE='$(OVMF_CODE)' OVMF_VARS='$(OVMF_VARS)' scripts/qemu-run.sh

qemu-agent-start: hobbyos.img
	@SMP='$(SMP)' ACCEL='$(ACCEL)' MACHINE='$(MACHINE)' MEM='$(MEM)' QEMU='$(QEMU)' OVMF_FD='$(OVMF_FD)' OVMF_CODE='$(OVMF_CODE)' OVMF_VARS='$(OVMF_VARS)' scripts/qemu-agent.sh start
qemu-agent-status:
	@scripts/qemu-agent.sh status
qemu-agent-monitor:
	@python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock command '$(HMP)'
qemu-agent-type:
	@python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock text '$(TEXT)' $(if $(filter 1 yes true,$(ENTER)),--enter,)
qemu-agent-key:
	@python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock key '$(KEY)'
qemu-agent-logs:
	@scripts/qemu-agent.sh logs
qemu-agent-stop:
	@scripts/qemu-agent.sh stop
qemu-agent-wait:
	@scripts/wait-for-log.sh .qemu/qemu-serial.log '$(PATTERN)' '$(TIMEOUT)'
test-clock-matrix: hobbyos.img
	@bash scripts/test-clock-matrix.sh
test-taskman-visual:
	@bash scripts/test-taskman-visual.sh focused
test-interrupt-bringup:
	@bash scripts/test-interrupt-bringup.sh all

test-timer-clockevent:
	@bash scripts/test-timer-clockevent.sh all
selftest-kernel:
	@$(MAKE) kernel-check SELFTEST=1 SELFTEST_AUTORUN=1

# Teste básico - hub simples
run-hub: hobbyos.img
	qemu-system-x86_64 \
		-machine q35,accel=kvm \
		-cpu host \
		-m 2G \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=hobbyos.img,format=raw,cache=writeback \
		-serial file:qemu-serial.log \
		-device nec-usb-xhci,id=xhci \
		-device usb-hub,bus=xhci.0,port=1 \
		-device usb-kbd,bus=xhci.0,port=1.1

# Teste avançado - múltiplos dispositivos
run-hub-multi: hobbyos.img
	qemu-system-x86_64 \
		-machine q35,accel=kvm \
		-cpu host \
		-m 2G \
		-bios /usr/share/ovmf/OVMF.fd \
		-drive file=hobbyos.img,format=raw,cache=writeback \
		-serial stdio \
		-device nec-usb-xhci,id=xhci \
		-device usb-hub,bus=xhci.0,port=1 \
		-device usb-kbd,bus=xhci.0,port=1.1 \
		-device usb-kbd,bus=xhci.0,port=1.2

# TO Reconize Pen Drive in Linux: udisksctl mount -b /dev/sda1

# liga watch do XHCI de dump: xhci_set_debug_flags(XHCI_DBG_WATCH | XHCI_DBG_DUMP);

# to use SERIAL PORT (KNUP): sudo picocom -b 115200 /dev/ttyUSB0

stack-check:
	@$(MAKE) kernel-check JOBS=$(or $(JOBS),2) KERNEL_EXTRA_CFLAGS=-fstack-usage
	@python3 scripts/check-stack-usage.py --limit 2048
