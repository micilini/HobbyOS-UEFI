#include "smp_topology.h"
#include "../acpi/madt.h"
#include "../drivers/serial.h" 
#include "../apic/lapic.h"


CpuInfo g_cpus[MAX_CPUS];
uint32_t g_cpu_count = 0;
uint8_t g_bsp_apic_id = 0;


static void serial_print_dec(uint32_t n)
{
    if (n == 0)
    {
        serial_write_all("0");
        return;
    }

    char buffer[32];
    int i = 0;
    while (n > 0)
    {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }

    for (int j = i - 1; j >= 0; j--)
    {
        serial_putc_all(buffer[j]);
    }
}

void smp_topology_init()
{
    
    g_bsp_apic_id = (uint8_t)lapic_get_id(); 

    serial_write_all("[SMP] Topology: BSP APIC ID detected = ");
    serial_print_dec(g_bsp_apic_id);
    serial_write_all("\n");

    
    uint32_t madt_count = madt_get_cpu_count();
    
    if (madt_count > MAX_CPUS) {
        serial_write_all("[SMP] Warning: More CPUs detected than supported. Limiting the ");
        serial_print_dec(MAX_CPUS);
        serial_write_all("\n");
        madt_count = MAX_CPUS;
    }

    
    uint8_t apic_ids[MAX_CPUS];
    uint32_t found = madt_get_cpu_apic_ids(apic_ids, MAX_CPUS);

    
    g_cpu_count = found;
    
    serial_write_all("[SMP] Checking CPUs...\n");
    for (uint32_t i = 0; i < g_cpu_count; i++)
    {
        g_cpus[i].apic_id = apic_ids[i];
        g_cpus[i].is_online = false;
        
        
        if (g_cpus[i].apic_id == g_bsp_apic_id) {
            g_cpus[i].is_bsp = true;
            g_cpus[i].is_online = true; 
        } else {
            g_cpus[i].is_bsp = false;
        }

        serial_write_all("      - CPU Index ");
        serial_print_dec(i);
        serial_write_all(" | APIC ID: ");
        serial_print_dec(g_cpus[i].apic_id);
        serial_write_all(" | Tipo: ");
        
        if (g_cpus[i].is_bsp) {
            serial_write_all("BSP (Online)\n");
        } else {
            serial_write_all("AP (Offline)\n");
        }
    }

    serial_write_all("[SMP] Total of CPUs found: ");
    serial_print_dec(g_cpu_count);
    serial_write_all("\n");
}