#ifndef CMD_MODALTEST_H
#define CMD_MODALTEST_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    MODALTEST_OPEN_CLOSE_SELFTEST_HEAP_DIRECTION = 0,
    MODALTEST_OPEN_CLOSE_SELFTEST_USED_BLOCKS,
    MODALTEST_OPEN_CLOSE_SELFTEST_ZERO_IS_NOT_IDLE,
    MODALTEST_OPEN_CLOSE_SELFTEST_RECORD_FIT,
    MODALTEST_OPEN_CLOSE_SELFTEST_STATS_DELTA,
    MODALTEST_OPEN_CLOSE_SELFTEST_GLOBAL_HEAP_IS_DIAGNOSTIC,
    MODALTEST_OPEN_CLOSE_SELFTEST_OUTER_HEAP_REQUIRED,
    MODALTEST_OPEN_CLOSE_SELFTEST_TIMER_NODE_NOISE
} modaltest_open_close_selftest_case_t;

bool modaltest_open_close_selftest_case(
    modaltest_open_close_selftest_case_t test_case);
int cmd_modaltest(int argc, char **argv);

#endif
