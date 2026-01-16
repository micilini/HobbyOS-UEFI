# Makefile HobbyOS - Modular & Fixed

ARCH = x86_64

# Pastas
BOOT_DIR = bootloader
KERNEL_DIR = kernel

# Ferramentas
CC = gcc
LD = ld
OBJCOPY = objcopy

# --- CONFIGURAÇÃO CRÍTICA DO GNU-EFI ---
# Ajuste estes caminhos se o seu Linux for diferente (ex: /usr/lib64/gnuefi)
EFILIB = /usr/lib
EFIINC = /usr/include/efi

# O arquivo mágico (CRT0) e o Script de Linkagem
CRT0 = $(EFILIB)/crt0-efi-$(ARCH).o
LDSCRIPT = $(EFILIB)/elf_$(ARCH)_efi.lds

# Flags Bootloader
EFIINCS = -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol
CFLAGS_EFI = $(EFIINCS) -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -DEFI_FUNCTION_WRAPPER -Wall
# Note que removemos o script do LDFLAGS aqui para colocar na regra de compilação explicitamente
LDFLAGS_EFI = -nostdlib -znocombreloc -shared -Bsymbolic -L $(EFILIB) -L /usr/lib -lgnuefi -lefi

# Flags Kernel
CFLAGS_KERNEL = -ffreestanding -mno-red-zone -mgeneral-regs-only -Wall
LDFLAGS_KERNEL = -T $(KERNEL_DIR)/link.ld -static -Bsymbolic -nostdlib

all: hobbyos.img

# --- Bootloader ---

# Lista de Objetos do Bootloader (Main + Módulos da pasta src)
BOOT_OBJS = $(BOOT_DIR)/main.o \
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
KERNEL_SRCS = $(KERNEL_DIR)/kernel.c \
              $(KERNEL_DIR)/src/core/kernel_init.c \
              $(KERNEL_DIR)/src/core/panic.c \
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
              $(KERNEL_DIR)/src/cpu/cpu.c \
              $(KERNEL_DIR)/src/apic/lapic.c \
              $(KERNEL_DIR)/src/apic/ioapic.c \
              $(KERNEL_DIR)/src/timer/hpet.c \
			  $(KERNEL_DIR)/src/graphics/console.c \
			  $(KERNEL_DIR)/src/graphics/terminal.c \
			  $(KERNEL_DIR)/src/core/idt.c \
			  $(KERNEL_DIR)/src/core/interrupts.c \
			  $(KERNEL_DIR)/src/libc/memory.c \
			  $(KERNEL_DIR)/src/drivers/ps2.c \
              $(KERNEL_DIR)/src/drivers/keyboard.c \
              $(KERNEL_DIR)/src/libc/string.c \
			  $(KERNEL_DIR)/src/shell/shell.c \
			  $(KERNEL_DIR)/src/shell/commands/registry.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_help.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_clear.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_version.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_mem.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_panic.c \
			  $(KERNEL_DIR)/src/shell/commands/cmd_cpu.o \
		      $(KERNEL_DIR)/src/shell/commands/cmd_pci.o \
			  $(KERNEL_DIR)/src/shell/commands/cmd_acpi.o \
			  $(KERNEL_DIR)/src/shell/commands/cmd_irq.o \
			  $(KERNEL_DIR)/src/shell/commands/cmd_echo.o \
			  $(KERNEL_DIR)/src/core/irq_stats.o \
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
			  $(KERNEL_DIR)/src/drivers/usb/xhci/xhci.c

# Transforma .c em .o
KERNEL_OBJS = $(KERNEL_SRCS:.c=.o)

# Regra genérica para compilar qualquer .c do Kernel
# Note o $(CFLAGS_KERNEL) que já configuramos antes
$(KERNEL_DIR)/%.o: $(KERNEL_DIR)/%.c
	$(CC) $(CFLAGS_KERNEL) -c $< -o $@

# Linkagem Final do Kernel
kernel.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS_KERNEL) -o $@ $(KERNEL_OBJS)

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
	rm -f *.o *.so *.EFI *.elf *.img bootx64.so BOOTX64.EFI kernel.elf hobbyos.img

	# Objetos do bootloader (recursivo)
	find $(BOOT_DIR) -type f -name "*.o" -delete

	# Objetos do kernel (recursivo)
	find $(KERNEL_DIR) -type f -name "*.o" -delete


run: hobbyos.img
	qemu-system-x86_64 \
		-machine q35,accel=kvm \
		-cpu host \
		-smp 4,sockets=1,cores=4,threads=1 \
		-m 2G \
		-bios /usr/share/ovmf/OVMF.fd \
		-net none \
		-drive file=hobbyos.img,format=raw,cache=writeback \
		-serial stdio \
		-device nec-usb-xhci,id=xhci,msi=on,msix=off -device usb-kbd,bus=xhci.0

# TO Reconize Pen Drive in Linux: udisksctl mount -b /dev/sda1

# liga watch do XHCI de dump: xhci_set_debug_flags(XHCI_DBG_WATCH | XHCI_DBG_DUMP);
