#ifndef CMD_TASKMANTEST_H
#define CMD_TASKMANTEST_H

#include <stdbool.h>
#include <stdint.h>

#include "cmd_taskman.h"

#define TASKMANTEST_FIXTURE_MAX 257u
#define TASKMANTEST_CONTROL_RECORD_MAX 512u

typedef struct
{
    uint32_t max;
    uint32_t active;
    uint32_t handles_present;
    uint8_t gate_released;
    uint8_t cleanup_in_progress;
} taskmantest_fixture_snapshot_t;

typedef struct
{
    uint32_t refresh_ms;
    uint32_t target_frames;

    uint64_t sessions_delta;
    uint64_t auto_exit_sessions_delta;

    uint64_t full_frames_delta;
    uint64_t fallback_frames_delta;

    uint64_t render_failures_delta;
    uint64_t shortfalls_delta;

    uint64_t modal_scroll_delta;
    uint64_t shell_exit_scroll_delta;
    uint64_t clipped_delta;

    uint32_t last_mode;
    uint32_t last_pages;
    uint32_t last_captured;

    uint8_t model_live;
    uint8_t pending;
    uint8_t controls_default;

    uint8_t valid;
} taskmantest_auto_session_result_t;

typedef enum
{
    TASKMANTEST_AUTO_SESSION_SELFTEST_DELTA = 0,
    TASKMANTEST_AUTO_SESSION_SELFTEST_REFRESH_PATTERN,
    TASKMANTEST_AUTO_SESSION_SELFTEST_RECORD_FIT
} taskmantest_auto_session_selftest_id_t;

uint32_t taskmantest_fixture_max(void);
void taskmantest_fixture_snapshot(taskmantest_fixture_snapshot_t *out);
bool taskmantest_auto_session_result_valid(
    const taskman_stats_t *before,
    const taskman_stats_t *after,
    uint32_t target_frames,
    taskmantest_auto_session_result_t *out);
bool taskmantest_auto_session_selftest_case(
    taskmantest_auto_session_selftest_id_t test);
int cmd_taskmantest(int argc,char **argv);

#endif
