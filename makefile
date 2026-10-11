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
KERNEL_C_POLICY_FLAGS = -O2 -fno-strict-aliasing -fno-stack-protector -fno-delete-null-pointer-checks -fno-omit-frame-pointer
KERNEL_WARNING_FLAGS = -Wall -Wextra -Wundef -Wmissing-prototypes -Wvla -Wframe-larger-than=2048 -Werror
KERNEL_EXTRA_CFLAGS ?=
SELFTEST ?= 0
SELFTEST_AUTORUN ?= 0
SERIAL_PROOF_DIAGNOSTIC ?= 0
SERIAL_PROOF_ID ?= 0
ifeq ($(SERIAL_PROOF_DIAGNOSTIC),1)
ifneq ($(SELFTEST)$(IRQ_LATENCY),11)
$(error SERIAL_PROOF_DIAGNOSTIC=1 requires SELFTEST=1 IRQ_LATENCY=1)
endif
else ifneq ($(SERIAL_PROOF_DIAGNOSTIC),0)
$(error SERIAL_PROOF_DIAGNOSTIC must be 0 or 1)
endif
FORMAT_TEST ?= 0
DEBUG_ASSERT ?= 1
IRQ_LATENCY ?= 0
IRQ_ENTRY_DIAGNOSTIC ?= 0
IRQ_LATENCY_DETAILS ?= 1
IRQ_LOCAL_TSC_ORDER ?= 0
ifeq ($(IRQ_LOCAL_TSC_ORDER),1)
ifneq ($(IRQ_LATENCY)$(SELFTEST),11)
$(error IRQ_LOCAL_TSC_ORDER=1 requires IRQ_LATENCY=1 SELFTEST=1)
endif
else ifneq ($(IRQ_LOCAL_TSC_ORDER),0)
$(error IRQ_LOCAL_TSC_ORDER must be 0 or 1)
endif
ifeq ($(IRQ_LATENCY_DETAILS),0)
ifneq ($(IRQ_ENTRY_DIAGNOSTIC),1)
$(error IRQ_LATENCY_DETAILS=0 requires IRQ_ENTRY_DIAGNOSTIC=1)
endif
else ifneq ($(IRQ_LATENCY_DETAILS),1)
$(error IRQ_LATENCY_DETAILS must be 0 or 1)
endif
ifeq ($(IRQ_ENTRY_DIAGNOSTIC),1)
ifneq ($(IRQ_LATENCY)$(SELFTEST),11)
$(error IRQ_ENTRY_DIAGNOSTIC=1 requires IRQ_LATENCY=1 SELFTEST=1)
endif
else ifneq ($(IRQ_ENTRY_DIAGNOSTIC),0)
$(error IRQ_ENTRY_DIAGNOSTIC must be 0 or 1)
endif
IRQ_LATENCY_SRCS =
ifeq ($(IRQ_LATENCY),1)
ifneq ($(DEBUG_ASSERT),1)
$(error IRQ_LATENCY=1 requires DEBUG_ASSERT=1)
endif
IRQ_LATENCY_SRCS += $(KERNEL_DIR)/src/core/irq_latency_interior.c
ifeq ($(IRQ_ENTRY_DIAGNOSTIC),1)
IRQ_LATENCY_SRCS += $(KERNEL_DIR)/src/core/irq_latency_heap.c
endif
IRQ_LATENCY_SRCS += $(KERNEL_DIR)/src/core/irq_latency_time.c $(KERNEL_DIR)/src/core/irq_latency_pairs.c
IRQ_LATENCY_SRCS += $(KERNEL_DIR)/src/core/irq_latency_record.c
IRQ_LATENCY_SRCS += $(KERNEL_DIR)/src/core/irq_latency_component.c
IRQ_LATENCY_SRCS += $(KERNEL_DIR)/src/core/irq_latency_scope.c
IRQ_LATENCY_SRCS += $(KERNEL_DIR)/src/core/irq_latency_boundary.c \
                    $(KERNEL_DIR)/src/core/irq_latency_calibration.c \
                    $(KERNEL_DIR)/src/core/irq_latency_arch.S
