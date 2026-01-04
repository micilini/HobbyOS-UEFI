#include "acpi.h"
#include <stddef.h>

static AcpiRsdp* g_rsdp = NULL;
static AcpiSdtHeader* g_xsdt = NULL;

void init_acpi(void* rsdp_address) {
    g_rsdp = (AcpiRsdp*)rsdp_address;
    
    
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address != 0) {
        g_xsdt = (AcpiSdtHeader*)((uint64_t)g_rsdp->xsdt_address);
    } 
    
    
}

void* acpi_find_table(const char* signature) {
    if (!g_xsdt) return NULL;

    int entries = (g_xsdt->length - sizeof(AcpiSdtHeader)) / 8;
    uint64_t* pointers = (uint64_t*)((char*)g_xsdt + sizeof(AcpiSdtHeader));

    for (int i = 0; i < entries; i++) {
        AcpiSdtHeader* header = (AcpiSdtHeader*)pointers[i];
        if (header->signature[0] == signature[0] &&
            header->signature[1] == signature[1] &&
            header->signature[2] == signature[2] &&
            header->signature[3] == signature[3]) {
            return (void*)header;
        }
    }
    return NULL;
}