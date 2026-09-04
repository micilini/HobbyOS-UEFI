#include "shell.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "commands/registry.h"
#include "../drivers/keyboard.h"
#include "../drivers/serial.h"
#include "../drivers/usb/xhci/xhci.h"
#include "../core/spinlock.h"
#include "../core/input_router.h"
#include "../core/input_event.h"

static char g_buffer[SHELL_CMD_BUFFER_SIZE];
static int g_len = 0;
static int g_pos = 0;
static bool g_cursor_visible = true;
static bool g_shell_active = false;
static volatile uint8_t g_shell_initialized;
static volatile uint8_t g_shell_thread_started;
static char g_cursor_underlying = ' ';
static volatile uint8_t g_shell_input_paused_for_modal_ui;
static volatile uint64_t g_shell_modal_pause_drops;
static volatile uint8_t g_shell_test_pause_hook_armed;
static volatile uint8_t g_shell_test_pause_hook_entered;
static volatile uint8_t g_shell_test_pause_hook_release;

static bool shell_modal_pause_load(void)
{
    return __atomic_load_n(&g_shell_input_paused_for_modal_ui,
                           __ATOMIC_ACQUIRE) != 0;
}

static void shell_modal_pause_store(bool paused)
{
    __atomic_store_n(&g_shell_input_paused_for_modal_ui,
                     paused ? 1u : 0u, __ATOMIC_RELEASE);
}