else ifneq ($(IRQ_LATENCY),0)
$(error IRQ_LATENCY must be 0 or 1)
endif
ASSERT_TEST ?= 0
ARCH_TEST ?= 0
ARCH_NEGATIVE_GP ?= 0
ifeq ($(DEBUG_ASSERT),1)
else ifneq ($(DEBUG_ASSERT),0)
$(error DEBUG_ASSERT must be 0 or 1)
endif
ifeq ($(SELFTEST_AUTORUN),1)
ifneq ($(SELFTEST),1)
$(error SELFTEST_AUTORUN=1 requires SELFTEST=1)
endif
endif
ifeq ($(FORMAT_TEST),1)
ifneq ($(SELFTEST),1)
$(error FORMAT_TEST=1 requires SELFTEST=1)
endif
endif
ifeq ($(ASSERT_TEST),1)
ifneq ($(SELFTEST),1)
$(error ASSERT_TEST=1 requires SELFTEST=1)
endif
ifneq ($(DEBUG_ASSERT),1)
$(error ASSERT_TEST=1 requires DEBUG_ASSERT=1)
endif
endif
ifeq ($(ARCH_TEST),1)
ifneq ($(SELFTEST),1)
$(error ARCH_TEST=1 requires SELFTEST=1)
endif
ifneq ($(DEBUG_ASSERT),1)
$(error ARCH_TEST=1 requires DEBUG_ASSERT=1)
endif
else ifneq ($(ARCH_TEST),0)
$(error ARCH_TEST must be 0 or 1)
endif
ifeq ($(ARCH_NEGATIVE_GP),1)
ifneq ($(ARCH_TEST),1)
$(error ARCH_NEGATIVE_GP=1 requires ARCH_TEST=1)
endif
else ifneq ($(ARCH_NEGATIVE_GP),0)
$(error ARCH_NEGATIVE_GP must be 0 or 1)
endif
SELFTEST_CFLAGS =
ifeq ($(SERIAL_PROOF_DIAGNOSTIC),1)
SELFTEST_CFLAGS += -DHOBBYOS_SERIAL_PROOF_DIAGNOSTIC=1
SELFTEST_CFLAGS += -DHOBBYOS_SERIAL_PROOF_ID=$(SERIAL_PROOF_ID)
endif
SELFTEST_CFLAGS += -DHOBBYOS_DEBUG_ASSERT=$(DEBUG_ASSERT)
SELFTEST_CFLAGS += -DHOBBYOS_IRQ_LATENCY=$(IRQ_LATENCY)
SELFTEST_CFLAGS += -DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=$(IRQ_ENTRY_DIAGNOSTIC)
SELFTEST_CFLAGS += -DHOBBYOS_IRQ_LATENCY_DETAILS=$(IRQ_LATENCY_DETAILS)
SELFTEST_CFLAGS += -DHOBBYOS_IRQ_LOCAL_TSC_ORDER=$(IRQ_LOCAL_TSC_ORDER)
ifeq ($(SELFTEST),1)
SELFTEST_CFLAGS += -DHOBBYOS_SELFTEST=1
endif
FRAMEBUFFER_REFERENCE ?= 0
ifeq ($(FRAMEBUFFER_REFERENCE),1)
ifneq ($(SELFTEST),1)
$(error FRAMEBUFFER_REFERENCE=1 requires SELFTEST=1)
endif
SELFTEST_CFLAGS += -DHOBBYOS_FRAMEBUFFER_REFERENCE=1
endif
ifeq ($(SELFTEST_AUTORUN),1)
SELFTEST_CFLAGS += -DHOBBYOS_SELFTEST_AUTORUN=1
endif
FORMAT_TEST_SRCS =
ifeq ($(FORMAT_TEST),1)
SELFTEST_CFLAGS += -DHOBBYOS_FORMAT_TEST=1
FORMAT_TEST_SRCS += $(KERNEL_DIR)/src/libc/format_selftest.c
endif
ASSERT_TEST_SRCS =
ifeq ($(ASSERT_TEST),1)
SELFTEST_CFLAGS += -DHOBBYOS_ASSERT_TEST=1
ASSERT_TEST_SRCS += $(KERNEL_DIR)/src/core/assert_selftest.c
endif
ARCH_TEST_SRCS =
ifeq ($(ARCH_TEST),1)
SELFTEST_CFLAGS += -DHOBBYOS_ARCH_TEST=1
ARCH_TEST_SRCS += $(KERNEL_DIR)/src/cpu/arch_selftest.c
endif
DIAGNOSTIC_COMMAND_SRCS =
ifeq ($(SELFTEST),1)
DIAGNOSTIC_COMMAND_SRCS += $(KERNEL_DIR)/src/core/selftest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_panic.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_irq.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_usbdiag.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_smpstress.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_taskdiag.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_tasktest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_schedtest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_synctest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_accounttest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_killtest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_reaptest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_inputtest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_modaltest.c \
                         $(KERNEL_DIR)/src/shell/commands/cmd_taskmantest.c
