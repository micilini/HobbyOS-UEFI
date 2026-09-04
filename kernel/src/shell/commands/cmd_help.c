#include "cmd_help.h"

#include "registry.h"

#include "../../graphics/console.h"
#include "../../drivers/serial.h"

static void help_write(const char *text)
{
    console_write(text);
    serial_write_all(text);
}

static void help_put_char(char value)
{
    console_put_char(value);
    serial_putc_all(value);
}

static void print_command_line(const ShellCommand *cmd)
{

    help_write("  ");
    help_write(cmd->name);

    if (cmd->aliases)
    {
        for (const char *const *a = cmd->aliases; *a; a++)
        {
            help_write(" | ");
            help_write(*a);
        }
    }

    help_write("  - ");
    if (cmd->desc)
        help_write(cmd->desc);
    help_write("\n");
}

static void print_command_details(const ShellCommand *cmd)
{
    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_HOBBYOS_BLUE);
    help_write("Command: ");
    help_write(cmd->name);
    help_write("\n");

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    if (cmd->desc)
    {
        help_write("Description: ");
        help_write(cmd->desc);
        help_write("\n");
    }

    if (cmd->usage)
    {
        help_write("Usage: ");
        help_write(cmd->usage);
        help_write("\n");
    }

    if (cmd->aliases)
    {
        help_write("Aliases: ");
        bool first = true;
        for (const char *const *a = cmd->aliases; *a; a++)
        {
            if (!first)
                help_write(", ");
            help_write(*a);
            first = false;
        }
        help_write("\n");
    }

    if (cmd->details && cmd->details[0])
    {
        help_write("Details:\n  ");
        for (const char *p = cmd->details; *p; p++)
        {
            help_put_char(*p);
            if (*p == '\n' && p[1])
                help_write("  ");
        }
        help_write("\n");
    }
}

int cmd_help(int argc, char **argv)
{

    if (argc > 2)
    {
        help_write("Usage: help [command]\n");
        return 1;
    }

    if (argc == 2 && argv[1] && argv[1][0])
    {
        const ShellCommand *cmd = shell_registry_find(argv[1]);
        if (!cmd)
        {
            console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_HOBBYOS_BLUE);
            help_write("Command '");
            help_write(argv[1]);
            help_write("' not found.\n");
            return 1;
        }

        print_command_details(cmd);
        return 0;
    }

    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_HOBBYOS_BLUE);
    help_write("Available Commands:\n");

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    uint32_t count = 0;
    const ShellCommand *cmds = shell_registry_get_all(&count);

    for (uint32_t i = 0; i < count; i++)
    {
        print_command_line(&cmds[i]);
    }

    return 0;
}
