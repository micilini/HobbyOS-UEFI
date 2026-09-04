#ifndef CMD_SCHEDTEST_H
#define CMD_SCHEDTEST_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    SCHEDTEST_ASYNC_NONE = 0,
    SCHEDTEST_ASYNC_YIELD,
    SCHEDTEST_ASYNC_CPU_PIN,
    SCHEDTEST_ASYNC_ENTRY_WINDOW
} schedtest_async_kind_t;

typedef enum
{
    SCHEDTEST_ASYNC_IDLE = 0,
    SCHEDTEST_ASYNC_RUNNING,
    SCHEDTEST_ASYNC_PASS,
    SCHEDTEST_ASYNC_FAIL
} schedtest_async_state_t;

typedef struct
{
    uint64_t run;
    schedtest_async_kind_t kind;
    schedtest_async_state_t state;
    uint32_t workers;
    uint32_t iterations;
    uint32_t remaining;
    uint64_t migrations;
    uint64_t mismatches;
    uint64_t scheduler_violations;
} schedtest_async_snapshot_t;

void schedtest_async_snapshot(schedtest_async_snapshot_t *out);
bool schedtest_async_snapshot_for_run(uint64_t run,
                                      schedtest_async_snapshot_t *out);
const char *schedtest_async_kind_string(schedtest_async_kind_t kind);
const char *schedtest_async_state_string(schedtest_async_state_t state);
bool schedtest_async_completion_contract_valid(void);

int cmd_schedtest(int argc, char **argv);

#endif