endif
LOCK_TEST_SRCS =
ALLOC_TEST_SRCS =
BUILD_PROFILE_TEST_SRCS =
MEMORY_OBSERVATION_SRCS =
ifneq ($(filter 1,$(DEBUG_ASSERT) $(SELFTEST)),)
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/page_table_decode.c \
                          $(KERNEL_DIR)/src/cpu/cpu_control_snapshot.c
endif
ifeq ($(SELFTEST),1)
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/memory_observe_decode.c \
                          $(KERNEL_DIR)/src/memory/memory_observation.c \
                          $(KERNEL_DIR)/src/shell/commands/cmd_memory_observation.c
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/tlb_fixture.c \
                          $(KERNEL_DIR)/src/memory/tlb_proof.c \
                          $(KERNEL_DIR)/src/shell/commands/cmd_tlbtest.c
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/mapping_probe.c \
                          $(KERNEL_DIR)/src/memory/mapping_probe_match.c \
                          $(KERNEL_DIR)/src/memory/mapping_probe_read.S \
                          $(KERNEL_DIR)/src/shell/commands/cmd_mappingtest.c
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/physical_fixture.c \
                          $(KERNEL_DIR)/src/shell/commands/cmd_phystest.c
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/ram_map_fixture.c \
                          $(KERNEL_DIR)/src/shell/commands/cmd_rammap.c
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/null_probe.c \
                          $(KERNEL_DIR)/src/memory/null_probe_match.c \
                          $(KERNEL_DIR)/src/memory/null_probe_read.S \
                          $(KERNEL_DIR)/src/shell/commands/cmd_lowmap.c
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/protection_probe.c \
                          $(KERNEL_DIR)/src/memory/protection_probe_match.c \
                          $(KERNEL_DIR)/src/memory/protection_probe_access.S \
                          $(KERNEL_DIR)/src/shell/commands/cmd_wxtest.c
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/guard_probe.c \
                          $(KERNEL_DIR)/src/memory/guard_probe_match.c \
                          $(KERNEL_DIR)/src/memory/guard_probe_access.S \
                          $(KERNEL_DIR)/src/memory/stack_test.c \
                          $(KERNEL_DIR)/src/shell/commands/cmd_stacktest.c
ifeq ($(IRQ_LATENCY),1)
MEMORY_OBSERVATION_SRCS += $(KERNEL_DIR)/src/memory/irq_latency_test.c \
                          $(KERNEL_DIR)/src/memory/stack_report.c \
                          $(KERNEL_DIR)/src/memory/warning_report.c \
                          $(KERNEL_DIR)/src/memory/irq_latency_stress.c \
                          $(KERNEL_DIR)/src/shell/commands/cmd_irqlatency.c
