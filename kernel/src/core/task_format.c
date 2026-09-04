#include "task_format.h"

#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../libc/string.h"

typedef struct
{
    char *out;
    uint32_t size;
    uint32_t length;
    uint8_t failed;
} format_builder_t;

static volatile task_id_t g_task_view_trace_id;

static const task_format_column_spec_t g_task_format_columns[
    TASK_FORMAT_COL_COUNT] = {
    {TASK_FORMAT_COL_PID, 21, 21, "PID", "PID"},
    {TASK_FORMAT_COL_USER, 5, 2, "USER", "U"},
    {TASK_FORMAT_COL_CPU, 4, 3, "CPU", "C"},
    {TASK_FORMAT_COL_LCPU, 5, 3, "LCPU", "L"},
    {TASK_FORMAT_COL_AFF, 4, 2, "AFF", "A"},
    {TASK_FORMAT_COL_STATE, 9, 4, "STATE", "ST"},
    {TASK_FORMAT_COL_CLASS, 12, 2, "CLASS", "C"},
    {TASK_FORMAT_COL_QUANTUM, 8, 4, "Q", "Q"},
    {TASK_FORMAT_COL_TIME, 12, 8, "TIME+", "TIME"},
    {TASK_FORMAT_COL_CPU_PERCENT, 7, 6, "%CPU", "CPU%"},
    {TASK_FORMAT_COL_KILL, 6, 5, "KILL", "KILL"},
    {TASK_FORMAT_COL_MEMORY, 9, 6, "MEM~", "MEM"},
    {TASK_FORMAT_COL_NAME, 1, 1, "NAME", "NAME"},
};

static void builder_init(format_builder_t *builder, char *out, uint32_t size)
{
    builder->out = out;
    builder->size = size;
    builder->length = 0;
    builder->failed = (!out || !size);
    if (out && size)
        out[0] = 0;
}

static void builder_char(format_builder_t *builder, char value)
{
    if (builder->failed)
        return;
    if (builder->length + 1 >= builder->size)
    {
        builder->failed = 1;
        builder->out[0] = 0;
        return;
    }
    builder->out[builder->length++] = value;
    builder->out[builder->length] = 0;
}

static void builder_text(format_builder_t *builder, const char *text)
{
    if (!text)
    {
        builder->failed = 1;
        if (builder->out && builder->size)
            builder->out[0] = 0;
        return;
    }
    while (*text)
        builder_char(builder, *text++);
}