static bool shell_consume_routed_event(input_event_t ev, bool apply_to_shell)
{
    if (__atomic_load_n(&g_shell_test_pause_hook_armed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_shell_test_pause_hook_entered, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_shell_test_pause_hook_release,
                                __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
        __atomic_store_n(&g_shell_test_pause_hook_armed, 0, __ATOMIC_RELEASE);
    }
    if (shell_modal_pause_load()) {
        __atomic_add_fetch(&g_shell_modal_pause_drops, 1,
                           __ATOMIC_RELAXED);
        return false;
    }
    if (apply_to_shell) {
        if (ev.type == INPUT_EVENT_CHAR)
            shell_receive_char((char)ev.value);
        else if (ev.type == INPUT_EVENT_SPECIAL)
            shell_receive_special(ev.value);
    }
    return true;
}

static spinlock_t g_shell_lock;
static shell_status_snapshot_t g_shell_status;

_Static_assert(SHELL_STATUS_MESSAGE_MAX == CONSOLE_STATUS_MESSAGE_MAX,
               "shell and console status bounds must match");

static bool g_limit_banner = false;

static void shell_set_limit_banner(bool on);

#define SHELL_COLOR_ACCENT 0xFF00FFFF
const char *PROMPT = "HobbyOS> ";

#define SHELL_HISTORY_MAX 32

static uint32_t g_input_origin_x = 0;
static uint32_t g_input_origin_y = 0;

static char g_history[SHELL_HISTORY_MAX][SHELL_CMD_BUFFER_SIZE];
static int g_hist_count = 0;
static int g_hist_head = 0;

static int g_hist_nav = -1;

static char g_hist_draft[SHELL_CMD_BUFFER_SIZE];
static int g_hist_draft_len = 0;
static int g_hist_draft_pos = 0;

static void shell_hide_cursor();
static void shell_show_cursor();
static void shell_reset_buffer();
static int shell_dispatch_command_line(const char *line,
                                       bool console_unknown);
static void shell_insert_char(char c);
static void shell_backspace();
static void shell_delete();
static void shell_move_left();
static void shell_move_right();
static void shell_history_push(const char *line);
static const char *shell_history_get_by_age(int age);
static void shell_replace_input_state(const char *src, int len, int pos);
static void shell_status_flush_locked(void);
static void shell_status_clear_locked(bool count_clear);

static void shell_strncpy0(char *dst, const char *src, int dst_size)
{
    if (!dst || dst_size <= 0)
        return;

    int i = 0;
    if (src)
    {
        for (; i < dst_size - 1 && src[i]; i++)
        {
            dst[i] = src[i];
        }
    }
    dst[i] = 0;
}

static void shell_status_copy_locked(const char *message)
{
    uint32_t length = 0;
    if (!message)
        message = "";
    while (message[length] && length < SHELL_STATUS_MESSAGE_MAX - 1u)
    {
        char value = message[length];
        if (value == '\n' || value == '\r' || value == '\t')
            value = ' ';
        g_shell_status.message[length++] = value;
    }
    if (message[length] && length >= 3u)
    {
        g_shell_status.message[length - 3u] = '.';
        g_shell_status.message[length - 2u] = '.';
        g_shell_status.message[length - 1u] = '.';
    }
    g_shell_status.message[length] = '\0';
}

static void shell_status_show_locked(bool flushed)
{
    if (!g_shell_status.message[0] || !g_shell_active ||
        shell_modal_pause_load())
        return;
    console_status_set(g_shell_status.message);
    g_shell_status.pending = 0;
    g_shell_status.visible = 1;
    if (flushed)
        g_shell_status.flushed++;
}

static void shell_status_flush_locked(void)
{
    if (g_shell_status.pending)
        shell_status_show_locked(true);
}

static void shell_status_clear_locked(bool count_clear)
{
    bool had_status = g_shell_status.pending || g_shell_status.visible;
    if (g_shell_status.visible)
        console_status_clear();
    g_shell_status.pending = 0;
    g_shell_status.visible = 0;
    g_shell_status.message[0] = '\0';
    if (had_status && count_clear)
        g_shell_status.cleared++;
}

static void shell_status_defer_visible_locked(void)
{
    if (!g_shell_status.visible)
        return;
    console_status_clear();
    g_shell_status.visible = 0;
    g_shell_status.pending = g_shell_status.message[0] ? 1u : 0u;
    if (g_shell_status.pending)
        g_shell_status.deferred++;
}

static uint64_t shell_status_publish_new_locked(const char *message,
                                                bool terminal)
{
    uint64_t generation = g_shell_status.generation + 1u;
    if (!generation)
        generation = 1u;
    if (g_shell_status.pending || g_shell_status.visible)
        g_shell_status.replaced++;
    g_shell_status.generation = generation;
    g_shell_status.terminal = terminal ? 1u : 0u;
    shell_status_copy_locked(message);
    g_shell_status.published++;
    if (g_shell_active && !shell_modal_pause_load())
        shell_status_show_locked(false);
    else
    {
        g_shell_status.pending = 1;
        g_shell_status.visible = 0;
        g_shell_status.deferred++;
    }
    return generation;
}

uint64_t shell_status_begin(const char *message)
{
    if (!message || !message[0])
        return 0;
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    uint64_t generation = shell_status_publish_new_locked(message, false);
    spin_unlock_irqrestore(&g_shell_lock, flags);
    return generation;
}

bool shell_status_complete(uint64_t generation, const char *message)
{
    if (!generation || !message || !message[0])
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    if (generation != g_shell_status.generation || g_shell_status.terminal)
    {
        spin_unlock_irqrestore(&g_shell_lock, flags);
        return false;
    }
    if (g_shell_status.pending || g_shell_status.visible)
        g_shell_status.replaced++;
    shell_status_copy_locked(message);
    g_shell_status.terminal = 1;
    g_shell_status.published++;
    if (g_shell_active && !shell_modal_pause_load())
        shell_status_show_locked(false);
    else
    {
        g_shell_status.pending = 1;
        g_shell_status.visible = 0;
        g_shell_status.deferred++;
    }
    spin_unlock_irqrestore(&g_shell_lock, flags);
    return true;
}

void shell_clear_status_on_input(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    shell_status_clear_locked(true);
    spin_unlock_irqrestore(&g_shell_lock, flags);
}

bool shell_status_snapshot(shell_status_snapshot_t *out)
{
    if (!out)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    *out = g_shell_status;
    spin_unlock_irqrestore(&g_shell_lock, flags);
    return true;
}

#ifdef HOBBYOS_SELFTEST
void shell_test_status_reset(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    shell_status_clear_locked(false);
    memset(&g_shell_status, 0, sizeof(g_shell_status));
    spin_unlock_irqrestore(&g_shell_lock, flags);
}

void shell_test_status_set_runtime(bool active, bool modal_paused)
{
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    if (modal_paused)
        shell_status_defer_visible_locked();
    g_shell_active = active;
    shell_modal_pause_store(modal_paused);
    if (active && !modal_paused)
        shell_status_flush_locked();
    spin_unlock_irqrestore(&g_shell_lock, flags);
}

bool shell_test_status_set_input(const char *text, int position)
{
    if (!text)
        return false;
    int length = (int)strlen(text);
    if (length >= SHELL_CMD_BUFFER_SIZE || position < 0 || position > length)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    memset(g_buffer, 0, sizeof(g_buffer));
    memcpy(g_buffer, text, (uint32_t)length);
    g_len = length;
    g_pos = position;
    spin_unlock_irqrestore(&g_shell_lock, flags);
    return true;
}

bool shell_test_status_input_equals(const char *text, int position)
{
    if (!text)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    bool equal = g_pos == position && g_len == (int)strlen(text) &&
                 strcmp(g_buffer, text) == 0;
    spin_unlock_irqrestore(&g_shell_lock, flags);
    return equal;
}
#endif

static void shell_set_limit_banner(bool on)
{
    g_limit_banner = on;
}

static void shell_history_push(const char *line)
{
    if (!line || !line[0])
        return;

    if (g_hist_count > 0)
    {
        int last = (g_hist_head - 1 + SHELL_HISTORY_MAX) % SHELL_HISTORY_MAX;
        if (strcmp(g_history[last], line) == 0)
            return;
    }

    shell_strncpy0(g_history[g_hist_head], line, SHELL_CMD_BUFFER_SIZE);

    g_hist_head = (g_hist_head + 1) % SHELL_HISTORY_MAX;
    if (g_hist_count < SHELL_HISTORY_MAX)
        g_hist_count++;
}

static const char *shell_history_get_by_age(int age)
{

    if (age < 0 || age >= g_hist_count)
        return NULL;

    int idx = (g_hist_head - 1 - age + SHELL_HISTORY_MAX) % SHELL_HISTORY_MAX;
    return g_history[idx];
}

static void shell_hide_cursor()
{
    if (!g_cursor_visible)
        return;

    char under = (g_pos < g_len) ? g_buffer[g_pos] : ' ';
    g_cursor_underlying = under;

    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_redraw_char(under);

    g_cursor_visible = false;
}

static void shell_show_cursor()
{

    if (g_pos < g_len)
        g_cursor_underlying = g_buffer[g_pos];
    else
        g_cursor_underlying = ' ';

    console_draw_cursor(g_cursor_underlying);

    g_cursor_visible = true;
}

static void shell_reset_buffer()
{

    shell_set_limit_banner(false);

    memset(g_buffer, 0, SHELL_CMD_BUFFER_SIZE);
    g_len = 0;
    g_pos = 0;

    g_hist_nav = -1;
    g_hist_draft_len = 0;
    g_hist_draft_pos = 0;
    g_hist_draft[0] = 0;

    console_set_color(SHELL_COLOR_ACCENT, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write(PROMPT);
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);

    console_get_cursor(&g_input_origin_x, &g_input_origin_y);

    shell_show_cursor();
}

static void shell_replace_input_state(const char *src, int len, int pos)
{
    shell_hide_cursor();

    console_set_cursor(g_input_origin_x, g_input_origin_y);

    int old_len = g_len;
    for (int i = 0; i < old_len; i++)
    {
        console_put_char(' ');
    }

    console_set_cursor(g_input_origin_x, g_input_origin_y);

    memset(g_buffer, 0, SHELL_CMD_BUFFER_SIZE);

    if (!src)
    {
        g_len = 0;
        g_pos = 0;
        shell_show_cursor();
        return;
    }

    int n = len;
    if (n < 0)
        n = (int)strlen(src);
    if (n > SHELL_CMD_BUFFER_SIZE - 1)
        n = SHELL_CMD_BUFFER_SIZE - 1;

    memcpy(g_buffer, src, n);
    g_buffer[n] = 0;
    g_len = n;

    if (pos < 0)
        pos = g_len;
    if (pos > g_len)
        pos = g_len;
    g_pos = pos;

    console_write(g_buffer);

    console_set_cursor(g_input_origin_x, g_input_origin_y);
    for (int i = 0; i < g_pos; i++)
    {
        console_move_right();
    }

    shell_show_cursor();
}

static void shell_insert_char(char c)
{
    if (g_len >= SHELL_CMD_BUFFER_SIZE - 1)
    {
        shell_set_limit_banner(true);
        return;
    }

    shell_set_limit_banner(false);

    shell_hide_cursor();

    if (g_pos == g_len)
    {
        g_buffer[g_pos] = c;
        g_pos++;
        g_len++;
        g_buffer[g_len] = 0;
        console_put_char(c);
    }
    else
    {

        memmove(&g_buffer[g_pos + 1], &g_buffer[g_pos], g_len - g_pos);
        g_buffer[g_pos] = c;
        g_len++;
        g_buffer[g_len] = 0;

        console_write(&g_buffer[g_pos]);
        g_pos++;

        for (int i = 0; i < (g_len - g_pos); i++)
            console_move_left();
    }

    shell_show_cursor();
}

static void shell_backspace()
{
    if (g_pos == 0)
        return;

    shell_set_limit_banner(false);
    shell_hide_cursor();

    if (g_pos == g_len)
    {

        g_pos--;
        g_len--;
        g_buffer[g_len] = 0;
        console_backspace();
    }
    else
    {

        memmove(&g_buffer[g_pos - 1], &g_buffer[g_pos], g_len - g_pos);
        g_len--;
        g_pos--;

        g_buffer[g_len] = 0;

        console_backspace();
        console_write(&g_buffer[g_pos]);

        console_put_char(' ');
        console_move_left();

        for (int i = 0; i < (g_len - g_pos); i++)
            console_move_left();
    }

    shell_show_cursor();
}

static void shell_delete()
{
    if (g_pos >= g_len)
        return;

    shell_set_limit_banner(false);

    shell_hide_cursor();

    memmove(&g_buffer[g_pos], &g_buffer[g_pos + 1], g_len - g_pos - 1);
    g_len--;
    g_buffer[g_len] = 0;

    console_write(&g_buffer[g_pos]);
    console_put_char(' ');
    console_move_left();

    for (int i = 0; i < (g_len - g_pos); i++)
        console_move_left();

    shell_show_cursor();
}

static void shell_move_left()
{
    if (g_pos == 0)
        return;

    shell_hide_cursor();
    console_move_left();
    g_pos--;
    shell_show_cursor();
}

static void shell_move_right()
{
    if (g_pos >= g_len)
        return;

    shell_hide_cursor();
    console_move_right();
    g_pos++;
    shell_show_cursor();
}

static int shell_parse_args_inplace(char *line, char **argv, int max_argv)
{

    int argc = 0;
    char *p = line;

    while (*p == ' ' || *p == '\t')
        p++;

    while (*p && argc < max_argv)
    {
        if (*p == '"')
        {

            p++;
            argv[argc++] = p;

            while (*p && *p != '"')
                p++;
            if (*p == '"')
            {
                *p = 0;
                p++;
            }
        }
        else
        {

            argv[argc++] = p;

            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
            {
                *p = 0;
                p++;
            }
        }

        while (*p == ' ' || *p == '\t')
            p++;
    }

    return argc;
}

static int shell_dispatch_command_line(const char *line,
                                       bool console_unknown)
{
    if (!line || !line[0])
    {
        return 64;
    }

    char work[SHELL_CMD_BUFFER_SIZE];
    char original_line[SHELL_CMD_BUFFER_SIZE];

    memset(work, 0, sizeof(work));
    memset(original_line, 0, sizeof(original_line));

    int n = (int)strlen(line);
    if (n > SHELL_CMD_BUFFER_SIZE - 1)
        n = SHELL_CMD_BUFFER_SIZE - 1;

    for (int i = 0; i < n; i++)
    {
        work[i] = line[i];
        original_line[i] = line[i];
    }
    work[n] = 0;
    original_line[n] = 0;

    char *argv[16];
    int argc = shell_parse_args_inplace(work, argv, 16);

    if (argc <= 0)
    {
        return 64;
    }

    const ShellCommand *cmd = shell_registry_find(argv[0]);
    if (!cmd || !cmd->handler)
    {
        if (console_unknown)
        {
            console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_HOBBYOS_BLUE);
            console_write("Command '");
            console_write(original_line);
            console_write("' not recognized.\n");
        }
        return 127;
    }

    return cmd->handler(argc, argv);
}