endif
LOCK_TEST_SRCS += $(KERNEL_DIR)/src/shell/commands/cmd_locktest.c
ALLOC_TEST_SRCS += $(KERNEL_DIR)/src/shell/commands/cmd_alloctest.c
BUILD_PROFILE_TEST_SRCS += $(KERNEL_DIR)/src/shell/commands/cmd_profiletest.c
endif
ifeq ($(ARCH_NEGATIVE_GP),1)
SELFTEST_CFLAGS += -DHOBBYOS_ARCH_NEGATIVE_GP=1
endif
CFLAGS_KERNEL = $(KERNEL_COMMON_FLAGS) $(KERNEL_C_POLICY_FLAGS) -std=gnu11 -MMD -MP $(KERNEL_WARNING_FLAGS) -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion $(KERNEL_EXTRA_CFLAGS) $(SELFTEST_CFLAGS)
ASFLAGS_KERNEL = $(KERNEL_COMMON_FLAGS) -DHOBBYOS_DEBUG_ASSERT=$(DEBUG_ASSERT) -DHOBBYOS_IRQ_LATENCY=$(IRQ_LATENCY) -DHOBBYOS_IRQ_ENTRY_DIAGNOSTIC=$(IRQ_ENTRY_DIAGNOSTIC) -DHOBBYOS_IRQ_LOCAL_TSC_ORDER=$(IRQ_LOCAL_TSC_ORDER)
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
              $(KERNEL_DIR)/src/core/clocksource.c \
              $(KERNEL_DIR)/src/core/pvclock.c \
              $(KERNEL_DIR)/src/core/task_metrics.c \
			  $(KERNEL_DIR)/src/core/task_format.c \
			  $(KERNEL_DIR)/src/core/runtime_ready.c \
			  $(KERNEL_DIR)/src/core/build_profile.c \
			  $(KERNEL_DIR)/src/core/task_lifecycle.c \
              $(KERNEL_DIR)/src/graphics/splash.c \
              $(KERNEL_DIR)/src/utils/bitmap.c \
              $(KERNEL_DIR)/src/memory/gdt.c \
              $(KERNEL_DIR)/src/memory/pmem.c \
              $(KERNEL_DIR)/src/memory/physical_contract.c \
              $(KERNEL_DIR)/src/memory/physical_translation.c \
              $(KERNEL_DIR)/src/memory/boot_address.c \
              $(KERNEL_DIR)/src/memory/mapping_core.c \
              $(KERNEL_DIR)/src/memory/owned_mapping.c \
			  $(KERNEL_DIR)/src/memory/device_origin.c \
              $(KERNEL_DIR)/src/memory/dma_buffer.c \
              $(KERNEL_DIR)/src/memory/paging.c \
              $(KERNEL_DIR)/src/memory/ram_map_plan.c \
              $(KERNEL_DIR)/src/memory/ram_map_boot.c \
              $(KERNEL_DIR)/src/memory/paging_entry.c \
              $(KERNEL_DIR)/src/memory/paging_build.c \
              $(KERNEL_DIR)/src/memory/image_permissions.c \
              $(KERNEL_DIR)/src/memory/image_permissions_boot.c \
              $(KERNEL_DIR)/src/memory/image_walk.c \
              $(KERNEL_DIR)/src/memory/stack_domain.c \
              $(KERNEL_DIR)/src/memory/stack_map.c \
              $(KERNEL_DIR)/src/memory/kernel_stack.c \
              $(KERNEL_DIR)/src/memory/stack_fault.c \
              $(KERNEL_DIR)/src/memory/bootstrap_claim.c \
              $(KERNEL_DIR)/src/memory/bootstrap_tables.c \
              $(KERNEL_DIR)/src/memory/bootstrap_lifetime.c \
              $(KERNEL_DIR)/src/memory/low_fault.c \
              $(KERNEL_DIR)/src/memory/cache_attributes.c \
              $(KERNEL_DIR)/src/memory/tlb_interval.c \
              $(KERNEL_DIR)/src/memory/tlb_deferred.c \
              $(KERNEL_DIR)/src/memory/kernel_tlb.c \
              $(KERNEL_DIR)/src/smp/cpu_request.c \
              $(KERNEL_DIR)/src/smp/cpu_request_state.c \
              $(KERNEL_DIR)/src/memory/heap.c \
              $(KERNEL_DIR)/src/graphics/graphics.c \
              $(KERNEL_DIR)/src/acpi/acpi.c \
              $(KERNEL_DIR)/src/acpi/madt.c \
              $(KERNEL_DIR)/src/apic/legacy_pic.c \
              $(KERNEL_DIR)/src/cpu/cpu.c \
              $(KERNEL_DIR)/src/cpu/control_policy.c \
              $(KERNEL_DIR)/src/cpu/mmio.c \
              $(KERNEL_DIR)/src/apic/lapic.c \
              $(KERNEL_DIR)/src/apic/lapic_delivery.c \
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
				  $(FORMAT_TEST_SRCS) \
				  $(ASSERT_TEST_SRCS) \
				  $(ARCH_TEST_SRCS) \
			  $(KERNEL_DIR)/src/shell/shell.c \
			  $(KERNEL_DIR)/src/shell/diagnostic_result.c \
			  $(KERNEL_DIR)/src/shell/commands/registry.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_help.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_clear.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_version.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_mem.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_cpu.c \
		      $(KERNEL_DIR)/src/shell/commands/cmd_pci.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_acpi.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_echo.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_ps.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_kill.c \
			  $(KERNEL_DIR)/src/shell/commands/taskman_view.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_taskman.c \
			  $(DIAGNOSTIC_COMMAND_SRCS) \
			  $(LOCK_TEST_SRCS) \
			  $(ALLOC_TEST_SRCS) \
			  $(BUILD_PROFILE_TEST_SRCS) \
			  $(MEMORY_OBSERVATION_SRCS) \
			  $(IRQ_LATENCY_SRCS) \
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
			  $(KERNEL_DIR)/src/drivers/usb/xhci/xhci_dma.c \
			  $(KERNEL_DIR)/src/drivers/usb/xhci/usb_ownership.c \
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
			  $(KERNEL_DIR)/src/smp/ap_entry.S \
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

