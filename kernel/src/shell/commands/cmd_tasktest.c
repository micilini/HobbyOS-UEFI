#include "cmd_tasktest.h"

#include "../../core/selftest.h"
#include "../../core/runtime_ready.h"
#include "../../drivers/serial.h"
#include "../../graphics/console.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"
#include "cmd_ps.h"
#include "../shell.h"

static void tasktest_both(const char *text)
{
    console_write(text);
    serial_write_all(text);
}

#ifdef HOBBYOS_SELFTEST

#define TASKTEST_HARNESS_RECORD_MAX 256u

typedef struct
{
    char bytes[TASKTEST_HARNESS_RECORD_MAX];
    uint32_t length;
    uint8_t overflow;
} tasktest_harness_record_t;

static void harness_record_reset(tasktest_harness_record_t *record)
{
    record->length = 0;
    record->overflow = 0;
    record->bytes[0] = 0;
}

static bool harness_record_char(tasktest_harness_record_t *record, char value)
{
    if (!record || record->overflow ||
        record->length + 1u >= TASKTEST_HARNESS_RECORD_MAX) {
        if (record) record->overflow = 1;
        return false;
    }
    record->bytes[record->length++] = value;
    record->bytes[record->length] = 0;
    return true;
}

static bool harness_record_text(tasktest_harness_record_t *record,
                                const char *text)
{
    if (!text) return false;
    while (*text)
        if (!harness_record_char(record, *text++)) return false;
    return true;
}

static bool harness_record_u64(tasktest_harness_record_t *record,
                               uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        if (!harness_record_char(record, reverse[--count])) return false;
    return true;
}

static bool harness_record_status(tasktest_harness_record_t *record,
                                  int status)
{
    if (status < 0) {
        if (!harness_record_char(record, '-')) return false;
        return harness_record_u64(record, (uint64_t)(-(int64_t)status));
    }
    return harness_record_u64(record, (uint64_t)status);
}

static bool harness_record_hex32(tasktest_harness_record_t *record,
                                 uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t shift = (7u - i) * 4u;
        if (!harness_record_char(record,
            digits[(value >> shift) & 0x0fu])) return false;
    }
    return true;
}

static bool harness_record_finish(tasktest_harness_record_t *record)
{
    return record && !record->overflow && record->length > 12u &&
        record->bytes[0] == '\n' &&
        harness_record_char(record, '\n');
}

static bool harness_record_complete(const tasktest_harness_record_t *record)
{
    static const char prefix[] = "\n[HARNESS][";
    if (!record || record->overflow || record->length < sizeof(prefix) + 1u ||
        record->bytes[record->length] != 0 ||
        record->bytes[record->length - 1u] != '\n') return false;
    for (uint32_t i = 0; i < sizeof(prefix) - 1u; i++)
        if (record->bytes[i] != prefix[i]) return false;
    return true;
}

static bool harness_record_emit(tasktest_harness_record_t *record)
{
    if (!harness_record_finish(record) || !harness_record_complete(record)) {
        static const char error[] =
            "\n[HARNESS][EMIT_ERROR] reason=record-overflow\n";
        console_write(error);
        serial_write_all(error);
        return false;
    }
    console_write(record->bytes);
    serial_write_all(record->bytes);
    return true;
}

static void harness_record_begin(tasktest_harness_record_t *record,
                                 const char *kind)
{
    harness_record_reset(record);
    harness_record_text(record, "\n[HARNESS][");
    harness_record_text(record, kind);
    harness_record_char(record, ']');
}

static bool tasktest_ps_loop_count_valid(uint64_t count)
{
    return count >= 1u && count <= 500u;
}

