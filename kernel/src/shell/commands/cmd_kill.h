#ifndef CMD_KILL_H
#define CMD_KILL_H

typedef enum
{
    KILL_CLI_ACCEPTED = 0,

    KILL_CLI_ALREADY_PENDING = 2,
    KILL_CLI_ALREADY_ZOMBIE = 3,
    KILL_CLI_ALREADY_EXITING = 4,

    KILL_CLI_USAGE = 64,
    KILL_CLI_INVALID_PID = 65,
    KILL_CLI_PROTECTED = 66,
    KILL_CLI_NOT_KILLABLE = 67,
    KILL_CLI_NOT_FOUND = 68,
    KILL_CLI_INTERNAL_ERROR = 69
} kill_cli_status_t;

int cmd_kill(int argc, char **argv);

#endif
