#include "shell.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "commands/registry.h"
#include "../drivers/keyboard.h"
#include "../drivers/usb/xhci/xhci.h"
#include "../core/spinlock.h"

static char g_buffer[SHELL_CMD_BUFFER_SIZE];
static int g_len = 0;
static int g_pos = 0;
static bool g_cursor_visible = true;
static bool g_shell_active = false;
static char g_cursor_underlying = ' ';

static spinlock_t g_shell_lock;

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
static void shell_execute_command_line(const char *line);
static void shell_insert_char(char c);
static void shell_backspace();
static void shell_delete();
static void shell_move_left();
static void shell_move_right();
static void shell_history_push(const char *line);
static const char *shell_history_get_by_age(int age);
static void shell_replace_input_state(const char *src, int len, int pos);

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

static void shell_execute_command_line(const char *line)
{
    if (!line || !line[0])
    {
        return;
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
        return;
    }

    const ShellCommand *cmd = shell_registry_find(argv[0]);
    if (!cmd || !cmd->handler)
    {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("Command '");
        console_write(original_line);
        console_write("' not recognized.\n");
        return;
    }

    cmd->handler(argc, argv);
}

void shell_init()
{
    spinlock_init(&g_shell_lock);

    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);
    g_shell_active = true;
    shell_reset_buffer();
    spin_unlock_irqrestore(&g_shell_lock, flags);
}

void shell_on_tick()
{
    if (!g_shell_active)
        return;

    static int tick = 0;
    tick++;
    if ((tick % 40) == 0)
    {
        irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);

        if (g_shell_active)
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
    if (!g_shell_active)
        return;

    console_begin_batch();

    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);

    if (!g_shell_active)
    {
        spin_unlock_irqrestore(&g_shell_lock, flags);
        console_end_batch();
        return;
    }

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
            shell_execute_command_line(line);
        }

        flags = spin_lock_irqsave(&g_shell_lock);
        g_shell_active = true;
        shell_reset_buffer();
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
    if (!g_shell_active)
        return;

    console_begin_batch();

    irq_flags_t flags = spin_lock_irqsave(&g_shell_lock);

    if (!g_shell_active)
    {
        spin_unlock_irqrestore(&g_shell_lock, flags);
        console_end_batch();
        return;
    }

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

void shell_refresh_view()
{
}