static void builder_u64(format_builder_t *builder, uint64_t value)
{
    char reverse[TASK_FORMAT_PID_CAP];
    uint32_t count = 0;
    do
    {
        reverse[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value && count < sizeof(reverse));
    while (count)
        builder_char(builder, reverse[--count]);
}

static bool format_u64(uint64_t value, char *out, uint32_t out_size)
{
    format_builder_t builder;
    builder_init(&builder, out, out_size);
    builder_u64(&builder, value);
    return !builder.failed;
}

static bool copy_text(const char *text, char *out, uint32_t out_size)
{
    format_builder_t builder;
    builder_init(&builder, out, out_size);
    builder_text(&builder, text);
    return !builder.failed;
}

const task_format_column_spec_t *task_format_column_spec(
    task_format_column_id_t id)
{
    if ((uint32_t)id >= TASK_FORMAT_COL_COUNT)
        return NULL;
    return &g_task_format_columns[id];
}

const char *task_format_state(task_state_t state, task_format_style_t style)
{
    const char *long_name = scheduler_task_state_to_string(state);
    if (style == TASK_FORMAT_LONG)
        return long_name;
    if (!strcmp(long_name, "READY"))
        return "RDY";
    if (!strcmp(long_name, "RUNNING"))
        return "RUN";
    if (!strcmp(long_name, "BLOCKED"))
        return "BLK";
    if (!strcmp(long_name, "SLEEPING"))
        return "SLP";
    if (!strcmp(long_name, "ZOMBIE"))
        return "ZMB";
    return "UNK";
}

const char *task_format_class(int task_class, task_format_style_t style)
{
    if (task_class == TASK_CLASS_INTERACTIVE)
        return style == TASK_FORMAT_COMPACT ? "I" : "Interactive";
    if (task_class == TASK_CLASS_NORMAL)
        return style == TASK_FORMAT_COMPACT ? "N" : "Normal";
    return style == TASK_FORMAT_COMPACT ? "?" : "Unknown";
}

const char *task_format_kill_status(const task_snapshot_t *task)
{
    return scheduler_task_kill_state_to_string(task);
}

bool task_format_pid(task_id_t id, char *out, uint32_t out_size)
{
    return format_u64(id, out, out_size);
}

bool task_format_runtime(uint64_t runtime_ns, char *out, uint32_t out_size)
{
    return task_metrics_format_runtime(runtime_ns, out, out_size);
}

bool task_format_cpu_percent(const task_cpu_sample_t *sample,
                             task_state_t state, char *out,
                             uint32_t out_size)
{
    format_builder_t builder;
    builder_init(&builder, out, out_size);
    if (state == TASK_ZOMBIE)
    {
        builder_text(&builder, "0.0%");
        return !builder.failed;
    }
    if (!sample || !sample->valid)
    {
        builder_text(&builder, "--");
        return !builder.failed;
    }
    if (sample->anomalous)
        builder_char(&builder, '!');
    builder_u64(&builder, sample->cpu_x10 / 10u);
    builder_char(&builder, '.');
    builder_u64(&builder, sample->cpu_x10 % 10u);
    builder_char(&builder, '%');
    return !builder.failed;
}

bool task_format_memory(uint64_t bytes, char *out, uint32_t out_size)
{
    const char *unit = "B";
    uint64_t divisor = 1;
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
    {
        unit = "GiB";
        divisor = 1024ULL * 1024ULL * 1024ULL;
    }
    else if (bytes >= 1024ULL * 1024ULL)
    {
        unit = "MiB";
        divisor = 1024ULL * 1024ULL;
    }
    else if (bytes >= 1024ULL)
    {
        unit = "KiB";
        divisor = 1024ULL;
    }

    format_builder_t builder;
    builder_init(&builder, out, out_size);
    builder_u64(&builder, bytes / divisor);
    if (divisor != 1)
    {
        builder_char(&builder, '.');
        builder_u64(&builder, ((bytes % divisor) * 10u) / divisor);
    }
    builder_text(&builder, unit);
    return !builder.failed;
}

bool task_format_name(const char *name, uint32_t width, char *out,
                      uint32_t out_size, bool *truncated)
{
    if (truncated)
        *truncated = false;
    if (!out || !out_size || width >= out_size)
        return false;

    const char *source = (name && name[0]) ? name : "(unnamed)";
    uint32_t count = 0;
    while (source[count] && count < width)
    {
        char value = source[count];
        out[count] = (value == '\n' || value == '\r' || value == '\t')
                         ? ' '
                         : value;
        count++;
    }
    if (source[count])
    {
        if (truncated)
            *truncated = true;
        if (width == 1)
            out[0] = '.';
        else if (width == 2)
            out[0] = out[1] = '.';
        else if (width)
            out[width - 3] = out[width - 2] = out[width - 1] = '.';
        count = width;
    }
    out[count] = 0;
    return true;
}

bool task_format_snapshot_fields(const task_snapshot_t *task,
                                 const task_cpu_sample_t *sample,
                                 task_format_style_t style,
                                 task_format_fields_t *out)
{
    if (!task || !out || (style != TASK_FORMAT_LONG &&
                          style != TASK_FORMAT_COMPACT))
        return false;
    memset(out, 0, sizeof(*out));

    bool ok = task_format_pid(task->id, out->pid, sizeof(out->pid));
    ok = ok && copy_text(style == TASK_FORMAT_COMPACT ? "R" : "Root",
                         out->user, sizeof(out->user));
    if (task->on_cpu || task->is_current)
        ok = ok && format_u64(task->current_cpu_slot, out->cpu,
                              sizeof(out->cpu));
    else
        ok = ok && copy_text("-", out->cpu, sizeof(out->cpu));
    if (task->last_cpu_slot == TASK_CPU_SLOT_NONE)
        ok = ok && copy_text("-", out->last_cpu, sizeof(out->last_cpu));
    else
        ok = ok && format_u64(task->last_cpu_slot, out->last_cpu,
                              sizeof(out->last_cpu));
    ok = ok && copy_text(style == TASK_FORMAT_COMPACT ? "A" : "Any",
                         out->affinity, sizeof(out->affinity));
    ok = ok && copy_text(task_format_state(task->state, style), out->state,
                         sizeof(out->state));
    ok = ok && copy_text(task_format_class(task->task_class, style),
                         out->task_class, sizeof(out->task_class));

    format_builder_t quantum;
    builder_init(&quantum, out->quantum, sizeof(out->quantum));
    builder_u64(&quantum, (uint32_t)task->quantum);
    if (style == TASK_FORMAT_LONG)
    {
        builder_char(&quantum, '/');
        builder_u64(&quantum, (uint32_t)task->quantum_default);
    }
    ok = ok && !quantum.failed;
    ok = ok && task_format_runtime(task->runtime_ns_total, out->runtime,
                                   sizeof(out->runtime));
    ok = ok && task_format_cpu_percent(sample, task->state,
                                       out->cpu_percent,
                                       sizeof(out->cpu_percent));
    ok = ok && copy_text(task_format_kill_status(task), out->kill_status,
                         sizeof(out->kill_status));
    ok = ok && task_format_memory(task->kernel_mem_est_bytes, out->memory,
                                  sizeof(out->memory));
    bool ignored = false;
    ok = ok && task_format_name(task->name, TASK_FORMAT_NAME_CAP - 1u,
                                out->name, sizeof(out->name), &ignored);

    out->cpu_valid = task->state == TASK_ZOMBIE ||
                     (sample && sample->valid);
    out->cpu_anomalous = task->state != TASK_ZOMBIE && sample &&
                         sample->valid && sample->anomalous;
    out->zombie = task->state == TASK_ZOMBIE;
    return ok;
}

static void swap_snapshot(task_snapshot_t *left, task_snapshot_t *right)
{
    task_snapshot_t value = *left;
    *left = *right;
    *right = value;
}

static void snapshot_sift(task_snapshot_t *tasks, uint32_t count,
                          uint32_t root)
{
    for (;;)
    {
        uint32_t child = root * 2u + 1u;
        if (child >= count)
            return;
        if (child + 1u < count && tasks[child].id < tasks[child + 1u].id)
            child++;
        if (tasks[root].id >= tasks[child].id)
            return;
        swap_snapshot(&tasks[root], &tasks[child]);
        root = child;
    }
}

void task_snapshot_sort_by_pid(task_snapshot_t *tasks, uint32_t count)
{
    if (!tasks || count < 2)
        return;
    for (uint32_t index = count / 2u; index; index--)
        snapshot_sift(tasks, count, index - 1u);
    for (uint32_t end = count - 1u; end; end--)
    {
        swap_snapshot(&tasks[0], &tasks[end]);
        snapshot_sift(tasks, end, 0);
    }
}

bool task_view_trace_set(task_id_t id)
{
    if (id == TASK_ID_INVALID)
        return false;
    __atomic_store_n(&g_task_view_trace_id, id, __ATOMIC_RELEASE);
    return true;
}

void task_view_trace_disable(void)
{
    __atomic_store_n(&g_task_view_trace_id, TASK_ID_INVALID,
                     __ATOMIC_RELEASE);
}

task_id_t task_view_trace_target(void)
{
    return __atomic_load_n(&g_task_view_trace_id, __ATOMIC_ACQUIRE);
}

static void serial_u64(uint64_t value)
{
    char reverse[TASK_FORMAT_PID_CAP];
    uint32_t count = 0;
    do
    {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        serial_putc_all(reverse[--count]);
}

void task_view_trace_emit(const char *source, const task_snapshot_t *task,
                          const task_cpu_sample_t *sample)
{
    if (!source || !task || task->id != task_view_trace_target())
        return;
    task_format_fields_t fields;
    if (!task_format_snapshot_fields(task, sample, TASK_FORMAT_LONG, &fields))
        return;
    serial_write_all("[TASKVIEW][ROW] source=");
    serial_write_all(source);
    serial_write_all(" id=");
    serial_u64(task->id);
    serial_write_all(" state=");
    serial_write_all(fields.state);
    serial_write_all(" class=");
    serial_write_all(fields.task_class);
    serial_write_all(" runtime=");
    serial_write_all(fields.runtime);
    serial_write_all(" cpu=");
    serial_write_all(fields.cpu_percent);
    serial_write_all(" kill=");
    serial_write_all(fields.kill_status);
    serial_write_all(" mem=");
    serial_write_all(fields.memory);
    serial_write_all(" mem_bytes=");
    serial_u64(task->kernel_mem_est_bytes);
    serial_write_all(" name=");
    serial_write_all(fields.name);
    serial_write_all("\n");
}

bool task_format_selftest(uint64_t *out_cases)
{
    uint64_t cases = 0;
    bool ok = true;
    char out[64];
    bool truncated = false;
    task_cpu_sample_t sample = {0};
    task_snapshot_t task;
    memset(&task, 0, sizeof(task));

#define FORMAT_CASE(expression) do { cases++; ok = (expression) && ok; } while (0)
    FORMAT_CASE(!strcmp(task_format_state(TASK_READY, TASK_FORMAT_LONG),
                        "READY"));
    FORMAT_CASE(!strcmp(task_format_state(TASK_READY, TASK_FORMAT_COMPACT),
                        "RDY"));
    FORMAT_CASE(!strcmp(task_format_state((task_state_t)99,
                                          TASK_FORMAT_COMPACT), "UNK"));
    FORMAT_CASE(!strcmp(task_format_class(TASK_CLASS_INTERACTIVE,
                                          TASK_FORMAT_LONG), "Interactive"));
    FORMAT_CASE(!strcmp(task_format_class(TASK_CLASS_NORMAL,
                                          TASK_FORMAT_COMPACT), "N"));
    FORMAT_CASE(!strcmp(task_format_class(99, TASK_FORMAT_LONG), "Unknown"));
    FORMAT_CASE(task_format_pid(1, out, sizeof(out)) && !strcmp(out, "1"));
    FORMAT_CASE(task_format_pid(UINT64_MAX, out, sizeof(out)) &&
                !strcmp(out, "18446744073709551615"));
    FORMAT_CASE(task_format_runtime(0, out, sizeof(out)) &&
                !strcmp(out, "00:00.000"));
    FORMAT_CASE(task_format_runtime(65000000000ULL, out, sizeof(out)) &&
                !strcmp(out, "01:05.000"));
    FORMAT_CASE(task_format_runtime(360000000000000ULL, out, sizeof(out)) &&
                !strcmp(out, "100:00:00.000"));
    FORMAT_CASE(task_format_runtime(UINT64_MAX, out, sizeof(out)));
    FORMAT_CASE(task_format_cpu_percent(NULL, TASK_READY, out, sizeof(out)) &&
                !strcmp(out, "--"));
    sample.valid = 1; sample.cpu_x10 = 123;
    FORMAT_CASE(task_format_cpu_percent(&sample, TASK_READY, out,
                                        sizeof(out)) && !strcmp(out, "12.3%"));
    sample.anomalous = 1;
    FORMAT_CASE(task_format_cpu_percent(&sample, TASK_READY, out,
                                        sizeof(out)) && !strcmp(out, "!12.3%"));
    FORMAT_CASE(task_format_cpu_percent(NULL, TASK_ZOMBIE, out,
                                        sizeof(out)) && !strcmp(out, "0.0%"));
    FORMAT_CASE(task_format_memory(0, out, sizeof(out)) && !strcmp(out, "0B"));
    FORMAT_CASE(task_format_memory(1023, out, sizeof(out)) &&
                !strcmp(out, "1023B"));
    FORMAT_CASE(task_format_memory(1024, out, sizeof(out)) &&
                !strcmp(out, "1.0KiB"));
    FORMAT_CASE(task_format_memory(16800, out, sizeof(out)) &&
                !strcmp(out, "16.4KiB"));
    FORMAT_CASE(task_format_memory(1024ULL * 1024ULL, out, sizeof(out)) &&
                !strcmp(out, "1.0MiB"));
    FORMAT_CASE(task_format_memory(1024ULL * 1024ULL * 1024ULL, out,
                                   sizeof(out)) && !strcmp(out, "1.0GiB"));
    task.flags = TASK_FLAG_KILL_PROTECTED;
    FORMAT_CASE(!strcmp(task_format_kill_status(&task), "PROT"));
    task.flags = 0;
    FORMAT_CASE(!strcmp(task_format_kill_status(&task), "NO"));
    task.flags = TASK_FLAG_KILLABLE;
    FORMAT_CASE(!strcmp(task_format_kill_status(&task), "-"));
    task.kill_pending = 1;
    FORMAT_CASE(!strcmp(task_format_kill_status(&task), "PEND"));
    task.exit_started = 1;
    FORMAT_CASE(!strcmp(task_format_kill_status(&task), "EXIT"));
    task.state = TASK_ZOMBIE;
    FORMAT_CASE(!strcmp(task_format_kill_status(&task), "ZOMB"));
    FORMAT_CASE(task_format_name("", 31, out, sizeof(out), &truncated) &&
                !truncated && !strcmp(out, "(unnamed)"));
    FORMAT_CASE(task_format_name("1234567890123456789012345678901", 31,
                                 out, sizeof(out), &truncated) &&
                !truncated && strlen(out) == 31);
#undef FORMAT_CASE
    if (out_cases)
        *out_cases = cases;
    return ok;
}
