#include "smp_boot.h"
#include "smp_topology.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../memory/gdt.h"
#include "../core/idt.h"
#include "../memory/paging.h"
// Removemos drivers/timer.h para não depender de IRQs
// #include "../drivers/timer.h" 

extern uint8_t _trampoline_start[];
extern uint8_t _trampoline_end[];

extern uint64_t smp_trampoline_cr3;
extern uint64_t smp_trampoline_stack;
extern uint64_t smp_trampoline_entry_ptr;
extern uint64_t smp_cpu_id_mailbox;

volatile bool g_ap_boot_done = false;

// --- Helpers Locais de Impressão ---

static void smp_print_dec(uint32_t n)
{
    if (n == 0) { serial_write_all("0"); return; }
    char buffer[32];
    int i = 0;
    while (n > 0) { buffer[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i - 1; j >= 0; j--) { serial_putc_all(buffer[j]); }
}

static void smp_print_hex(uint64_t n)
{
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4)
    {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
}

// --- NOVO: Delay Spin (Busy Wait) ---
// Evita usar timer_sleep que depende de interrupções
static void delay_spin(uint64_t count)
{
    // Ajuste empírico: 100000 loops ~= "algo rápido mas perceptível"
    // O objetivo não é precisão, é garantir que o hardware tenha tempo de processar o sinal
    for (volatile uint64_t i = 0; i < count * 100000; i++) {
        __asm__ volatile("pause");
    }
}

// Ponto de entrada C para os APs
void ap_kernel_entry()
{
    // Checkpoint C
    serial_write_all("C"); 

    // 1. Identificar quem somos
    uint32_t my_apic_id = lapic_get_id();
    CpuInfo *me = NULL;

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].apic_id == my_apic_id) {
            me = &g_cpus[i];
            break;
        }
    }

    if (me) {
        // 2. Carregar GDT e TSS exclusivos desta CPU (alocados na Fase 2)
        // Precisamos de uma GdtPtr local para o LGDT
        GdtPtr gdtr;
        gdtr.size = sizeof(GdtTable) - 1;
        gdtr.offset = (uint64_t)me->gdt_ptr;
        
        // Aplica a GDT desta CPU
        gdt_flush((uint64_t)&gdtr);

        // Carrega o TSS desta CPU (ltr 0x28)
        __asm__ volatile("mov $0x28, %ax; ltr %ax");
    }

    // 3. Handshake
    g_ap_boot_done = true;

    init_idt();
    init_lapic();

    serial_write_all("!"); // Sucesso total!

    while(1) {
        __asm__ volatile("sti; hlt");
    }
}

void smp_boot_aps()
{
    serial_write_all("[SMP] Boot: Preparing Trampoline at 0x8000...\n");

    // Agora os símbolos _trampoline_start/end estarão no endereço virtual (0xFFFFFFFF...)
    uint64_t trampoline_len = (uint64_t)_trampoline_end - (uint64_t)_trampoline_start;
    
    serial_write_all("      Source (Virt): "); 
    smp_print_hex((uint64_t)_trampoline_start);
    serial_write_all(" | Size: ");
    smp_print_dec((uint32_t)trampoline_len);
    serial_write_all(" bytes.\n");

    // 1. Copiar o código do trampoline para 0x8000
    // O seu Paging mapeia 0x8000 (físico) como 0x8000 (virtual), então a escrita é direta.
    memcpy((void*)TRAMPOLINE_ADDR, _trampoline_start, trampoline_len);

    // 2. Debug Check: Verificação vital
    volatile uint8_t *check = (volatile uint8_t*)TRAMPOLINE_ADDR;
    serial_write_all("      Check 0x8000 content: [");
    smp_print_hex(check[0]); 
    serial_write_all(", ");
    smp_print_hex(check[1]); 
    serial_write_all("]\n");

    if (check[0] == 0 && check[1] == 0) {
        serial_write_all("CRITICAL: Trampoline copy still results in zeroes! Check Linker Script.\n");
        return; 
    }

    // 3. Configurar Mailbox (Offsets agora são simples)
    uint64_t offset_cr3   = (uint64_t)&smp_trampoline_cr3 - (uint64_t)_trampoline_start;
    uint64_t offset_stack = (uint64_t)&smp_trampoline_stack - (uint64_t)_trampoline_start;
    uint64_t offset_entry = (uint64_t)&smp_trampoline_entry_ptr - (uint64_t)_trampoline_start;

    volatile uint64_t *ptr_cr3   = (volatile uint64_t*)(TRAMPOLINE_ADDR + offset_cr3);
    volatile uint64_t *ptr_stack = (volatile uint64_t*)(TRAMPOLINE_ADDR + offset_stack);
    volatile uint64_t *ptr_entry = (volatile uint64_t*)(TRAMPOLINE_ADDR + offset_entry);

    // 4. Configurar dados de boot
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));

    *ptr_cr3 = current_cr3;
    *ptr_entry = (uint64_t)&ap_kernel_entry;

    // 5. Sincronizar memória
    __asm__ volatile("wbinvd");

    serial_write_all("[SMP] Trampoline Ready. Waking up APs...\n");

    for (uint32_t i = 0; i < g_cpu_count; i++)
    {
        if (g_cpus[i].is_bsp) continue;

        serial_write_all("      -> Waking CPU Index ");
        smp_print_dec(i);
        serial_write_all(" (APIC ID ");
        smp_print_dec(g_cpus[i].apic_id);
        serial_write_all(")... ");

        *ptr_stack = g_cpus[i].stack_top;
        g_ap_boot_done = false;

        // Sequência Protocolar
        lapic_send_init(g_cpus[i].apic_id);
        delay_spin(10); 

        lapic_send_sipi(g_cpus[i].apic_id, (TRAMPOLINE_ADDR >> 12));
        delay_spin(2); 

        // Espera Handshake
        int timeout = 1000; 
        while (!g_ap_boot_done && timeout > 0) {
            delay_spin(1);
            timeout--;
        }

        if (g_ap_boot_done) {
            g_cpus[i].is_online = true;
            serial_write_all("ONLINE!\n");
        } else {
            // Tenta último SIPI de resgate
            lapic_send_sipi(g_cpus[i].apic_id, (TRAMPOLINE_ADDR >> 12));
            delay_spin(500);
            if (g_ap_boot_done) {
                g_cpus[i].is_online = true;
                serial_write_all("ONLINE (Retry)!\n");
            } else {
                serial_write_all("FAILED (No response)\n");
            }
        }
    }
    serial_write_all("[SMP] AP Boot Sequence Complete.\n");
}