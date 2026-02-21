#include "../shared/protocol.h"
#include "src/core/kernel_init.h"
#include "src/shell/shell.h"
#include "src/graphics/terminal.h"
#include "src/graphics/graphics.h"
#include "src/graphics/splash.h"
#include "src/core/dpc.h"
#include "src/drivers/keyboard.h"
#include "src/drivers/timer.h"
#include "src/core/timers.h"
#include "src/drivers/serial.h"

#include "src/core/interrupts.h"
#include "src/core/scheduler.h"

#include <stdint.h>
#include <stddef.h>

#define KERNEL_STACK_SIZE (1024 * 1024)

__attribute__((aligned(16))) static unsigned char g_kernel_stack[KERNEL_STACK_SIZE];

static void serial_debug_char(char c)
{
    serial_putc_all(c);
}

static void debug_serial(const char *str)
{
    serial_write_all(str);
}

void kernel_main_high(BootInfo *boot_info);

void kernel_main_high(BootInfo *boot_info)
{

    serial_init_from_bootinfo(NULL);
    serial_write_all("\n[KERNEL] ENTRY OK\n");

    serial_write_all("[KERNEL] sizeof(BootInfo)=0x");
    serial_write_hex64_all((uint64_t)sizeof(BootInfo));
    serial_write_all("\n");

    serial_write_all("[KERNEL] offsetof(serial)=0x");
    serial_write_hex64_all((uint64_t)offsetof(BootInfo, serial));
    serial_write_all("\n");

    serial_write_all("[KERNEL] offsetof(serial.count)=0x");
    serial_write_hex64_all((uint64_t)offsetof(BootInfo, serial) + (uint64_t)offsetof(typeof(boot_info->serial), count));
    serial_write_all("\n");

    serial_write_all("[KERNEL] boot_info ptr=0x");
    serial_write_hex64_all((uint64_t)(uintptr_t)boot_info);
    serial_write_all("\n");

    if ((uintptr_t)boot_info < 0x1000)
    {
        serial_write_all("[KERNEL] boot_info INVALID (<0x1000). Nao da pra ler portas do BootInfo.\n");
    }
    else
    {

        serial_write_all("[KERNEL] boot_info->serial.count=");
        serial_write_hex64_all((uint64_t)boot_info->serial.count);
        serial_write_all("\n");

        uint32_t n = boot_info->serial.count;
        if (n > 32)
        {
            serial_write_all("[KERNEL] WARNING: count > 32 (possivel mismatch de struct). Vou limitar em 32.\n");
            n = 32;
        }

        for (uint32_t i = 0; i < n; i++)
        {
            serial_write_all("[KERNEL] port[");
            serial_write_hex64_all((uint64_t)i);
            serial_write_all("] io_base=0x");
            serial_write_hex64_all((uint64_t)boot_info->serial.ports[i].io_base);
            serial_write_all(" kind=0x");
            serial_write_hex64_all((uint64_t)boot_info->serial.ports[i].kind);
            serial_write_all("\n");
        }

        serial_write_all("[KERNEL] calling serial_init_from_bootinfo(boot_info)...\n");
        serial_init_from_bootinfo(boot_info);
        serial_write_all("[KERNEL] serial_init_from_bootinfo done.\n");
    }

    if ((uint64_t)boot_info < 0x1000)
    {

        serial_init_from_bootinfo(NULL);
        debug_serial("[KERNEL] CRITICAL: boot_info pointer is NULL or Garbage!\n");
        for (;;)
            ;
    }

    serial_init_from_bootinfo(boot_info);
    debug_serial("[KERNEL] Serial init OK (ports from BootInfo + fallback)\n");

    debug_serial("\n\n[KERNEL] Alive on Higher Half (0xFFFFFFFF82...)\n");

    debug_serial("[KERNEL] boot_info address: 0x");
    serial_write_hex64_all((uint64_t)boot_info);
    debug_serial("\n");

    debug_serial("[KERNEL] boot_info->serial.count = 0x");
    serial_write_hex64_all((uint64_t)boot_info->serial.count);
    debug_serial("\n");

    debug_serial("[KERNEL] Initializing Core (Paging, GDT, IDT)...\n");
    init_system_core(boot_info);
    debug_serial("[KERNEL] Core Initialized!\n");

    debug_serial("[KERNEL] Initializing Graphics...\n");
    init_graphics(boot_info->framebuffer, NULL);
    debug_serial("[KERNEL] Graphics Initialized.\n");

    debug_serial("[KERNEL] Drawing Splash...\n");

    play_splash_screen(boot_info);

    debug_serial("[KERNEL] Starting Terminal & Shell...\n");
    init_terminal();
    shell_init();

    debug_serial("[KERNEL] Entering Main Loop.\n");

    for (;;)
    {
        __asm__ volatile("sti; hlt");

        if (interrupts_consume_reschedule())
        {
            schedule_voluntary();
        }
    }
}
