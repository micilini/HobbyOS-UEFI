#include "../shared/protocol.h"
#include "src/core/kernel_init.h"
#include "src/shell/shell.h"
#include "src/graphics/terminal.h"
#include "src/graphics/graphics.h"
#include "src/graphics/splash.h"
#include "src/core/dpc.h"
#include "src/drivers/keyboard.h"
#include "src/drivers/timer.h"
#include "src/drivers/pci.h"
#include "src/core/timers.h"
#include "src/drivers/serial.h"

#include "src/core/interrupts.h"
#include "src/core/scheduler.h"
#include "src/core/runtime_ready.h"
#include "src/core/irq_bootstrap.h"
#include "src/core/selftest.h"
#include "src/core/panic.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define KERNEL_STACK_SIZE (1024 * 1024)

__attribute__((aligned(16), used)) static unsigned char g_kernel_stack[KERNEL_STACK_SIZE];

static void debug_serial(const char *str)
{
    serial_write_all(str);
}

static char *boot_progress_append_text(char *out, const char *text)
{
    while (*text)
        *out++ = *text++;
    return out;
}

static char *boot_progress_append_u64(char *out, uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        *out++ = reverse[--count];
    return out;
}

static void boot_progress(const char *stage)
{
    timer_clockevent_snapshot_t event = {0};
    (void)timer_clockevent_snapshot(&event);
    char line[176];
    char *p = boot_progress_append_text(line, "[BOOT][PROGRESS] stage=");
    p = boot_progress_append_text(p, stage);
    p = boot_progress_append_text(p, " monotonic_ms=");
    p = boot_progress_append_u64(p, timer_get_uptime_ms());
    p = boot_progress_append_text(p, " clockevent_ticks=");
    p = boot_progress_append_u64(p, event.total_ticks);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
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
    boot_progress("GRAPHICS_INITIALIZED");

    splash_result_t splash_result = play_splash_screen(boot_info);
    (void)splash_result;

    boot_progress("TERMINAL_TRANSITION");
    debug_serial("[KERNEL] Starting Terminal & Shell...\n");
    init_terminal();
    shell_init();
    if (!irq_bootstrap_release_services() || !pci_msi_enable_prepared())
        kpanic("External interrupt service release failed");

    shell_runtime_snapshot_t shell_runtime = {0};
    uint64_t shell_deadline = timer_get_uptime_ms() + 10000u;
    do {
        (void)shell_runtime_snapshot(&shell_runtime);
        if (shell_runtime.initialized && shell_runtime.thread_started)
            break;
        timer_sleep(10);
    } while (timer_get_uptime_ms() <= shell_deadline);

    if (shell_runtime.initialized && shell_runtime.thread_started) {
        debug_serial("\n[BOOT][SHELL_READY] PASS\n");
        boot_progress("SHELL_READY");
    } else
        debug_serial("\n[BOOT][SHELL_READY] FAIL\n");

    bool runtime_ok = shell_runtime.initialized &&
        shell_runtime.thread_started &&
        runtime_ready_wait_and_publish(10000, 1000);
    bool autorun_enabled = false;
#if defined(HOBBYOS_SELFTEST) && defined(HOBBYOS_SELFTEST_AUTORUN)
    autorun_enabled = true;
#endif
    bool autorun_ok = true;
    if (runtime_ok) {
        runtime_ready_mark_testing(autorun_enabled);
#if defined(HOBBYOS_SELFTEST) && defined(HOBBYOS_SELFTEST_AUTORUN)
        autorun_ok = selftest_autorun_if_enabled();
#endif
    } else {
        debug_serial("\n[BOOT][RUNTIME_READY] FAIL\n");
    }
    runtime_ready_mark_test_complete(autorun_enabled,
                                     runtime_ok && autorun_ok);

    if (runtime_ok && autorun_ok)
        boot_progress("RUNTIME_READY");
    boot_progress("MAIN_LOOP");
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