#ifdef HOBBYOS_SELFTEST
int shell_execute_command_line_for_selftest(const char *line)
{
    return shell_dispatch_command_line(line, false);
}
#endif

void shell_thread_entry(void *arg);


void shell_init()
{
    __atomic_store_n(&g_shell_initialized, 0, __ATOMIC_RELEASE);
    spinlock_init(&g_shell_lock);

    shell_modal_pause_store(false);
    __atomic_store_n(&g_shell_modal_pause_drops, 0, __ATOMIC_RELEASE);

    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    memset(&g_shell_status, 0, sizeof(g_shell_status));
    g_shell_active = true;
    shell_reset_buffer();
    spin_unlock_irqrestore(&g_shell_lock, flags);
    __atomic_store_n(&g_shell_initialized, 1, __ATOMIC_RELEASE);
}

void shell_on_tick()
{
    static int tick = 0;
    tick++;
    if ((tick % 40) == 0)
    {

        irq_flags_t flags;
        if (!spin_trylock_irqsave(&g_shell_lock, &flags))
            return;

        if (g_shell_active && !shell_modal_pause_load())
        {
            if (g_cursor_visible)
                shell_hide_cursor();
            else
                shell_show_cursor();
        }

        spin_unlock_irqrestore(&g_shell_lock, flags);
    }
}