# graphics.c/.h are protected by the TASKMAN visual-maintenance contract.
# Keep their two legacy public definitions untouched until that maintenance.
$(KERNEL_DIR)/src/graphics/graphics.o: CFLAGS_KERNEL += -Wno-missing-prototypes

# Prevent GCC from recognizing the kernel's own byte loops as libc calls.
$(KERNEL_DIR)/src/libc/memory.o: CFLAGS_KERNEL += -fno-tree-loop-distribute-patterns

# Regra para compilar .c
$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.c
	$(CC) $(CFLAGS_KERNEL) -c $< -o $@

# NOVA REGRA: Regra para compilar .S (Assembly)
$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.S
	$(CC) $(ASFLAGS_KERNEL) -c $< -o $@

$(KERNEL_DIR)/src/smp/trampoline.o: $(KERNEL_DIR)/src/cpu/control_policy_bits.h $(KERNEL_DIR)/src/memory/paging_boot_bits.h

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
HOBBYOS_VALIDATION_CONTRACT ?= guarded-kernel-stacks

image: hobbyos.img

production-image:
	+$(MAKE) clean
	+$(MAKE) hobbyos.img SELFTEST=0 SELFTEST_AUTORUN=0 FORMAT_TEST=0 \
		ASSERT_TEST=0 ARCH_TEST=0 ARCH_NEGATIVE_GP=0 DEBUG_ASSERT=0 IRQ_LATENCY=0 \
		SERIAL_PROOF_DIAGNOSTIC=0 KERNEL_EXTRA_CFLAGS=
	@HOBBYOS_VALIDATION_CONTRACT='$(HOBBYOS_VALIDATION_CONTRACT)' bash scripts/verify-production-test-policy.sh

production-test-policy:
	@HOBBYOS_VALIDATION_CONTRACT='$(HOBBYOS_VALIDATION_CONTRACT)' bash scripts/verify-production-test-policy.sh

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
