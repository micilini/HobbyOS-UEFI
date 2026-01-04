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


EFI_SYSTEM_TABLE *SystemTable;

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SysTable) {
    SystemTable = SysTable;
    InitializeLib(ImageHandle, SystemTable);

    
    Framebuffer* fb = get_framebuffer();
    if (fb == NULL) {
        Print(L"[-] Critical: Failed to initialize Graphics Output Protocol.\n");
        while(1);
    }
    
    
    Print(L"HobbyOS UEFI (Bootloader)\nVersion: 0.2\nDevelopment By: Micilini\n\n\n");

    
    Print(L"[*] Starting Bootloader...\n");

    
    EFI_FILE* root = open_root_volume(ImageHandle);
    if (root == NULL) {
        Print(L"[-] Critical: Failed to open root volume.\n");
        while(1);
    }

    
    EFI_FILE* kernel_file = load_file(root, L"kernel.elf");
    if (kernel_file == NULL) {
        Print(L"[-] Critical: Kernel file not found.\n");
        while(1);
    }
    Print(L"[*] Kernel file has been loaded successfully\n");

    
    void* entry_point = load_elf_kernel(kernel_file);
    if (entry_point == NULL) {
        Print(L"[-] Critical: Failed to load Kernel ELF into memory.\n");
        while(1);
    }
    Print(L"[*] Kernel has been successfully loaded into memory\n");

    
    Print(L"[*] Video Memory Address Obtained: 0x%lx\n", (UINT64)fb->BaseAddress);

    
    void* rsdp = find_acpi_rsdp();
    if (rsdp == NULL) {
        Print(L"[-] Warning: ACPI RSDP not found.\n");
    } else {
        Print(L"[*] ACPI RSDP Address Obtained: 0x%lx\n", (UINT64)rsdp);
    }

    
    
    Psf1_Font* font = load_psf_font(root, L"EFI\\fonts\\zap-light16.psf");
    if (font == NULL) {
        Print(L"[-] Error: Font file not found or invalid.\n");
    } else {
        Print(L"[*] Font loaded successfully (CharSize: %d)\n", font->psf_header->charsize);
    }

    
    SimpleImage* logo = load_bmp_image(root, L"EFI\\images\\logo.bmp");
    if (logo == NULL) {
        Print(L"[-] Warning: Logo not found.\n");
    } else {
        Print(L"[*] Logo loaded successfully (%dx%d)\n", logo->Width, logo->Height);
    }

    
    
    
    MemoryMap* mem_map = get_memory_map();
    if (mem_map == NULL) {
        Print(L"[-] Critical: Failed to retrieve Memory Map.\n");
        while(1);
    }
    Print(L"[*] Memory Map retrieved successfully (Key: %d)\n", mem_map->MapKey);

    
    Print(L"[*] All checks passed. Handing over control to Kernel...\n");
    
    
    

    start_kernel(ImageHandle, entry_point, fb, font, logo, rsdp, mem_map); 
    
    
    
    return EFI_SUCCESS;
}