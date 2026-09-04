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
#include "cmd_ps.h"
#include "cmd_kill.h"
#include "cmd_taskman.h"
#include "cmd_schedtest.h"
#include "cmd_synctest.h"
#include "cmd_accounttest.h"
#include "cmd_killtest.h"
#include "cmd_reaptest.h"
#include "cmd_inputtest.h"
#include "cmd_modaltest.h"
#include "cmd_taskmantest.h"
#include "cmd_taskdiag.h"
#include "cmd_tasktest.h"
#include "../diagnostic_result.h"

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

static const char *g_aliases_ps[] = {"tasks", "tasklist", 0};

static const char *g_aliases_kill[] = {"terminate", "taskkill", 0};

static const char *g_aliases_taskman[] = {"tm", "top", 0};
static const char *g_aliases_taskdiag[] = {"td", "tdiag", 0};
#ifdef HOBBYOS_SELFTEST
static const char *g_aliases_tasktest[] = {"tt", 0};
#endif
static const char *g_aliases_schedtest[] = {"st", 0};
static const char *g_aliases_synctest[] = {"sync-test", 0};
static const char *g_aliases_accounttest[] = {"accttest","metrictest",0};
static const char *g_aliases_killtest[] = {"kt",0};
static const char *g_aliases_reaptest[] = {"rt",0};
static const char *g_aliases_inputtest[] = {"it",0};
static const char *g_aliases_modaltest[] = {"mt",0};
static const char *g_aliases_taskmantest[] = {"tmt",0};

static int dispatch_accounttest(int argc, char **argv)
{
    int rc = cmd_accounttest(argc, argv);
    const char *sub = argc > 1 ? argv[1] : "check";
    return strcmp(sub, "check") == 0
        ? diagnostic_result_complete("accounttest check", rc) : rc;
}

static int dispatch_taskmantest(int argc, char **argv)
{
    int rc = cmd_taskmantest(argc, argv);
    return argc > 1 && strcmp(argv[1], "check") == 0
        ? diagnostic_result_complete("taskmantest check", rc) : rc;
}

static int dispatch_inputtest(int argc, char **argv)
{
    int rc = cmd_inputtest(argc, argv);
    const char *sub = argc > 1 ? argv[1] : "check";
    return strcmp(sub, "check") == 0
        ? diagnostic_result_complete("inputtest check", rc) : rc;
}

static int dispatch_modaltest(int argc, char **argv)
{
    int rc = cmd_modaltest(argc, argv);
    const char *sub = argc > 1 ? argv[1] : "check";
    return strcmp(sub, "check") == 0
        ? diagnostic_result_complete("modaltest check", rc) : rc;
}

static int dispatch_synctest(int argc, char **argv)
{
    int rc = cmd_synctest(argc, argv);
    const char *sub = argc > 1 ? argv[1] : "check";
    if (strcmp(sub, "check") == 0)
        return diagnostic_result_complete("synctest check", rc);
    if (strcmp(sub, "timer-order") == 0)
        return diagnostic_result_complete("synctest timer-order", rc);
    if (strcmp(sub, "timer-backlog") == 0)
        return diagnostic_result_complete("synctest timer-backlog", rc);
    return rc;
}

static int dispatch_reaptest(int argc, char **argv)
{
    int rc = cmd_reaptest(argc, argv);
    if (argc > 1 && strcmp(argv[1], "check") == 0)
        return diagnostic_result_complete("reaptest check", rc);
    if (argc > 1 && strcmp(argv[1], "timer-ref") == 0)
        return diagnostic_result_complete("reaptest timer-ref", rc);
    return rc;
}

static int dispatch_killtest(int argc, char **argv)
{
    int rc = cmd_killtest(argc, argv);
    const char *sub = argc > 1 ? argv[1] : "check";
    if (strcmp(sub, "check") == 0)
        return diagnostic_result_complete("killtest check", rc);
    if (strcmp(sub, "smpstress-sweep") == 0 ||
        strcmp(sub, "smpstresssweep") == 0)
        return diagnostic_result_complete("killtest smpstress-sweep", rc);
    return rc;
}

