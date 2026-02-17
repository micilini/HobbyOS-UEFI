#include "src/boot.h"
#include "src/file.h"
#include "src/utils.h"
#include "src/kernel_loader.h"
#include "src/mem.h"
#include "src/gop.h"
#include "src/acpi.h"
#include "src/font.h"
#include "src/starter.h"
#include "src/bmp.h"

#include "src/serial.h"

#include <stdint.h>
#include <stddef.h>

EFI_SYSTEM_TABLE *SystemTable;




static void serial_write_u32_all(BootInfo *boot_info, uint32_t value)
{
    char buf[16];
    int idx = 0;

    if (value == 0)
    {
        serial_write_all(boot_info, "0");
        return;
    }

    while (value > 0 && idx < (int)(sizeof(buf) - 1))
    {
        uint32_t digit = value % 10;
        buf[idx++] = (char)('0' + digit);
        value /= 10;
    }

    
    for (int i = idx - 1; i >= 0; i--)
    {
        char out[2];
        out[0] = buf[i];
        out[1] = '\0';
        serial_write_all(boot_info, out);
    }
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SysTable)
{
    SystemTable = SysTable;
    InitializeLib(ImageHandle, SystemTable);

    BootInfo boot_info;
    ZeroMem(&boot_info, sizeof(BootInfo));

    serial_discover_ports(&boot_info);
    Print(L"[SERIAL] Found %u ports\n", (UINT32)boot_info.serial.count);
    for (UINT32 i = 0; i < boot_info.serial.count; i++)
    {
        Print(L"[SERIAL] port[%u] = 0x%04x kind=%u\n",
              i,
              (UINT16)boot_info.serial.ports[i].io_base,
              (UINT32)boot_info.serial.ports[i].kind);
    }
    serial_broadcast_boot_banner(&boot_info);

    Framebuffer *fb = get_framebuffer(&boot_info);
    if (fb == NULL)
    {
        Print(L"[-] Critical: Failed to initialize Graphics Output Protocol.\n");
        serial_write_all(&boot_info, "[UEFI] ERROR: GOP failed.\n");
        while (1) { }
    }

    
    
    
    serial_write_all(&boot_info, "[UEFI] GOP Mode Selected: W=0x");
    serial_write_hex64_all(&boot_info, (uint64_t)fb->Width);
    serial_write_all(&boot_info, " H=0x");
    serial_write_hex64_all(&boot_info, (uint64_t)fb->Height);
    serial_write_all(&boot_info, " PPSL=0x");
    serial_write_hex64_all(&boot_info, (uint64_t)fb->PixelsPerScanLine);
    serial_write_all(&boot_info, " FB=0x");
    serial_write_hex64_all(&boot_info, (uint64_t)fb->BaseAddress);
    serial_write_all(&boot_info, " Size=0x");
    serial_write_hex64_all(&boot_info, (uint64_t)fb->BufferSize);
    serial_write_all(&boot_info, "\n");

    Print(L"HobbyOS UEFI (Bootloader)\nVersion: 0.2\nDevelopment By: Micilini\n\n\n\n");

    Print(L"[*] Starting Bootloader...\n");
    serial_write_all(&boot_info, "[UEFI] Starting Bootloader...\n");

    EFI_FILE *root = open_root_volume(ImageHandle);
    if (root == NULL)
    {
        Print(L"[-] Critical: Failed to open root volume.\n");
        serial_write_all(&boot_info, "[UEFI] ERROR: open_root_volume failed.\n");
        while (1) { }
    }

    EFI_FILE *kernel_file = load_file(root, L"kernel.elf");
    if (kernel_file == NULL)
    {
        Print(L"[-] Critical: Kernel file not found.\n");
        serial_write_all(&boot_info, "[UEFI] ERROR: kernel.elf not found.\n");
        while (1) { }
    }
    Print(L"[*] Kernel file has been loaded successfully\n");
    serial_write_all(&boot_info, "[UEFI] kernel.elf loaded.\n");

    void *entry_point = load_elf_kernel(kernel_file);
    if (entry_point == NULL)
    {
        Print(L"[-] Critical: Failed to load Kernel ELF into memory.\n");
        serial_write_all(&boot_info, "[UEFI] ERROR: load_elf_kernel failed.\n");
        while (1) { }
    }
    Print(L"[*] Kernel has been successfully loaded into memory\n");

    serial_write_all(&boot_info, "[UEFI] ELF mapped, entry_point=0x");
    serial_write_hex64_all(&boot_info, (uint64_t)entry_point);
    serial_write_all(&boot_info, "\n");

    Print(L"[*] Video Memory Address Obtained: 0x%lx\n", (UINT64)fb->BaseAddress);

    void *rsdp = find_acpi_rsdp();
    if (rsdp == NULL)
    {
        Print(L"[-] Warning: ACPI RSDP not found.\n");
        serial_write_all(&boot_info, "[UEFI] WARN: ACPI RSDP not found.\n");
    }
    else
    {
        Print(L"[*] ACPI RSDP Address Obtained: 0x%lx\n", (UINT64)rsdp);
        serial_write_all(&boot_info, "[UEFI] ACPI RSDP=0x");
        serial_write_hex64_all(&boot_info, (uint64_t)rsdp);
        serial_write_all(&boot_info, "\n");
    }

    Psf1_Font *font = load_psf_font(root, L"EFI\\fonts\\zap-light16.psf");
    if (font == NULL)
    {
        Print(L"[-] Error: Font file not found or invalid.\n");
        serial_write_all(&boot_info, "[UEFI] WARN: font not found.\n");
    }
    else
    {
        Print(L"[*] Font loaded successfully (CharSize: %d)\n", font->psf_header->charsize);
        serial_write_all(&boot_info, "[UEFI] Font OK.\n");
    }

    SimpleImage *logo = load_bmp_image(root, L"EFI\\images\\logo.bmp");
    if (logo == NULL)
    {
        Print(L"[-] Warning: Logo not found.\n");
        serial_write_all(&boot_info, "[UEFI] WARN: logo not found.\n");
    }
    else
    {
        Print(L"[*] Logo loaded successfully (%dx%d)\n", logo->Width, logo->Height);
        serial_write_all(&boot_info, "[UEFI] Logo OK.\n");
    }

    MemoryMap *mem_map = get_memory_map();
    if (mem_map == NULL)
    {
        Print(L"[-] Critical: Failed to retrieve Memory Map.\n");
        serial_write_all(&boot_info, "[UEFI] ERROR: get_memory_map failed.\n");
        while (1) { }
    }
    Print(L"[*] Memory Map retrieved successfully (Key: %d)\n", mem_map->MapKey);
    serial_write_all(&boot_info, "[UEFI] MemoryMap OK.\n");

    boot_info.framebuffer = fb;
    boot_info.font = font;
    boot_info.logo = logo;
    boot_info.rsdp = rsdp;
    boot_info.memory_map = mem_map;

    Print(L"[*] All checks passed. Handing over control to Kernel...\n");
    serial_write_all(&boot_info, "[UEFI] Jumping to kernel...\n");

    
    start_kernel(ImageHandle, entry_point, &boot_info);

    return EFI_SUCCESS;
}
