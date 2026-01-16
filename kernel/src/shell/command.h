#ifndef SHELL_COMMAND_H
#define SHELL_COMMAND_H

#include <stdint.h>

typedef int (*shell_command_handler_t)(int argc, char **argv);

typedef struct ShellCommand
{
    const char *name;
    const char *const *aliases;
    const char *desc;
    const char *usage;
    shell_command_handler_t handler;
} ShellCommand;

#endif