static bool tasktest_build_ps_loop_record(
    tasktest_harness_record_t *record,
    bool progress,
    bool pass,
    uint32_t requested,
    uint32_t completed,
    uint32_t failure_index,
    int status)
{
    harness_record_begin(record, progress ? "PS_LOOP_PROGRESS" : "PS_LOOP");
    if (!progress)
    {
        harness_record_char(record, ' ');
        harness_record_text(record, pass ? "PASS" : "FAIL");
    }
    harness_record_text(record, " requested=");
    harness_record_u64(record, requested);
    harness_record_text(record, " completed=");
    harness_record_u64(record, completed);
    harness_record_text(record, " failure_index=");
    harness_record_u64(record, failure_index);
    harness_record_text(record, " status=");
    harness_record_status(record, status);
    return harness_record_finish(record) && harness_record_complete(record);
}

static bool tasktest_emit_ps_loop_record(
    bool progress,
    bool pass,
    uint32_t requested,
    uint32_t completed,
    uint32_t failure_index,
    int status)
{
    tasktest_harness_record_t record;
    if (!tasktest_build_ps_loop_record(&record, progress, pass, requested,
                                       completed, failure_index, status))
        return false;
    console_write(record.bytes);
    serial_write_all(record.bytes);
    return true;
}

static const char *const g_tasktest_suites[] = {
    "identity", "runtime", "scheduler", "sync", "accounting", "kill", "reaper",
    "input", "modal", "ui", "format", "registry", "transport", "all"
};

typedef struct
{
    uint64_t next_sequence;
    uint64_t last_sequence;
    uint32_t last_crc;
    int last_status;
    char last_payload[TASKTEST_TRANSPORT_PAYLOAD_MAX + 1u];
    uint64_t accepted;
    uint64_t completed;
    uint64_t replays;
    uint64_t crc_rejects;
    uint64_t sequence_rejects;
    uint64_t syntax_rejects;
    uint64_t recursive_rejects;
} tasktest_transport_state_t;

typedef enum
{
    TRANSPORT_DECISION_ACCEPT = 0,
    TRANSPORT_DECISION_REPLAY,
    TRANSPORT_DECISION_REJECT
} transport_decision_t;

typedef enum
{
    TRANSPORT_PAYLOAD_OK = 0,
    TRANSPORT_PAYLOAD_INVALID,
    TRANSPORT_PAYLOAD_TOO_LONG
} transport_payload_result_t;

static tasktest_transport_state_t g_transport = {
    .next_sequence = 1u
};
static uint8_t g_transport_dispatch_inflight;

#define TASKTEST_TRANSPORT_FRAME_MAX \
    ((sizeof("tasktest exec ") - 1u) + 20u + 1u + 8u + 1u + \
     TASKTEST_TRANSPORT_PAYLOAD_MAX + 1u)

_Static_assert(TASKTEST_TRANSPORT_FRAME_MAX < SHELL_CMD_BUFFER_SIZE,
               "tasktest transport frame must fit the shell line buffer");

static uint32_t tasktest_strlen_bounded(const char *text, uint32_t cap)
{
    uint32_t length = 0;
    if (!text)
        return 0;
    while (length < cap && text[length])
        length++;
    return length;
}

