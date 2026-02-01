
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

#define KERNEL_STACK_SIZE (1024 * 1024)

__attribute__((aligned(16))) static unsigned char g_kernel_stack[KERNEL_STACK_SIZE];

static void kernel_main(BootInfo *boot_info);

__attribute__((naked, noreturn)) void _start(BootInfo *boot_info)
{
    (void)boot_info;

    __asm__ volatile(
        "cli\n"
        "lea g_kernel_stack(%%rip), %%rax\n"
        "addq %[stack_size], %%rax\n"
        "andq $-16, %%rax\n"
        "movq %%rax, %%rsp\n"
        "xorq %%rbp, %%rbp\n"
        "call kernel_main\n"
        "1: hlt\n"
        "jmp 1b\n"
        :
        : [stack_size] "i"(KERNEL_STACK_SIZE)
        : "rax", "memory");
}

static void kernel_main(BootInfo *boot_info)
{

    init_graphics(boot_info->framebuffer, NULL);

    init_system_core(boot_info);

    play_splash_screen(boot_info);

    init_terminal();

    shell_init();

    for (;;)
    {
        timers_poll();
        timer_run_deferred();
        dpc_run();

        __asm__ volatile("hlt");
    }
}