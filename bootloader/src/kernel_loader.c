#include "kernel_loader.h"
#include "utils.h"
#include <elf.h>

static inline EFI_PHYSICAL_ADDRESS align_down_4k(EFI_PHYSICAL_ADDRESS x)
{
    return (EFI_PHYSICAL_ADDRESS)(x & ~((EFI_PHYSICAL_ADDRESS)0xFFF));
}

static inline EFI_PHYSICAL_ADDRESS align_up_4k(EFI_PHYSICAL_ADDRESS x)
{
    return (EFI_PHYSICAL_ADDRESS)((x + 0xFFF) & ~((EFI_PHYSICAL_ADDRESS)0xFFF));
}

static int is_valid_elf_header(Elf64_Ehdr *header)
{
    unsigned char magic[] = {0x7F, 'E', 'L', 'F'};
    if (memcmp(header->e_ident, magic, 4) != 0)
        return 0;

    if (header->e_ident[EI_CLASS] != ELFCLASS64)
        return 0;
    if (header->e_ident[EI_DATA] != ELFDATA2LSB)
        return 0;
    if (header->e_type != ET_EXEC)
        return 0;
    if (header->e_machine != EM_X86_64)
        return 0;
    if (header->e_version != EV_CURRENT)
        return 0;

    return 1;
}

void *load_elf_kernel(EFI_FILE *kernel_file)
{
    EFI_STATUS status;

    Elf64_Ehdr header;
    UINTN size = sizeof(Elf64_Ehdr);

    uefi_call_wrapper(kernel_file->SetPosition, 2, kernel_file, 0);
    status = uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &size, &header);

    if (EFI_ERROR(status) || size != sizeof(Elf64_Ehdr))
    {
        Print(L"[-] Error: Failed to read ELF Header\n");
        return NULL;
    }

    if (!is_valid_elf_header(&header))
    {
        Print(L"[-] Error: Invalid Kernel ELF Format (Check Arch/Bitmode)\n");
        return NULL;
    }

    UINTN ph_table_size = header.e_phnum * header.e_phentsize;
    void *ph_buffer = NULL;

    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, ph_table_size, &ph_buffer);
    if (EFI_ERROR(status) || !ph_buffer)
    {
        Print(L"[-] Error: Failed to allocate buffer for Program Headers\n");
        return NULL;
    }

    uefi_call_wrapper(kernel_file->SetPosition, 2, kernel_file, header.e_phoff);
    status = uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &ph_table_size, ph_buffer);
    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Failed to read Program Headers (Status: %r)\n", status);
        uefi_call_wrapper(BS->FreePool, 1, ph_buffer);
        return NULL;
    }

    EFI_PHYSICAL_ADDRESS min_start = (EFI_PHYSICAL_ADDRESS)(~0ULL);
    EFI_PHYSICAL_ADDRESS max_end = 0;

    Elf64_Phdr *phdr = (Elf64_Phdr *)ph_buffer;

    Print(L"[DEBUG] Scanning ELF Segments...\n");
    for (int i = 0; i < header.e_phnum; i++)
    {

        Print(L"  Seg %d: Type=%x, PAddr=0x%lx, MemSz=0x%lx\n",
              i, phdr->p_type, phdr->p_paddr, phdr->p_memsz);

        if (phdr->p_type == PT_LOAD && phdr->p_memsz > 0 && phdr->p_paddr >= 0x10000)
        {
            EFI_PHYSICAL_ADDRESS seg_start = align_down_4k((EFI_PHYSICAL_ADDRESS)phdr->p_paddr);
            EFI_PHYSICAL_ADDRESS seg_end = align_up_4k((EFI_PHYSICAL_ADDRESS)(phdr->p_paddr + phdr->p_memsz));

            if (seg_start < min_start)
                min_start = seg_start;
            if (seg_end > max_end)
                max_end = seg_end;
        }

        phdr = (Elf64_Phdr *)((UINT8 *)phdr + header.e_phentsize);
    }
    Print(L"[DEBUG] Analysis Done. MinStart=0x%lx\n", min_start);

    if (min_start == (EFI_PHYSICAL_ADDRESS)(~0ULL) || max_end <= min_start)
    {
        Print(L"[-] Error: No valid PT_LOAD segments found in Kernel ELF (above 1MB)\n");
        uefi_call_wrapper(BS->FreePool, 1, ph_buffer);
        return NULL;
    }

    UINTN total_pages = (UINTN)((max_end - min_start) / 0x1000);

    EFI_PHYSICAL_ADDRESS alloc_base = min_start;
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiLoaderData, total_pages, &alloc_base);

    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Failed to allocate kernel range at 0x%lx (pages=%lu) (Status: %r)\n",
              alloc_base, total_pages, status);
        uefi_call_wrapper(BS->FreePool, 1, ph_buffer);
        return NULL;
    }

    uefi_call_wrapper(BS->SetMem, 3, (void *)min_start, (max_end - min_start), 0);

    phdr = (Elf64_Phdr *)ph_buffer;

    for (int i = 0; i < header.e_phnum; i++)
    {

        if (phdr->p_type == PT_LOAD && phdr->p_filesz > 0 && phdr->p_paddr >= 0x100000)
        {
            uefi_call_wrapper(kernel_file->SetPosition, 2, kernel_file, phdr->p_offset);

            UINTN to_read = (UINTN)phdr->p_filesz;
            status = uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &to_read, (void *)phdr->p_paddr);

            if (EFI_ERROR(status) || to_read != (UINTN)phdr->p_filesz)
            {
                Print(L"[-] Error: Failed to read segment to 0x%lx (Status: %r)\n",
                      (EFI_PHYSICAL_ADDRESS)phdr->p_paddr, status);
                uefi_call_wrapper(BS->FreePool, 1, ph_buffer);
                return NULL;
            }
        }

        phdr = (Elf64_Phdr *)((UINT8 *)phdr + header.e_phentsize);
    }

    uefi_call_wrapper(BS->FreePool, 1, ph_buffer);

    return (void *)header.e_entry;
}