static int dispatch_schedtest(int argc, char **argv)
{
    int rc = cmd_schedtest(argc, argv);
    if (argc < 2 || strcmp(argv[1], "check") == 0)
        return diagnostic_result_complete("schedtest check", rc);
    return rc;
}

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
    {.name = "ps",
     .aliases = g_aliases_ps,
     .desc = "List a paginated snapshot of HobbyOS kernel tasks",
     .usage = "ps [page] [page_size]",
     .details = "Lists HobbyOS kernel tasks, not POSIX processes.\nUses a copy-only scheduler snapshot.\nPages are 1-based.\nThe first CPU sample is --.\nLater CPU percentages use the interval since the previous ps invocation.\nUse TASKMAN for a live modal view.",
     .handler = cmd_ps},
    {.name = "kill",
     .aliases = g_aliases_kill,
     .desc = "Request cooperative cancellation of a kernel task by PID",
     .usage = "kill <pid>",
     .details = "Requests cooperative cancellation.\nIt does not forcibly interrupt arbitrary kernel code.\nThe task exits at a cancellation point or interruptible wait.\nProtected and non-killable tasks are rejected.\nA repeated accepted request becomes ALREADY_PENDING.\nZOMBIE and EXITING are reported distinctly.",
     .handler = cmd_kill},
    {.name = "taskman",
     .aliases = g_aliases_taskman,
     .desc = "Paginated live task monitor",
     .usage = "taskman [refresh_ms]",
     .details = "Live paginated view of kernel tasks.\nRefresh range: 50..2000 ms.\nESC is the only exit key.\nArrows select; PageUp/PageDown/Home/End navigate.\nUSER=Root and AFF=Any are V1 presentation constants.",
     .handler = cmd_taskman},
    {.name = "taskdiag",
     .aliases = g_aliases_taskdiag,
     .desc = "Read-only kernel task diagnostics",
     .usage = "taskdiag [summary|task <pid>|scheduler|reaper|modal|input|accounting|all|check|selftest|trace <pid>|trace off|trace-status]",
     .details = "Shows copy-only task and subsystem diagnostics.\nDiagnostics are read-only and tracing is opt-in.\nUse taskdiag trace <pid> to compare PS and TASKMAN rows.\nUse taskdiag trace off when comparison is complete.",
     .handler = cmd_taskdiag},
#ifdef HOBBYOS_SELFTEST
    {.name = "tasktest",
     .aliases = g_aliases_tasktest,
     .desc = "Run bounded TASKMAN V1 kernel selftests",
     .usage = "tasktest [list|summary|readiness-status|transport-status|suite|all]",
     .details = "Runs fast, bounded selftests registered by subsystem.\nreadiness-status reports the TEST_READY gate; framed exec is rejected before that gate.\nStress, fault injection and soak remain host-harness stages.\nResults use machine-readable PASS, FAIL, SKIP and SUMMARY records.",
     .handler = cmd_tasktest},
    {.name = "diagnosticresulttest",
     .desc = "Validate bounded framebuffer diagnostic result delivery",
     .usage = "diagnosticresulttest all",
     .details = "Exercises synchronous PASS/FAIL and asynchronous status delivery.\nSynthetic FAIL output is expected and does not indicate a guest fault.",
     .handler = cmd_diagnosticresulttest},
