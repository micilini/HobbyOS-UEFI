#ifndef CMD_SYNCTEST_H
#define CMD_SYNCTEST_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    SYNCTEST_ASYNC_NONE = 0,
    SYNCTEST_ASYNC_SEM,
    SYNCTEST_ASYNC_BOUNDARY,
    SYNCTEST_ASYNC_BOUNDARY_NOISE,
    SYNCTEST_ASYNC_SLEEP,
    SYNCTEST_ASYNC_CANCEL,
    SYNCTEST_ASYNC_TIMER_CANCEL,
    SYNCTEST_ASYNC_RACE
} synctest_async_kind_t;

typedef enum
{
    SYNCTEST_ASYNC_IDLE = 0,
    SYNCTEST_ASYNC_RUNNING,
    SYNCTEST_ASYNC_PASS,
    SYNCTEST_ASYNC_FAIL
} synctest_async_state_t;

typedef struct
{
    uint64_t run;
    synctest_async_kind_t kind;
    synctest_async_state_t state;
    uint64_t requested;
    uint64_t completed;
    uint64_t errors;
    uint64_t detail_a;
    uint64_t detail_b;
} synctest_async_snapshot_t;

void synctest_async_snapshot(synctest_async_snapshot_t *out);
bool synctest_async_snapshot_for_run(uint64_t run,
                                     synctest_async_snapshot_t *out);
const char *synctest_async_kind_string(synctest_async_kind_t kind);
const char *synctest_async_state_string(synctest_async_state_t state);
bool synctest_async_completion_contract_valid(void);
int cmd_synctest(int argc, char **argv);
#endif
