#include "cmd_help.h"

#include "registry.h"

#include "../../graphics/console.h"

static void print_command_line(const ShellCommand *cmd)
{

    console_write("  ");
    console_write(cmd->name);

    if (cmd->aliases)
    {
        for (const char *const *a = cmd->aliases; *a; a++)
        {
            console_write(" | ");
            console_write(*a);
        }
    }

    console_write("  - ");
    if (cmd->desc)
        console_write(cmd->desc);
    console_write("\n");
}

static void print_command_details(const ShellCommand *cmd)
{
    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("Commmand: ");
    console_write(cmd->name);
    console_write("\n");

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    if (cmd->desc)
    {
        console_write("Descriptions: ");
        console_write(cmd->desc);
        console_write("\n");
    }

    if (cmd->usage)
    {
        console_write("Usage: ");
        console_write(cmd->usage);
        console_write("\n");
    }

    if (cmd->aliases)
    {
        console_write("Aliases: ");
        bool first = true;
        for (const char *const *a = cmd->aliases; *a; a++)
        {
            if (!first)
                console_write(", ");
            console_write(*a);
            first = false;
        }
        console_write("\n");
    }
}

int cmd_help(int argc, char **argv)
{

    if (argc >= 2 && argv[1] && argv[1][0])
    {
        const ShellCommand *cmd = shell_registry_find(argv[1]);
        if (!cmd)
        {
            console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_HOBBYOS_BLUE);
            console_write("Command '");
            console_write(argv[1]);
            console_write("' not found.\n");
            return 1;
        }

        print_command_details(cmd);
        return 0;
    }

    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("Available Commands:\n");

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    uint32_t count = 0;
    const ShellCommand *cmds = shell_registry_get_all(&count);

    for (uint32_t i = 0; i < count; i++)
    {
        print_command_line(&cmds[i]);
    }

    return 0;
}