#endif
    {.name = "schedtest",
     .aliases = g_aliases_schedtest,
     .desc = "Scheduler handoff diagnostics and finite yield stress",
     .usage = "schedtest check|stats|cpu-pin <iterations>|entry-window <iterations>|yield <workers> <iterations>|reap0",
     .handler = dispatch_schedtest},
    {.name = "synctest", .aliases = g_aliases_synctest,
     .desc = "Atomic wait, semaphore and timer diagnostics",
     .usage = "synctest check|sem N [W S]|boundary N|sleep|cancel|race N|stale|timercancel|fallback|oom|stats",
     .handler = dispatch_synctest},
    {.name="killtest",.aliases=g_aliases_killtest,.desc="Cooperative cancellation diagnostics",.usage="killtest check|ready|running|sleeping|blocked|duplicate|protected|nonkillable|rename|cleanup|normal|zombie|timeout-race|parser|ui-setup|ui-status|ui-cleanup|stats|all",.handler=dispatch_killtest},
    {.name="reaptest",.aliases=g_aliases_reaptest,.desc="Reference-safe zombie reaper diagnostics",.usage="reaptest check|stats|normal N|killed N|grace|batch|snapshot|cleanup|timer-ref|notification|oncpu|stale|heap|churn|all",.handler=dispatch_reaptest},
    {.name="inputtest",.aliases=g_aliases_inputtest,.desc="Atomic input queue and routing diagnostics",.usage="inputtest check|stats|trace|queue|try-wait|drain|full|mixed|producers|begin-boundary|end-boundary|end-rollback|shell-pause-boundary|transitions|keyboard|leakage|marker|all",.handler=dispatch_inputtest},
    {.name="modaltest",.aliases=g_aliases_modaltest,.desc="Owned modal session and UI worker diagnostics",.usage="modaltest check|stats|open-close N|second-session|false-token|stale-token|wrong-owner|input-token|shell-blocked|kill-owner|owner-exit|arm-kill-next-ui|alloc-fail|create-fail|router-begin-fail|router-end-fail|completion-once|all",.handler=dispatch_modaltest},
    {.name="taskmantest",.aliases=g_aliases_taskmantest,.desc="Paginated TASKMAN layout/model/navigation diagnostics",.usage="taskmantest check|stats|layout|model|sort|pagination|refresh|input|formatter|formatting-max|heap-begin|heap-end|stress [nav churn zombie kill layout]|geometry C R|cap N|fail-next-allocation|setup N|cleanup|churn N|zombie|long-name|all",.handler=dispatch_taskmantest},
    {.name="accounttest",.aliases=g_aliases_accounttest,
     .desc="Monotonic clock, LAPIC calibration and runtime accounting diagnostics",
     .usage="accounttest check|core|clock|clock-smp [workers reads]|reaper-quiescence [workers]|lapic [ms]|sampler|format|long|lifecycle|ui-telemetry on|off|idle|busy|sleep|share|allcpu|migrate|stats",
     .handler=dispatch_accounttest},
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

bool shell_registry_validate(uint64_t *out_violations)
{
    const uint32_t count = (uint32_t)(sizeof(g_commands) /
                                      sizeof(g_commands[0]));
    uint64_t violations = 0;
    for (uint32_t index = 0; index < count; index++)
    {
        const ShellCommand *command = &g_commands[index];
        if (!command->name || !command->name[0])
            violations++;
        if (!command->handler)
            violations++;
        if (!command->usage || !command->usage[0])
            violations++;
        for (uint32_t other = index + 1u; other < count; other++)
            if (command->name && g_commands[other].name &&
                !strcmp(command->name, g_commands[other].name))
                violations++;

        if (!command->aliases)
            continue;
        for (const char *const *alias = command->aliases; *alias; alias++)
        {
            if (!(*alias)[0])
            {
                violations++;
                continue;
            }
            for (uint32_t other = 0; other < count; other++)
            {
                if (other == index)
                    continue;
                const ShellCommand *candidate = &g_commands[other];
                if (candidate->name && !strcmp(*alias, candidate->name))
                    violations++;
                if (!candidate->aliases)
                    continue;
                for (const char *const *other_alias = candidate->aliases;
                     *other_alias; other_alias++)
                    if (!strcmp(*alias, *other_alias))
                        violations++;
            }
        }
    }
    if (out_violations)
        *out_violations = violations;
    return violations == 0;
}
