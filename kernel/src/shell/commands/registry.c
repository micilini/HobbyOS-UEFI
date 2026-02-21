#include "registry.h"

#include "cmd_help.h"
#include "cmd_clear.h"
#include "cmd_version.h"
#include "cmd_mem.h"
#include "cmd_panic.h"
#include "cmd_power.h"
#include "cmd_cpu.h"
#include "cmd_pci.h"
#include "cmd_acpi.h"
#include "cmd_irq.h"
#include "cmd_echo.h"
#include "cmd_usbdiag.h"
#include "cmd_smpstress.h"

#include "../../libc/string.h"

static const char *g_help_aliases[] = {"hlp", NULL};
static const char *g_clear_aliases[] = {"clean", "cls", NULL};
static const char *g_version_aliases[] = {"ver", NULL};

static const char *g_mem_aliases[] = {"memory", NULL};
static const char *g_panic_aliases[] = {"pnc", NULL};

static const char *g_shutdown_aliases[] = {"halt", NULL};
static const char *g_restart_aliases[] = {"reboot", "reset", NULL};

static const char *g_aliases_cpu[] = {"cpu", 0};
static const char *g_aliases_pci[] = {"pci", 0};
static const char *g_aliases_acpi[] = {"acpi", 0};
static const char *g_aliases_irq[] = {"irq", "int", 0};
static const char *g_aliases_echo[] = {"eco", 0};
static const char *g_aliases_usbdiag[] = {"usb", "xhci", 0};

static const char *g_aliases_smpstress[] = {"stress", "smp", 0};

static const ShellCommand g_commands[] = {
    {.name = "help",
     .aliases = g_help_aliases,
     .desc = "show help commands",
     .usage = "help [command]",
     .handler = cmd_help},
    {.name = "clear",
     .aliases = g_clear_aliases,
     .desc = "clean screen",
     .usage = "clear",
     .handler = cmd_clear},
    {.name = "version",
     .aliases = g_version_aliases,
     .desc = "show OS version",
     .usage = "version",
     .handler = cmd_version},
    {.name = "mem",
     .aliases = g_mem_aliases,
     .desc = "memory statistics (PMM/Heap)",
     .usage = "mem",
     .handler = cmd_mem},
    {.name = "panic",
     .aliases = g_panic_aliases,
     .desc = "try screen kernel panic",
     .usage = "panic [message]",
     .handler = cmd_panic},
    {.name = "shutdown",
     .aliases = g_shutdown_aliases,
     .desc = "Turn off the machine (ACPI if possible; fallback halt)",
     .usage = "shutdown",
     .handler = cmd_shutdown},
    {.name = "restart",
     .aliases = g_restart_aliases,
     .desc = "restart the machine (ACPI/CF9/8042; fallback triple fault)",
     .usage = "restart",
     .handler = cmd_restart},
    {.name = "cpu",
     .aliases = g_aliases_cpu,
     .desc = "CPU vendor/model/features (CPUID)",
     .usage = "cpu",
     .handler = cmd_cpu},
    {.name = "pci",
     .aliases = g_aliases_pci,
     .desc = "List devices PCI (safe scan)",
     .usage = "pci",
     .handler = cmd_pci},
    {.name = "acpi",
     .aliases = g_aliases_acpi,
     .desc = "List detected tables ACPI",
     .usage = "acpi",
     .handler = cmd_acpi},
    {.name = "irq",
     .aliases = g_aliases_irq,
     .desc = "Simple stats of IRQs",
     .usage = "irq [reset]",
     .handler = cmd_irq},
    {.name = "echo",
     .aliases = g_aliases_echo,
     .desc = "prints text to the console",
     .usage = "echo [-n] \"text\"",
     .handler = cmd_echo},
    {.name = "usbdiag",
     .aliases = g_aliases_usbdiag,
     .desc = "USB/xHCI latency diagnostics",
     .usage = "usbdiag",
     .handler = cmd_usbdiag},
    {.name = "smpstress",
     .aliases = g_aliases_smpstress,
     .desc = "SMP load test + optional div0 (#DE) panic trigger (logs on serial)",
     .usage = "smpstress [workers] [panic_pct] [period_ms]",
     .handler = cmd_smpstress},
};

static bool shell_command_matches(const ShellCommand *cmd, const char *token)
{
    if (!cmd || !token)
        return false;

    if (strcmp(token, cmd->name) == 0)
    {
        return true;
    }

    if (cmd->aliases)
    {
        for (const char *const *a = cmd->aliases; *a; a++)
        {
            if (strcmp(token, *a) == 0)
            {
                return true;
            }
        }
    }

    return false;
}

const ShellCommand *shell_registry_find(const char *name_or_alias)
{
    if (!name_or_alias)
        return NULL;

    const uint32_t count = (uint32_t)(sizeof(g_commands) / sizeof(g_commands[0]));
    for (uint32_t i = 0; i < count; i++)
    {
        if (shell_command_matches(&g_commands[i], name_or_alias))
        {
            return &g_commands[i];
        }
    }
    return NULL;
}

const ShellCommand *shell_registry_get_all(uint32_t *out_count)
{
    if (out_count)
    {
        *out_count = (uint32_t)(sizeof(g_commands) / sizeof(g_commands[0]));
    }
    return g_commands;
}