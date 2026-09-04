#ifndef DIAGNOSTIC_RESULT_H
#define DIAGNOSTIC_RESULT_H

#include <stdbool.h>
#include <stdint.h>

#define DIAGNOSTIC_RESULT_MAX 128u

typedef enum
{
    DIAGNOSTIC_RESULT_RUNNING = 0,
    DIAGNOSTIC_RESULT_PASS,
    DIAGNOSTIC_RESULT_FAIL
} diagnostic_result_state_t;

int diagnostic_result_complete(const char *canonical_command,
                               int return_code);
uint64_t diagnostic_result_async_started(const char *canonical_command);
void diagnostic_result_async_completed(const char *canonical_command,
                                       uint64_t generation,
                                       bool passed);

#ifdef HOBBYOS_SELFTEST
int cmd_diagnosticresulttest(int argc, char **argv);
#endif

#endif