void shell_receive_char(char c)
{
    console_begin_batch();

    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);

    if (!g_shell_active || shell_modal_pause_load())
    {
        spin_unlock_irqrestore(&g_shell_lock, flags);
        console_end_batch();
        return;
    }

    shell_status_clear_locked(true);

    if (g_hist_nav != -1)
    {
        g_hist_nav = -1;
        g_hist_draft_len = g_len;
        g_hist_draft_pos = g_pos;
        shell_strncpy0(g_hist_draft, g_buffer, SHELL_CMD_BUFFER_SIZE);
    }

    if (c == '\n' || c == '\r')
    {
        char line[SHELL_CMD_BUFFER_SIZE];
        int n = g_len;

        if (n < 0)
            n = 0;
        if (n > SHELL_CMD_BUFFER_SIZE - 1)
            n = SHELL_CMD_BUFFER_SIZE - 1;

        for (int i = 0; i < n; i++)
        {
            line[i] = g_buffer[i];
        }
        line[n] = 0;

        shell_hide_cursor();
        console_put_char('\n');

        if (n > 0)
        {
            shell_history_push(line);
        }

        g_hist_nav = -1;

        g_shell_active = false;

        spin_unlock_irqrestore(&g_shell_lock, flags);

        if (n > 0)
        {
            (void)shell_dispatch_command_line(line, true);
        }

        flags = spin_lock_irqsave(&g_shell_lock);
        g_shell_active = true;
        shell_reset_buffer();
        shell_status_flush_locked();
        spin_unlock_irqrestore(&g_shell_lock, flags);

        console_end_batch();
        return;
    }

    if (c == '\b')
    {
        shell_backspace();
        goto end_batch;
    }

    if (c >= 32 && c <= 126)
    {
        shell_insert_char(c);

        if (g_hist_nav == -1)
        {
            g_hist_draft_len = g_len;
            g_hist_draft_pos = g_pos;
            shell_strncpy0(g_hist_draft, g_buffer, SHELL_CMD_BUFFER_SIZE);
        }
    }

