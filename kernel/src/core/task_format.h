#ifndef TASK_FORMAT_H
#define TASK_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#include "scheduler.h"
#include "task_metrics.h"

typedef enum
{
    TASK_FORMAT_LONG = 0,
    TASK_FORMAT_COMPACT
} task_format_style_t;

#define TASK_FORMAT_PID_CAP       21u
#define TASK_FORMAT_USER_CAP       6u
#define TASK_FORMAT_CPU_CAP       12u
#define TASK_FORMAT_STATE_CAP     16u
#define TASK_FORMAT_CLASS_CAP     16u
#define TASK_FORMAT_QUANTUM_CAP   24u
#define TASK_FORMAT_RUNTIME_CAP   32u
#define TASK_FORMAT_PERCENT_CAP   16u
#define TASK_FORMAT_KILL_CAP      12u
#define TASK_FORMAT_MEMORY_CAP    32u
#define TASK_FORMAT_NAME_CAP      32u

typedef struct
{
    char pid[TASK_FORMAT_PID_CAP];
    char user[TASK_FORMAT_USER_CAP];

    char cpu[TASK_FORMAT_CPU_CAP];
    char last_cpu[TASK_FORMAT_CPU_CAP];
    char affinity[TASK_FORMAT_CPU_CAP];

    char state[TASK_FORMAT_STATE_CAP];
    char task_class[TASK_FORMAT_CLASS_CAP];
    char quantum[TASK_FORMAT_QUANTUM_CAP];

    char runtime[TASK_FORMAT_RUNTIME_CAP];
    char cpu_percent[TASK_FORMAT_PERCENT_CAP];

    char kill_status[TASK_FORMAT_KILL_CAP];
    char memory[TASK_FORMAT_MEMORY_CAP];
    char name[TASK_FORMAT_NAME_CAP];

    uint8_t cpu_valid;
    uint8_t cpu_anomalous;
    uint8_t zombie;
} task_format_fields_t;

typedef enum
{
    TASK_FORMAT_COL_PID = 0,
    TASK_FORMAT_COL_USER,
    TASK_FORMAT_COL_CPU,
    TASK_FORMAT_COL_LCPU,
    TASK_FORMAT_COL_AFF,
    TASK_FORMAT_COL_STATE,
    TASK_FORMAT_COL_CLASS,
    TASK_FORMAT_COL_QUANTUM,
    TASK_FORMAT_COL_TIME,
    TASK_FORMAT_COL_CPU_PERCENT,
    TASK_FORMAT_COL_KILL,
    TASK_FORMAT_COL_MEMORY,
    TASK_FORMAT_COL_NAME,
    TASK_FORMAT_COL_COUNT
} task_format_column_id_t;

typedef struct
{
    task_format_column_id_t id;
    uint32_t long_width;
    uint32_t compact_width;
    const char *long_header;
    const char *compact_header;
} task_format_column_spec_t;

const task_format_column_spec_t *task_format_column_spec(
    task_format_column_id_t id);

const char *task_format_state(task_state_t state, task_format_style_t style);
const char *task_format_class(int task_class, task_format_style_t style);
const char *task_format_kill_status(const task_snapshot_t *task);

bool task_format_pid(task_id_t id, char *out, uint32_t out_size);
bool task_format_runtime(uint64_t runtime_ns, char *out, uint32_t out_size);
bool task_format_cpu_percent(const task_cpu_sample_t *sample,
                             task_state_t state, char *out,
                             uint32_t out_size);
bool task_format_memory(uint64_t bytes, char *out, uint32_t out_size);
bool task_format_name(const char *name, uint32_t width, char *out,
                      uint32_t out_size, bool *truncated);
bool task_format_snapshot_fields(const task_snapshot_t *task,
                                 const task_cpu_sample_t *sample,
                                 task_format_style_t style,
                                 task_format_fields_t *out);

void task_snapshot_sort_by_pid(task_snapshot_t *tasks, uint32_t count);

bool task_view_trace_set(task_id_t id);
void task_view_trace_disable(void);
task_id_t task_view_trace_target(void);
void task_view_trace_emit(const char *source, const task_snapshot_t *task,
                          const task_cpu_sample_t *sample);

bool task_format_selftest(uint64_t *out_cases);

#endif
