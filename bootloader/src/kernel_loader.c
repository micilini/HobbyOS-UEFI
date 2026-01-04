#include "kernel_loader.h"
#include "utils.h"
#include <elf.h>



static int is_valid_elf_header(Elf64_Ehdr* header) {
    
    unsigned char magic[] = {0x7F, 'E', 'L', 'F'};
    if (memcmp(header->e_ident, magic, 4) != 0) return 0;

    
    if (header->e_ident[EI_CLASS] != ELFCLASS64) return 0;
    if (header->e_ident[EI_DATA] != ELFDATA2LSB) return 0;
    if (header->e_type != ET_EXEC) return 0;
    if (header->e_machine != EM_X86_64) return 0;
    if (header->e_version != EV_CURRENT) return 0;

    return 1;
}

void* load_elf_kernel(EFI_FILE* kernel_file) {
    EFI_STATUS status;
    
    
    Elf64_Ehdr header;
    UINTN size = sizeof(Elf64_Ehdr);

    
    uefi_call_wrapper(kernel_file->SetPosition, 2, kernel_file, 0);
    status = uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &size, &header);

    if (EFI_ERROR(status) || size != sizeof(Elf64_Ehdr)) {
        Print(L"[-] Error: Failed to read ELF Header\n");
        return NULL;
    }

    
    if (!is_valid_elf_header(&header)) {
        Print(L"[-] Error: Invalid Kernel ELF Format (Check Arch/Bitmode)\n");
        return NULL;
    }

    
    UINTN ph_table_size = header.e_phnum * header.e_phentsize;
    void* ph_buffer;
    
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, ph_table_size, &ph_buffer);
    if (EFI_ERROR(status)) {
        Print(L"[-] Error: Failed to allocate buffer for Program Headers\n");
        return NULL;
    }

    
    uefi_call_wrapper(kernel_file->SetPosition, 2, kernel_file, header.e_phoff);
    uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &ph_table_size, ph_buffer);

    
    Elf64_Phdr* phdr = (Elf64_Phdr*)ph_buffer;
    
    for (int i = 0; i < header.e_phnum; i++) {
        if (phdr->p_type == PT_LOAD) {
            
            UINTN pages = (phdr->p_memsz + 0xFFF) / 0x1000;
            
            
            EFI_PHYSICAL_ADDRESS segment_addr = phdr->p_paddr;

            
            status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiLoaderData, pages, &segment_addr);
            
            if (EFI_ERROR(status)) {
                Print(L"[-] Error: Failed to allocate memory at 0x%lx (Status: %r)\n", segment_addr, status);
                uefi_call_wrapper(BS->FreePool, 1, ph_buffer);
                return NULL;
            }

            
            uefi_call_wrapper(kernel_file->SetPosition, 2, kernel_file, phdr->p_offset);
            UINTN size_to_read = phdr->p_filesz;
            uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &size_to_read, (void*)segment_addr);

            
            
            if (phdr->p_memsz > phdr->p_filesz) {
                UINTN bss_size = phdr->p_memsz - phdr->p_filesz;
                uefi_call_wrapper(BS->SetMem, 3, (void*)(segment_addr + phdr->p_filesz), bss_size, 0);
            }
        }
        
        phdr = (Elf64_Phdr*)((UINT8*)phdr + header.e_phentsize);
    }

    
    uefi_call_wrapper(BS->FreePool, 1, ph_buffer);

    
    return (void*)header.e_entry;
}