end_batch:
    shell_show_cursor();
    spin_unlock_irqrestore(&g_shell_lock, flags);
    console_end_batch();
}

void shell_receive_special(uint8_t key)
{
    console_begin_batch();

    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);

    if (!g_shell_active || shell_modal_pause_load())
    {
        spin_unlock_irqrestore(&g_shell_lock, flags);
        console_end_batch();
        return;
    }

    shell_status_clear_locked(true);

    shell_hide_cursor();

    if (key == KEY_SPECIAL_LEFT)
    {
        shell_move_left();
    }
    else if (key == KEY_SPECIAL_RIGHT)
    {
        shell_move_right();
    }
    else if (key == KEY_SPECIAL_UP)
    {
        if (g_hist_count > 0)
        {
            if (g_hist_nav == -1)
            {
                g_hist_draft_len = g_len;
                g_hist_draft_pos = g_pos;
                shell_strncpy0(g_hist_draft, g_buffer, SHELL_CMD_BUFFER_SIZE);
            }

            if (g_hist_nav < g_hist_count - 1)
            {
                g_hist_nav++;
                const char *cmd = shell_history_get_by_age(g_hist_nav);
                if (cmd)
                {
                    shell_replace_input_state(cmd, -1, -1);
                }
            }
        }
    }
    else if (key == KEY_SPECIAL_DOWN)
    {
        if (g_hist_count > 0 && g_hist_nav != -1)
        {
            if (g_hist_nav > 0)
            {
                g_hist_nav--;
                const char *cmd = shell_history_get_by_age(g_hist_nav);
                if (cmd)
                {
                    shell_replace_input_state(cmd, -1, -1);
                }
            }
            else if (g_hist_nav == 0)
            {
                g_hist_nav = -1;
                shell_replace_input_state(g_hist_draft, g_hist_draft_len, g_hist_draft_pos);
            }
        }
    }

    shell_show_cursor();

    spin_unlock_irqrestore(&g_shell_lock, flags);
    console_end_batch();
}

