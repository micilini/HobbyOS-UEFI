#include "cmd_ps.h"

#include "cmd_reaptest.h"
#include "../../core/scheduler.h"
#include "../../core/task_format.h"
#include "../../core/task_metrics.h"
#include "../../drivers/serial.h"
#include "../../graphics/console.h"

#define PS_DEFAULT_PAGE_SIZE 32u
#define PS_MAX_PAGE_SIZE 128u
#define PS_SNAPSHOT_RETRIES 4u

static task_snapshot_t g_ps_tasks[PS_MAX_PAGE_SIZE];
static task_cpu_sample_t g_ps_samples[PS_MAX_PAGE_SIZE];
static task_cpu_sampler_t g_ps_sampler;

static void ps_error(const char *text)
{
    console_write(text);
    serial_write_all(text);
}

static void serial_u64(uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do
    {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        serial_putc_all(reverse[--count]);
}

static bool parse_bounded_u32(const char *text, uint32_t minimum,
                              uint32_t maximum, uint32_t *out)
{
    task_id_t value = 0;
    if (!out || task_id_parse_decimal_ex(text, &value) != TASK_ID_PARSE_OK ||
        value < minimum || value > maximum)
        return false;
    *out = (uint32_t)value;
    return true;
}

static void print_field(const char *value)
{
    console_write(value ? value : "");
    console_write(" ");
}

static void print_task_row(const task_snapshot_t *task,
                           const task_cpu_sample_t *sample)
{
    task_format_fields_t fields;
    if (!task_format_snapshot_fields(task, sample, TASK_FORMAT_LONG, &fields))
    {
        ps_error("ps: row formatting failed\n");
        return;
    }
    print_field(fields.pid);
    print_field(fields.user);
    print_field(fields.cpu);
    print_field(fields.last_cpu);
    print_field(fields.affinity);
    print_field(fields.state);
    print_field(fields.task_class);
    print_field(fields.quantum);
    print_field(fields.runtime);
    print_field(fields.cpu_percent);
    print_field(fields.kill_status);
    print_field(fields.memory);
    console_write(fields.name);
    console_write("\n");
}

static void serial_snapshot(const task_snapshot_result_t *snapshot,
                            uint32_t page, uint32_t pages,
                            uint32_t page_size)
{
    serial_write_all("[PS][SNAPSHOT] generation=");
    serial_u64(snapshot->registry_generation);
    serial_write_all(" page=");
    serial_u64(page);
    serial_write_all(" pages=");
    serial_u64(pages);
    serial_write_all(" page_size=");
    serial_u64(page_size);
    serial_write_all(" offset=");
    serial_u64(snapshot->offset);
    serial_write_all(" written=");
    serial_u64(snapshot->written);
    serial_write_all(" total=");
    serial_u64(snapshot->total);
    serial_write_all(" has_previous=");
    serial_u64(page > 1u);
    serial_write_all(" has_next=");
    serial_u64(page < pages);
    serial_write_all("\n");
}

int cmd_ps(int argc, char **argv)
{
    if (argc > 3)
    {
        ps_error("Usage: ps [page] [page_size]\n");
        return 1;
    }

    uint32_t page = 1;
    uint32_t page_size = PS_DEFAULT_PAGE_SIZE;
    if (argc >= 2 && !parse_bounded_u32(argv[1], 1, UINT32_MAX, &page))
    {
        ps_error("ps: page must be a positive decimal number\n");
        ps_error("Usage: ps [page] [page_size]\n");
        return 1;
    }
    if (argc == 3 &&
        !parse_bounded_u32(argv[2], 1, PS_MAX_PAGE_SIZE, &page_size))
    {
        ps_error("ps: page_size must be 1..128\n");
        ps_error("Usage: ps [page] [page_size]\n");
        return 1;
    }

    task_snapshot_result_t snapshot = {0};
    uint32_t pages = 1;
    uint8_t captured = 0;
    for (uint32_t retry = 0; retry < PS_SNAPSHOT_RETRIES; retry++)
    {
        task_snapshot_result_t probe = scheduler_snapshot_tasks(NULL, 0, 0);
        pages = probe.total ? (probe.total + page_size - 1u) / page_size : 1u;
        if (page > pages)
        {
            ps_error("ps: page is outside the task snapshot range\n");
            ps_error("Usage: ps [page] [page_size]\n");
            return 1;
        }
        uint32_t offset = (page - 1u) * page_size;
        task_snapshot_result_t current = scheduler_snapshot_tasks(
            g_ps_tasks, page_size, offset);
        uint32_t expected = current.total > offset ? current.total - offset : 0;
        if (expected > page_size)
            expected = page_size;
        if (current.registry_generation == probe.registry_generation &&
            current.total == probe.total && current.offset == offset &&
            current.written == expected)
        {
            snapshot = current;
            captured = 1;
            break;
        }
    }
    if (!captured)
    {
        ps_error("ps: task registry changed repeatedly; retry\n");
        return 1;
    }

    task_snapshot_sort_by_pid(g_ps_tasks, snapshot.written);
    uint8_t had_baseline = g_ps_sampler.initialized;
    uint64_t prior_sample_ns = g_ps_sampler.sample_time_ns;
    if (!task_cpu_sampler_sample(&g_ps_sampler, g_ps_tasks, snapshot.written,
                                 snapshot.sample_time_ns, g_ps_samples,
                                 PS_MAX_PAGE_SIZE))
    {
        ps_error("ps: CPU sample failed\n");
        return 1;
    }
    uint64_t delta_ns = had_baseline && snapshot.sample_time_ns >= prior_sample_ns
                            ? snapshot.sample_time_ns - prior_sample_ns
                            : 0;

    serial_write_all("[PS][WINDOW] baseline=");
    serial_u64(had_baseline ? 1u : 0u);
    serial_write_all(" sample_ns=");
    serial_u64(snapshot.sample_time_ns);
    serial_write_all(" delta_ns=");
    serial_u64(delta_ns);
    serial_write_all("\n");
    serial_snapshot(&snapshot, page, pages, page_size);

    console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("HobbyOS TASK SNAPSHOT\n");
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write("Generation ");
    console_print_dec(snapshot.registry_generation);
    console_write(" | Total ");
    console_print_dec(snapshot.total);
    console_write(" | Page ");
    console_print_dec(page);
    console_write("/");
    console_print_dec(pages);
    console_write(" | Showing ");
    if (snapshot.written)
    {
        console_print_dec(snapshot.offset + 1u);
        console_write("-");
        console_print_dec(snapshot.offset + snapshot.written);
    }
    else
        console_write("0-0");
    console_write(" | Page size ");
    console_print_dec(page_size);
    console_write("\n");
    if (had_baseline)
    {
        console_write("CPU window: ");
        console_print_dec(delta_ns / 1000000ULL);
        console_write(" ms since previous ps sample\n");
    }
    else
        console_write("CPU window: initial sample; %CPU is --\n");
    console_write("PID USER CPU LCPU AFF STATE CLASS Q TIME+ %CPU KILL MEM~ NAME\n");

    for (uint32_t index = 0; index < snapshot.written; index++)
    {
        task_snapshot_t *task = &g_ps_tasks[index];
        if (task->state == TASK_ZOMBIE)
            reaptest_zombie_mem_observe_ps(task->id,
                                           task->kernel_mem_est_bytes);
        print_task_row(task, &g_ps_samples[index]);
        serial_write_all("[PS][ROW] page=");
        serial_u64(page);
        serial_write_all(" id=");
        serial_u64(task->id);
        serial_write_all("\n");
        task_view_trace_emit("PS", task, &g_ps_samples[index]);
    }

    if (page < pages)
    {
        console_write("Next: ps ");
        console_print_dec(page + 1u);
        console_write(" ");
        console_print_dec(page_size);
        console_write("\n");
    }
    if (page > 1u)
    {
        console_write("Previous: ps ");
        console_print_dec(page - 1u);
        console_write(" ");
        console_print_dec(page_size);
        console_write("\n");
    }
    return 0;
}
