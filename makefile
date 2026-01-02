# Makefile HobbyOS Modular

ARCH = x86_64
CC = gcc
OBJCOPY = objcopy

EFIINC = /usr/include/efi
EFIINCS = -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol

LIB = /usr/lib
EFILIB = /usr/lib/gnu-efi/lib
EFI_CRT_OBJS = $(LIB)/crt0-efi-$(ARCH).o
EFI_LDS = $(LIB)/elf_$(ARCH)_efi.lds

CFLAGS = $(EFIINCS) $(INCLUDES) -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -Wall -DEFI_FUNCTION_WRAPPER
LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic -L $(EFILIB) -L $(LIB) $(EFI_CRT_OBJS)

# Base System Files
SYS_OBJS = main.o commands.o utils.o disk.o filesystem.o editor.o graphics.o

# Final List
OBJS = $(SYS_OBJS)

all: hobbyos.img

# Rule for compiling .c files from the root
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for compiling .c files from the games/doom folder (future)
games/doom/%.o: games/doom/%.c
	$(CC) $(CFLAGS) -c $< -o $@

main.so: $(OBJS)
	ld $(LDFLAGS) $(OBJS) -o main.so -lefi -lgnuefi

main.efi: main.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym  -j .rel -j .rela -j .reloc --target=efi-app-$(ARCH) main.so main.efi

hobbyos.img: main.efi
	dd if=/dev/zero of=hobbyos.img bs=1M count=64
	mformat -i hobbyos.img -F ::
	mmd -i hobbyos.img ::/EFI
	mmd -i hobbyos.img ::/EFI/BOOT
	# Cria a estrutura de pastas do jogo
	mmd -i hobbyos.img ::/EFI/GAMES
	mmd -i hobbyos.img ::/EFI/GAMES/DOOM
	
	# Copia o Sistema
	mcopy -i hobbyos.img main.efi ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i hobbyos.img logo.bmp ::/EFI/BOOT/logo.bmp

clean:
	rm -f *.o *.so *.efi *.img

run: hobbyos.img
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -net none -drive format=raw,file=hobbyos.img