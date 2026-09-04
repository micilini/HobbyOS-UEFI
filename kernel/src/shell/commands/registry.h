#ifndef SHELL_COMMAND_REGISTRY_H
#define SHELL_COMMAND_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#include "../command.h"

const ShellCommand *shell_registry_find(const char *name_or_alias);

const ShellCommand *shell_registry_get_all(uint32_t *out_count);
bool shell_registry_validate(uint64_t *out_violations);

#endif
