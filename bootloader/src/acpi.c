#include "acpi.h"
#include "utils.h" 



static EFI_GUID Acpi1_Guid = ACPI_TABLE_GUID;      
static EFI_GUID Acpi2_Guid = ACPI_20_TABLE_GUID;   

void* find_acpi_rsdp() {
    EFI_CONFIGURATION_TABLE *configTable = SystemTable->ConfigurationTable;
    void* rsdp = NULL;

    Print(L"[*] ACPI: Searching for RSDP...\n");

    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
        
        
        if (memcmp(&configTable[i].VendorGuid, &Acpi2_Guid, sizeof(EFI_GUID)) == 0) {
            Print(L"[*] ACPI: Found ACPI 2.0 Table!\n");
            return configTable[i].VendorTable;
        }

        
        if (memcmp(&configTable[i].VendorGuid, &Acpi1_Guid, sizeof(EFI_GUID)) == 0) {
            rsdp = configTable[i].VendorTable;
        }
    }

    if (rsdp != NULL) {
        Print(L"[*] ACPI: Found ACPI 1.0 Table (Fallback).\n");
    } else {
        Print(L"[-] ACPI: Critical - RSDP Not found.\n");
    }

    return rsdp;
}