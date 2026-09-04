#include "cmd_kill.h"

#include "cmd_killtest.h"
#include "../../core/scheduler.h"
#include "../../drivers/serial.h"
#include "../../graphics/console.h"

static void serial_u64(uint64_t value);

static void kill_write(const char *text)
{
    console_write(text);
    serial_write_all(text);
}

static void kill_pid(task_id_t id)
{
    console_print_dec((uint64_t)id);
    serial_u64(id);
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

static void cli_log(int argc, task_id_parse_result_t parse, task_id_t id,
                    task_kill_result_t scheduler, kill_cli_status_t status)
{
    if (!killtest_cli_telemetry_enabled())
        return;
    serial_write_all("[KILL][CLI] argc=");
    serial_u64(argc < 0 ? 0u : (uint64_t)argc);
    serial_write_all(" parse=");
    serial_write_all(task_id_parse_result_to_string(parse));
    serial_write_all(" pid=");
    serial_u64(id);
    serial_write_all(" scheduler=");
    serial_write_all(scheduler_task_kill_result_to_string(scheduler));
    serial_write_all(" status=");
    serial_u64((uint64_t)status);
    serial_write_all("\n");
}

static kill_cli_status_t kill_status_for_result(task_kill_result_t result)
{
    switch (result)
    {
    case TASK_KILL_ACCEPTED: return KILL_CLI_ACCEPTED;
    case TASK_KILL_ALREADY_PENDING: return KILL_CLI_ALREADY_PENDING;
    case TASK_KILL_ALREADY_ZOMBIE: return KILL_CLI_ALREADY_ZOMBIE;
    case TASK_KILL_ALREADY_EXITING: return KILL_CLI_ALREADY_EXITING;
    case TASK_KILL_ERR_PROTECTED: return KILL_CLI_PROTECTED;
    case TASK_KILL_ERR_NOT_KILLABLE: return KILL_CLI_NOT_KILLABLE;
    case TASK_KILL_ERR_NOT_FOUND: return KILL_CLI_NOT_FOUND;
    default: return KILL_CLI_INTERNAL_ERROR;
    }
}

int cmd_kill(int argc, char **argv)
{
    if (argc != 2)
    {
        cli_log(argc, TASK_ID_PARSE_INVALID_ARGUMENT, TASK_ID_INVALID,
                TASK_KILL_ERR_INVALID, KILL_CLI_USAGE);
        kill_write("Usage: kill <pid>\n");
        return KILL_CLI_USAGE;
    }

    task_id_t id = TASK_ID_INVALID;
    task_id_parse_result_t parse = task_id_parse_decimal_ex(argv[1], &id);
    if (parse != TASK_ID_PARSE_OK)
    {
        cli_log(argc, parse, id, TASK_KILL_ERR_INVALID,
                KILL_CLI_INVALID_PID);
        kill_write("kill: invalid task PID (");
        kill_write(task_id_parse_result_to_string(parse));
        kill_write(")\n");
        return KILL_CLI_INVALID_PID;
    }

    task_kill_result_t result = scheduler_request_kill(id);
    kill_cli_status_t status = kill_status_for_result(result);
    cli_log(argc, parse, id, result, status);

    switch (result)
    {
    case TASK_KILL_ACCEPTED:
        kill_write("kill: cooperative cancellation requested for task PID ");
        kill_pid(id);
        kill_write("\n");
        break;
    case TASK_KILL_ALREADY_PENDING:
        kill_write("kill: cancellation is already pending for task PID ");
        kill_pid(id);
        kill_write("\n");
        break;
    case TASK_KILL_ALREADY_ZOMBIE:
        kill_write("kill: task PID ");
        kill_pid(id);
        kill_write(" is already a ZOMBIE\n");
        break;
    case TASK_KILL_ALREADY_EXITING:
        kill_write("kill: task PID ");
        kill_pid(id);
        kill_write(" is already exiting\n");
        break;
    case TASK_KILL_ERR_PROTECTED:
        kill_write("kill: task PID ");
        kill_pid(id);
        kill_write(" is protected\n");
        break;
    case TASK_KILL_ERR_NOT_KILLABLE:
        kill_write("kill: task PID ");
        kill_pid(id);
        kill_write(" is not cooperatively killable\n");
        break;
    case TASK_KILL_ERR_NOT_FOUND:
        kill_write("kill: task PID ");
        kill_pid(id);
        kill_write(" was not found\n");
        break;
    default:
        kill_write("kill: invalid task PID (INVALID_ARGUMENT)\n");
        break;
    }
    return status;
}