void shell_pause_input_for_modal_ui(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);

    shell_status_defer_visible_locked();
    shell_modal_pause_store(true);

    if (g_shell_active && g_cursor_visible)
        shell_hide_cursor();

    spin_unlock_irqrestore(&g_shell_lock, flags);
}

void shell_resume_input_from_modal_ui(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);

    shell_modal_pause_store(false);

    if (g_shell_active)
    {
        shell_show_cursor();
        shell_status_flush_locked();
    }

    spin_unlock_irqrestore(&g_shell_lock, flags);
}

void shell_thread_entry(void *arg)
{
    (void)arg;

    __atomic_store_n(&g_shell_thread_started, 1, __ATOMIC_RELEASE);

    serial_write_all("[SHELL] Shell thread started (waiting for events)\n");

    while (1)
    {
        input_event_t ev = input_router_default_wait();


        (void)shell_consume_routed_event(ev, true);
    }
}

bool shell_runtime_snapshot(shell_runtime_snapshot_t *out)
{
    if (!out) return false;
    out->initialized = __atomic_load_n(&g_shell_initialized, __ATOMIC_ACQUIRE);
    out->thread_started = __atomic_load_n(&g_shell_thread_started,
                                          __ATOMIC_ACQUIRE);
    out->active = 0;
    out->modal_paused = shell_modal_pause_load() ? 1u : 0u;
    if (!out->initialized) return true;
    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    out->active = g_shell_active ? 1u : 0u;
    spin_unlock_irqrestore(&g_shell_lock, flags);
    return true;
}

bool shell_is_input_paused_for_modal_ui(void)
{
    return shell_modal_pause_load();
}

uint64_t shell_modal_pause_drop_count(void)
{
    return __atomic_load_n(&g_shell_modal_pause_drops, __ATOMIC_ACQUIRE);
}

void shell_test_pause_boundary_arm(void)
{
    __atomic_store_n(&g_shell_test_pause_hook_entered, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_shell_test_pause_hook_release, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_shell_test_pause_hook_armed, 1, __ATOMIC_RELEASE);
}

bool shell_test_pause_boundary_entered(void)
{
    return __atomic_load_n(&g_shell_test_pause_hook_entered,
                           __ATOMIC_ACQUIRE) != 0;
}

void shell_test_pause_boundary_release(void)
{
    __atomic_store_n(&g_shell_test_pause_hook_release, 1, __ATOMIC_RELEASE);
}

bool shell_test_consume_default_event(bool apply_to_shell)
{
    input_event_t event = input_router_default_wait();
    return shell_consume_routed_event(event, apply_to_shell);
}

void shell_refresh_view()
{
}