uint32_t tasktest_transport_crc32(const char *payload, uint32_t length)
{
    uint32_t crc = 0xffffffffu;
    if (!payload && length)
        return 0;
    for (uint32_t i = 0; i < length; i++)
    {
        crc ^= (uint8_t)payload[i];
        for (uint32_t bit = 0; bit < 8u; bit++)
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

static bool tasktest_parse_u64(const char *text, uint64_t *out)
{
    if (!text || !text[0] || !out)
        return false;
    uint64_t value = 0;
    for (uint32_t i = 0; text[i]; i++)
    {
        if (text[i] < '0' || text[i] > '9')
            return false;
        uint32_t digit = (uint32_t)(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10u)
            return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static int tasktest_ps_loop(int argc, char **argv)
{
    uint64_t parsed = 0;
    if (argc != 3 || !tasktest_parse_u64(argv[2], &parsed) ||
        !tasktest_ps_loop_count_valid(parsed))
    {
        (void)tasktest_emit_ps_loop_record(false, false,
            parsed <= UINT32_MAX ? (uint32_t)parsed : 0u, 0u, 0u, 64);
        return 64;
    }

    uint32_t requested = (uint32_t)parsed;
    uint32_t completed = 0;
    uint32_t failure_index = 0;
    int status = 0;
    char *ps_argv[] = {"ps"};
    for (uint32_t index = 1; index <= requested; index++)
    {
        status = cmd_ps(1, ps_argv);
        if (status != 0)
        {
            failure_index = index;
            break;
        }
        completed++;
        if (completed % 25u == 0u &&
            !tasktest_emit_ps_loop_record(true, true, requested, completed,
                                          0u, 0))
        {
            status = 70;
            failure_index = index;
            break;
        }
    }

    bool pass = completed == requested && status == 0;
    if (!tasktest_emit_ps_loop_record(false, pass, requested, completed,
                                      failure_index, status))
        return 70;
    return pass ? 0 : status ? status : 1;
}

static bool tasktest_parse_crc(const char *text, uint32_t *out)
{
    if (!text || !out || tasktest_strlen_bounded(text, 9u) != 8u)
        return false;
    uint32_t value = 0;
    for (uint32_t i = 0; i < 8u; i++)
    {
        char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = 10u + (uint32_t)(c - 'a');
        else if (c >= 'A' && c <= 'F') digit = 10u + (uint32_t)(c - 'A');
        else return false;
        value = (value << 4u) | digit;
    }
    *out = value;
    return true;
}

static bool tasktest_payload_character_valid(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == ' ' || c == '-' || c == '.' || c == '+';
}

static transport_payload_result_t tasktest_join_payload(
    int argc, char **argv, int first, char *out, uint32_t out_size,
    uint32_t *out_length)
{
    if (!argv || !out || out_size < 2u || first >= argc)
        return TRANSPORT_PAYLOAD_INVALID;
    uint32_t used = 0;
    for (int arg = first; arg < argc; arg++)
    {
        if (!argv[arg] || !argv[arg][0])
            return TRANSPORT_PAYLOAD_INVALID;
        if (arg != first)
        {
            if (used + 1u >= out_size)
                return TRANSPORT_PAYLOAD_TOO_LONG;
            out[used++] = ' ';
        }
        for (uint32_t i = 0; argv[arg][i]; i++)
        {
            if (!tasktest_payload_character_valid(argv[arg][i]))
                return TRANSPORT_PAYLOAD_INVALID;
            if (used + 1u >= out_size)
                return TRANSPORT_PAYLOAD_TOO_LONG;
            out[used++] = argv[arg][i];
        }
    }
    if (!used || used > TASKTEST_TRANSPORT_PAYLOAD_MAX)
        return used ? TRANSPORT_PAYLOAD_TOO_LONG : TRANSPORT_PAYLOAD_INVALID;
    out[used] = 0;
    if (out_length)
        *out_length = used;
    return TRANSPORT_PAYLOAD_OK;
}

static bool tasktest_payload_recursive(const char *payload)
{
    static const char prefix[] = "tasktest exec";
    uint32_t i = 0;
    while (prefix[i] && payload && payload[i] == prefix[i])
        i++;
    return !prefix[i] && (payload[i] == 0 || payload[i] == ' ');
}

static transport_decision_t tasktest_transport_decide(
    const tasktest_transport_state_t *state, uint64_t sequence,
    uint32_t crc, const char *payload)
{
    if (!state || !sequence || !payload)
        return TRANSPORT_DECISION_REJECT;
    if (sequence == state->last_sequence && state->last_sequence != 0u)
    {
        if (crc == state->last_crc && !strcmp(payload, state->last_payload))
            return TRANSPORT_DECISION_REPLAY;
        return TRANSPORT_DECISION_REJECT;
    }
    return sequence == state->next_sequence ? TRANSPORT_DECISION_ACCEPT :
                                              TRANSPORT_DECISION_REJECT;
}

static void tasktest_transport_reject(uint64_t sequence, const char *reason)
{
    tasktest_harness_record_t record;
    harness_record_begin(&record, "FRAME");
    harness_record_text(&record, " REJECT seq=");
    harness_record_u64(&record, sequence);
    harness_record_text(&record, " reason=");
    harness_record_text(&record, reason);
    harness_record_text(&record, " expected_seq=");
    harness_record_u64(&record, g_transport.next_sequence);
    (void)harness_record_emit(&record);
}

static int tasktest_transport_exec(int argc, char **argv)
{
    uint64_t sequence = 0;
    uint32_t expected_crc = 0;
    char payload[TASKTEST_TRANSPORT_PAYLOAD_MAX + 1u];
    uint32_t length = 0;
    transport_payload_result_t payload_result;

    if (!runtime_ready_is_test_ready()) {
        tasktest_transport_reject(0, "runtime-not-ready");
        return 69;
    }

    if (argc < 5 || !tasktest_parse_u64(argv[2], &sequence) || !sequence ||
        !tasktest_parse_crc(argv[3], &expected_crc))
    {
        g_transport.syntax_rejects++;
        tasktest_transport_reject(sequence, "syntax");
        return 64;
    }
    payload_result = tasktest_join_payload(argc, argv, 4, payload,
                                           sizeof(payload), &length);
    if (payload_result != TRANSPORT_PAYLOAD_OK)
    {
        g_transport.syntax_rejects++;
        tasktest_transport_reject(sequence,
            payload_result == TRANSPORT_PAYLOAD_TOO_LONG ? "too-long" :
                                                           "payload");
        return 64;
    }
    if (tasktest_payload_recursive(payload))
    {
        g_transport.recursive_rejects++;
        tasktest_transport_reject(sequence, "recursive");
        return 64;
    }

    uint32_t actual_crc = tasktest_transport_crc32(payload, length);
    if (actual_crc != expected_crc)
    {
        g_transport.crc_rejects++;
        tasktest_harness_record_t record;
        harness_record_begin(&record, "FRAME");
        harness_record_text(&record, " REJECT seq=");
        harness_record_u64(&record, sequence);
        harness_record_text(&record, " reason=crc expected_seq=");
        harness_record_u64(&record, g_transport.next_sequence);
        harness_record_text(&record, " expected_crc=");
        harness_record_hex32(&record, expected_crc);
        harness_record_text(&record, " actual_crc=");
        harness_record_hex32(&record, actual_crc);
        (void)harness_record_emit(&record);
        return 65;
    }

    transport_decision_t decision = tasktest_transport_decide(
        &g_transport, sequence, expected_crc, payload);
    if (decision == TRANSPORT_DECISION_REPLAY)
    {
        g_transport.replays++;
        tasktest_harness_record_t record;
        harness_record_begin(&record, "REPLAY");
        harness_record_text(&record, " seq=");
        harness_record_u64(&record, sequence);
        harness_record_text(&record, " status=");
        harness_record_status(&record, g_transport.last_status);
        (void)harness_record_emit(&record);
        return g_transport.last_status;
    }
    if (decision != TRANSPORT_DECISION_ACCEPT)
    {
        g_transport.sequence_rejects++;
        tasktest_transport_reject(sequence, "sequence");
        return 65;
    }

    g_transport.accepted++;
    tasktest_harness_record_t record;
    harness_record_begin(&record, "FRAME");
    harness_record_text(&record, " ACCEPT seq=");
    harness_record_u64(&record, sequence);
    harness_record_text(&record, " crc=");
    harness_record_hex32(&record, expected_crc);
    harness_record_text(&record, " len=");
    harness_record_u64(&record, length);
    (void)harness_record_emit(&record);
    harness_record_begin(&record, "BEGIN");
    harness_record_text(&record, " seq=");
    harness_record_u64(&record, sequence);
    (void)harness_record_emit(&record);

    g_transport_dispatch_inflight = 1u;
    int status = shell_execute_command_line_for_selftest(payload);
    g_transport_dispatch_inflight = 0u;

    g_transport.last_sequence = sequence;
    g_transport.last_crc = expected_crc;
    g_transport.last_status = status;
    strcpy(g_transport.last_payload, payload);
    g_transport.next_sequence = sequence + 1u;
    g_transport.completed++;

    harness_record_begin(&record, "END");
    harness_record_text(&record, " seq=");
    harness_record_u64(&record, sequence);
    harness_record_text(&record, " status=");
    harness_record_status(&record, status);
    (void)harness_record_emit(&record);
    return status;
}

static int tasktest_transport_status(void)
{
    tasktest_harness_record_t record;
    uint64_t completed = g_transport.completed +
                         (g_transport_dispatch_inflight ? 1u : 0u);
    harness_record_begin(&record, "STATUS");
    harness_record_text(&record, " accepted=");
    harness_record_u64(&record, g_transport.accepted);
    /* This read-only payload is itself the accepted in-flight frame.  It is
       guaranteed to return zero, so report the post-END completed value. */
    harness_record_text(&record, " completed=");
    harness_record_u64(&record, completed);
    harness_record_text(&record, " crc_rejects=");
    harness_record_u64(&record, g_transport.crc_rejects);
    harness_record_text(&record, " sequence_rejects=");
    harness_record_u64(&record, g_transport.sequence_rejects);
    harness_record_text(&record, " syntax_rejects=");
    harness_record_u64(&record, g_transport.syntax_rejects);
    harness_record_text(&record, " recursive_rejects=");
    harness_record_u64(&record, g_transport.recursive_rejects);
    (void)harness_record_emit(&record);
    harness_record_begin(&record, "STATUS_DETAIL");
    harness_record_text(&record, " next_seq=");
    harness_record_u64(&record, g_transport.next_sequence);
    harness_record_text(&record, " last_seq=");
    harness_record_u64(&record, g_transport.last_sequence);
    harness_record_text(&record, " last_status=");
    harness_record_status(&record, g_transport.last_status);
    harness_record_text(&record, " replays=");
    harness_record_u64(&record, g_transport.replays);
    (void)harness_record_emit(&record);
    return 0;
}

static int tasktest_readiness_status(void)
{
    runtime_ready_snapshot_t snapshot;
    if (!runtime_ready_snapshot(&snapshot)) return 1;
    tasktest_harness_record_t record;
    harness_record_reset(&record);
    harness_record_text(&record, "\n[BOOT][READINESS_STATUS] ");
    harness_record_text(&record,
        snapshot.state == RUNTIME_READY_TEST_READY ? "PASS" : "FAIL");
    harness_record_text(&record, " state=");
    harness_record_text(&record,
        snapshot.state == RUNTIME_READY_TEST_READY ? "TEST_READY" : "OTHER");
    harness_record_text(&record, " cpus=");
    harness_record_u64(&record, snapshot.cpus_online);
    harness_record_char(&record, '/');
    harness_record_u64(&record, snapshot.cpus_expected);
    harness_record_text(&record, " scheduler=");
    harness_record_u64(&record, snapshot.scheduler_started);
    harness_record_text(&record, " dpc=");
    harness_record_u64(&record, snapshot.dpc_initialized);
    harness_record_text(&record, " shell=");
    harness_record_u64(&record, snapshot.shell_initialized &&
                                snapshot.shell_thread_started);
    harness_record_text(&record, " input=");
    harness_record_u64(&record, snapshot.input_valid);
    harness_record_text(&record, " modal=");
    harness_record_u64(&record, snapshot.modal_session_valid &&
                                snapshot.modal_ui_valid);
    harness_record_text(&record, " pci=");
    harness_record_u64(&record, snapshot.pci_scan_complete);
    harness_record_text(&record, " hotplug_active=");
    harness_record_u64(&record, snapshot.hotplug_active);
    harness_record_text(&record, " autorun=");
    harness_record_u64(&record, snapshot.autorun_enabled);
    harness_record_text(&record, " autorun_passed=");
    harness_record_u64(&record, snapshot.autorun_passed);
    bool ok = snapshot.state == RUNTIME_READY_TEST_READY &&
              snapshot.violations == 0;
    if (!harness_record_finish(&record)) ok = false;
    if (!record.overflow) {
        console_write(record.bytes);
        serial_write_all(record.bytes);
    }
    return ok ? 0 : 1;
}

bool tasktest_transport_selftest_case(tasktest_transport_test_id_t test)
{
    static const char vector[] = "123456789";
    static const char sample[] = "inputtest marker sample";
    uint32_t crc = tasktest_transport_crc32(sample,
        (uint32_t)(sizeof(sample) - 1u));
    tasktest_transport_state_t state;
    memset(&state, 0, sizeof(state));
    state.next_sequence = 1u;

    switch (test)
    {
    case TASKTEST_TRANSPORT_TEST_CRC_KNOWN_VECTOR:
        return tasktest_transport_crc32(vector,
                   (uint32_t)(sizeof(vector) - 1u)) == 0xcbf43926u;
    case TASKTEST_TRANSPORT_TEST_CANONICAL_JOIN:
    {
        char *args[] = {"tasktest", "exec", "1", "00000000",
                        "inputtest", "marker", "joined"};
        char joined[TASKTEST_TRANSPORT_PAYLOAD_MAX + 1u];
        uint32_t length = 0;
        return tasktest_join_payload(7, args, 4, joined, sizeof(joined),
                                     &length) == TRANSPORT_PAYLOAD_OK &&
               !strcmp(joined, "inputtest marker joined") && length == 23u;
    }
    case TASKTEST_TRANSPORT_TEST_SEQUENCE_INITIAL:
        return state.next_sequence == 1u && state.last_sequence == 0u &&
               tasktest_transport_decide(&state, 1u, crc, sample) ==
                   TRANSPORT_DECISION_ACCEPT;
    case TASKTEST_TRANSPORT_TEST_SEQUENCE_GAP:
        return tasktest_transport_decide(&state, 2u, crc, sample) ==
               TRANSPORT_DECISION_REJECT;
    case TASKTEST_TRANSPORT_TEST_SEQUENCE_STALE:
        state.next_sequence = 3u; state.last_sequence = 2u;
        state.last_crc = crc; strcpy(state.last_payload, sample);
        return tasktest_transport_decide(&state, 1u, crc, sample) ==
               TRANSPORT_DECISION_REJECT;
    case TASKTEST_TRANSPORT_TEST_REPLAY_SAME:
        state.next_sequence = 2u; state.last_sequence = 1u;
        state.last_crc = crc; strcpy(state.last_payload, sample);
        return tasktest_transport_decide(&state, 1u, crc, sample) ==
               TRANSPORT_DECISION_REPLAY;
    case TASKTEST_TRANSPORT_TEST_REPLAY_MISMATCH:
        state.next_sequence = 2u; state.last_sequence = 1u;
        state.last_crc = crc; strcpy(state.last_payload, sample);
        return tasktest_transport_decide(&state, 1u, crc ^ 1u, sample) ==
               TRANSPORT_DECISION_REJECT;
    case TASKTEST_TRANSPORT_TEST_RECURSIVE:
        return tasktest_payload_recursive("tasktest exec 1 00000000 ps") &&
               !tasktest_payload_recursive("tasktest all");
    case TASKTEST_TRANSPORT_TEST_MAX_FRAME:
        return TASKTEST_TRANSPORT_FRAME_MAX < SHELL_CMD_BUFFER_SIZE;
    case TASKTEST_TRANSPORT_TEST_HARNESS_FIT:
    case TASKTEST_TRANSPORT_TEST_HARNESS_LINE_FENCED:
    case TASKTEST_TRANSPORT_TEST_HARNESS_END_MAX:
    {
        tasktest_harness_record_t record;
        harness_record_begin(&record, "END");
        harness_record_text(&record, " seq=");
        harness_record_u64(&record, UINT64_MAX);
        harness_record_text(&record, " status=");
        harness_record_status(&record, INT32_MIN);
        return harness_record_finish(&record) &&
               harness_record_complete(&record);
    }
    case TASKTEST_TRANSPORT_TEST_HARNESS_REJECT_MAX:
    {
        tasktest_harness_record_t record;
        harness_record_begin(&record, "FRAME");
        harness_record_text(&record, " REJECT seq=");
        harness_record_u64(&record, UINT64_MAX);
        harness_record_text(&record,
            " reason=runtime-not-ready expected_seq=");
        harness_record_u64(&record, UINT64_MAX);
        return harness_record_finish(&record) &&
               harness_record_complete(&record);
    }
    case TASKTEST_TRANSPORT_TEST_HARNESS_STATUS_MAX:
    {
        tasktest_harness_record_t record;
        harness_record_begin(&record, "STATUS");
        const char *fields[] = {" accepted=", " completed=", " crc_rejects=",
            " sequence_rejects=", " syntax_rejects=",
            " recursive_rejects="};
        for (uint32_t i = 0; i < 6u; i++) {
            harness_record_text(&record, fields[i]);
            harness_record_u64(&record, UINT64_MAX);
        }
        return harness_record_finish(&record) &&
               harness_record_complete(&record);
    }
    case TASKTEST_TRANSPORT_TEST_PS_LOOP_BOUNDS:
        return tasktest_ps_loop_count_valid(1u) &&
               tasktest_ps_loop_count_valid(500u) &&
               !tasktest_ps_loop_count_valid(0u) &&
               !tasktest_ps_loop_count_valid(501u);
    case TASKTEST_TRANSPORT_TEST_PS_LOOP_RECORD_FIT:
    {
        tasktest_harness_record_t record;
        return tasktest_build_ps_loop_record(
            &record, false, false, 500u, 499u, 500u, INT32_MIN);
    }
    default:
        return false;
    }
}

static bool tasktest_known_suite(const char *name)
{
    for (uint32_t i = 0;
         i < (uint32_t)(sizeof(g_tasktest_suites) /
                        sizeof(g_tasktest_suites[0])); i++)
        if (!strcmp(name, g_tasktest_suites[i]))
            return true;
    return false;
}
#endif

int cmd_tasktest(int argc, char **argv)
{
#ifndef HOBBYOS_SELFTEST
    (void)argc;
    (void)argv;
    tasktest_both("tasktest: selftest build is disabled\n");
    return 69;
#else
    if (argc >= 2 && !strcmp(argv[1], "exec"))
        return tasktest_transport_exec(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "ps-loop"))
        return tasktest_ps_loop(argc, argv);

    if (argc > 2)
    {
        tasktest_both("Usage: tasktest [list|summary|suite|all]\n");
        return 64;
    }

    const char *action = argc == 1 ? "all" : argv[1];
    if (!strcmp(action, "list"))
    {
        tasktest_both("Selftest suites: identity runtime scheduler sync accounting kill reaper input modal ui format registry transport all\n");
        return 0;
    }
    if (!strcmp(action, "transport-status"))
        return tasktest_transport_status();
    if (!strcmp(action, "readiness-status"))
        return tasktest_readiness_status();
    if (!strcmp(action, "transport-selftest"))
    {
        selftest_summary_t summary;
        return selftest_run_suite("transport", &summary) ? 0 : 1;
    }
    if (!strcmp(action, "summary"))
    {
        selftest_summary_t summary;
        selftest_last_summary(&summary);
        selftest_emit_summary(&summary);
        return summary.fail ? 1 : 0;
    }
    if (!tasktest_known_suite(action))
    {
        tasktest_both("tasktest: unknown suite\n");
        tasktest_both("Usage: tasktest [list|summary|suite|all]\n");
        return 64;
    }

    selftest_summary_t summary;
    bool ok = !strcmp(action, "all") ? selftest_run_all(&summary) :
                                       selftest_run_suite(action, &summary);
    return ok ? 0 : 1;
#endif
}
