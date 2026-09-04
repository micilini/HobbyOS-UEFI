#include "console.h"
#include "graphics.h"
#include "../utils/utils.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../core/spinlock.h"
#include "../drivers/serial.h"

extern volatile int g_panic_in_progress;

static Psf1_Font *g_font = NULL;
static uint8_t g_early_clear_reported;

volatile uint8_t g_console_debug_enabled = 1;

static spinlock_t g_console_lock;

static ConsoleCell g_history[CONSOLE_MAX_HISTORY][CONSOLE_MAX_COLS_STORAGE];

static uint32_t g_view_start_line = 0;

static uint32_t g_cursor_x = 0;
static uint32_t g_cursor_y = 0;

static uint32_t g_color_fg = CONSOLE_COLOR_WHITE;
static uint32_t g_color_bg = CONSOLE_COLOR_HOBBYOS_BLUE;

static int g_console_batch = 0;
static console_region_stats_t g_region_stats;
static bool g_status_visible;
static uint32_t g_status_row;
static uint32_t g_status_bg;
static char g_status_message[CONSOLE_STATUS_MESSAGE_MAX];

static void console_status_repaint_internal(void);
static void console_status_restore_internal(void);

static void console_begin_batch_internal()
{
    g_console_batch++;
}

static void console_end_batch_internal()
{
    if (g_console_batch > 0)
        g_console_batch--;
    if (g_console_batch == 0)
        swap_buffers();
}

static volatile uint8_t g_console_render_suspended = 0;

void console_set_render_suspended(uint8_t suspended)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    g_console_render_suspended = suspended ? 1 : 0;
    spin_unlock_irqrestore(&g_console_lock, flags);
}

uint8_t console_is_render_suspended(void)
{
    return g_console_render_suspended;
}

void console_begin_batch()
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    console_begin_batch_internal();
    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_end_batch()
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    console_end_batch_internal();
    spin_unlock_irqrestore(&g_console_lock, flags);
}

static inline void console_flush_if_needed()
{
    if (g_console_render_suspended)
        return;
    if (g_console_batch == 0)
        swap_buffers();
}

static uint32_t g_max_cols = 0;
static uint32_t g_max_rows = 0;

static void compute_max_dimensions(void)
{
    Framebuffer *fb = graphics_framebuffer();
    if (!fb || !g_font || !g_font->psf_header)
        return;

    uint32_t fb_width = fb->Width;
    if (fb->PixelsPerScanLine && fb->PixelsPerScanLine < fb_width)
    {
        fb_width = fb->PixelsPerScanLine;
    }

    uint32_t fb_height = fb->Height;

    uint32_t usable_width = (fb_width > (CONSOLE_PAD_X * 2u)) ? (fb_width - (CONSOLE_PAD_X * 2u)) : fb_width;
    uint32_t usable_height = (fb_height > (CONSOLE_PAD_Y * 2u)) ? (fb_height - (CONSOLE_PAD_Y * 2u)) : fb_height;

    uint32_t max_cols = usable_width / 8u;
    uint32_t max_rows = usable_height / (uint32_t)g_font->psf_header->charsize;

    if (max_cols == 0)
        max_cols = 1;
    if (max_rows == 0)
        max_rows = 1;

    if (max_cols > CONSOLE_MAX_COLS_STORAGE)
        max_cols = CONSOLE_MAX_COLS_STORAGE;

    g_max_cols = max_cols;
    g_max_rows = max_rows;
}

static void draw_char_pixels_at(char c, uint32_t cx, uint32_t cy, uint32_t fg_color)
{

    if (g_console_render_suspended)
        return;

    if (!g_font)
        return;

    uint32_t char_height = g_font->psf_header->charsize;

    char *fontPtr = (char *)g_font->glyph_buffer + (c * char_height);

    uint32_t cx_pixel = CONSOLE_PAD_X + (cx * 8);
    uint32_t cy_pixel = CONSOLE_PAD_Y + (cy * char_height);

    for (uint32_t py = 0; py < char_height; py++)
    {
        char row = *fontPtr;

        for (uint32_t px = 0; px < 8; px++)
        {
            if (row & (0x80 >> px))
            {
                put_pixel(cx_pixel + px, cy_pixel + py, fg_color);
            }
            else
            {

                put_pixel(cx_pixel + px, cy_pixel + py, g_color_bg);
            }
        }
        fontPtr++;
    }
}

static void console_scroll()
{

    g_region_stats.scroll_count++;

    g_view_start_line = (g_view_start_line + 1) % CONSOLE_MAX_HISTORY;

    uint32_t bottom_line = (g_view_start_line + g_max_rows - 1) % CONSOLE_MAX_HISTORY;
    for (uint32_t x = 0; x < g_max_cols; x++)
    {
        g_history[bottom_line][x].c = 0;
        g_history[bottom_line][x].fg = g_color_fg;
        g_history[bottom_line][x].bg = g_color_bg;
    }

    if (!g_console_render_suspended)
    {
        for (uint32_t screen_y = 0; screen_y < g_max_rows; screen_y++)
        {
            uint32_t history_idx = (g_view_start_line + screen_y) % CONSOLE_MAX_HISTORY;
            uint32_t cy_pixel = CONSOLE_PAD_Y + (screen_y * g_font->psf_header->charsize);

            for (uint32_t x = 0; x < g_max_cols; x++)
            {
                ConsoleCell *cell = &g_history[history_idx][x];

                char c = cell->c;
                if (c == 0)
                    c = ' ';

                uint32_t cx_pixel = CONSOLE_PAD_X + (x * 8);
                char *fontPtr = (char *)g_font->glyph_buffer + (c * g_font->psf_header->charsize);

                for (uint32_t py = 0; py < g_font->psf_header->charsize; py++)
                {
                    char row = *fontPtr;
                    for (uint32_t px = 0; px < 8; px++)
                    {
                        if (row & (0x80 >> px))
                        {
                            put_pixel(cx_pixel + px, cy_pixel + py, cell->fg);
                        }
                        else
                        {
                            put_pixel(cx_pixel + px, cy_pixel + py, cell->bg);
                        }
                    }
                    fontPtr++;
                }
            }
        }
    }

    if (g_max_rows > 0)
    {
        g_cursor_y = g_max_rows - 1;
    }
    else
    {
        g_cursor_y = 0;
    }
    if (g_cursor_x >= g_max_cols)
    {
        g_cursor_x = 0;
    }

    if (g_status_visible)
    {
        g_status_row = (g_cursor_y == (g_max_rows - 1) && g_max_rows > 1)
            ? (g_max_rows - 2) : (g_max_rows - 1);
        console_status_repaint_internal();
    }

    console_flush_if_needed();
}

void console_init(BootInfo *boot_info)
{
    spinlock_init(&g_console_lock);

    if (!boot_info ||
        !graphics_bind_framebuffer(boot_info->framebuffer, NULL))
        return;
    g_font = boot_info->font;
    g_early_clear_reported = 0;
    g_status_visible = false;
    g_status_row = 0;
    g_status_bg = g_color_bg;
    g_status_message[0] = '\0';

    compute_max_dimensions();

    for (int y = 0; y < CONSOLE_MAX_HISTORY; y++)
    {
        for (int x = 0; x < CONSOLE_MAX_COLS_STORAGE; x++)
        {
            g_history[y][x].c = 0;
            g_history[y][x].fg = g_color_fg;
            g_history[y][x].bg = g_color_bg;
        }
    }
}

static void console_render_full_internal()
{
    if (!g_font || !graphics_framebuffer_is_bound())
        return;

    clear_screen(g_color_bg);

    for (uint32_t screen_y = 0; screen_y < g_max_rows; screen_y++)
    {
        uint32_t history_idx = (g_view_start_line + screen_y) % CONSOLE_MAX_HISTORY;

        for (uint32_t x = 0; x < g_max_cols; x++)
        {
            ConsoleCell *cell = &g_history[history_idx][x];
            char c = cell->c;
            if (c == 0)
                c = ' ';

            uint32_t cx_pixel = CONSOLE_PAD_X + (x * 8);
            uint32_t cy_pixel = CONSOLE_PAD_Y + (screen_y * g_font->psf_header->charsize);

            char *fontPtr = (char *)g_font->glyph_buffer + (c * g_font->psf_header->charsize);

            for (uint32_t py = 0; py < g_font->psf_header->charsize; py++)
            {
                char row = *fontPtr;
                for (uint32_t px = 0; px < 8; px++)
                {
                    if (row & (0x80 >> px))
                    {
                        put_pixel(cx_pixel + px, cy_pixel + py, cell->fg);
                    }
                    else
                    {
                        put_pixel(cx_pixel + px, cy_pixel + py, cell->bg);
                    }
                }
                fontPtr++;
            }
        }
    }

    console_status_repaint_internal();

    console_flush_if_needed();
}

void console_render_full()
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    console_render_full_internal();
    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_clear(uint32_t bg_color)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    g_color_bg = bg_color;
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_status_visible = false;
    g_status_message[0] = '\0';

    g_view_start_line = 0;

    for (int y = 0; y < CONSOLE_MAX_HISTORY; y++)
    {
        for (int x = 0; x < CONSOLE_MAX_COLS_STORAGE; x++)
        {
            g_history[y][x].c = 0;
            g_history[y][x].fg = g_color_fg;
            g_history[y][x].bg = bg_color;
        }
    }

    console_render_full_internal();

    spin_unlock_irqrestore(&g_console_lock, flags);

    Framebuffer *fb = graphics_framebuffer();
    if (!g_early_clear_reported && fb && fb->BaseAddress &&
        ((volatile uint32_t *)fb->BaseAddress)[0] == bg_color) {
        g_early_clear_reported = 1;
        serial_write_all("[GRAPHICS][EARLY_BIND] PASS clear=1 width=");
        console_print_dec_debug(fb->Width);
        serial_write_all(" height=");
        console_print_dec_debug(fb->Height);
        serial_write_all("\n[GRAPHICS][EARLY_CLEAR] PASS\n");
    }
}

static void console_put_char_internal(char c)
{

    if (!g_font || !graphics_framebuffer_is_bound())
        return;

    if (c == '\r')
        return;

    if (c == '\n')
    {
        g_cursor_x = 0;
        g_cursor_y++;
    }
    else
    {
        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;

        if (g_cursor_y >= g_max_rows)
        {
            console_scroll();
            if (g_max_rows > 0)
                g_cursor_y = g_max_rows - 1;
            else
                g_cursor_y = 0;
            actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        }

        if (g_cursor_x >= g_max_cols)
        {
            g_cursor_x = 0;
            g_cursor_y++;
            if (g_cursor_y >= g_max_rows)
            {
                console_scroll();
                if (g_max_rows > 0)
                    g_cursor_y = g_max_rows - 1;
                else
                    g_cursor_y = 0;
            }
            actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        }

        g_history[actual_line][g_cursor_x].c = c;
        g_history[actual_line][g_cursor_x].fg = g_color_fg;
        g_history[actual_line][g_cursor_x].bg = g_color_bg;

        draw_char_pixels_at(c, g_cursor_x, g_cursor_y, g_color_fg);

        g_cursor_x++;
    }

    if (g_cursor_x >= g_max_cols)
    {
        g_cursor_x = 0;
        g_cursor_y++;
    }

    if (g_cursor_y >= g_max_rows)
    {
        console_scroll();
        if (g_max_rows > 0)
            g_cursor_y = g_max_rows - 1;
        else
            g_cursor_y = 0;
    }
}

void console_put_char(char c)
{
    if (g_panic_in_progress)
    {
        console_put_char_internal(c);
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    console_put_char_internal(c);

    if (g_cursor_y < g_max_rows)
    {
        console_flush_if_needed();
    }

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_write(const char *str)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    while (*str)
    {
        console_put_char_internal(*str);
        str++;
    }

    console_flush_if_needed();

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_set_debug_enabled(uint8_t enabled)
{
    g_console_debug_enabled = enabled ? 1 : 0;
}

void console_write_debug(const char *str)
{
    if (!g_console_debug_enabled)
        return;
    serial_write_all(str);
}

void console_print_hex_debug(uint64_t n)
{
    if (!g_console_debug_enabled)
        return;
    serial_write_hex64_all(n);
}

void console_print_dec_debug(uint64_t n)
{
    if (!g_console_debug_enabled)
        return;

    if (n == 0)
    {
        serial_putc_all('0');
        return;
    }

    char buf[21];
    int i = 0;
    while (n > 0 && i < 20)
    {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0)
    {
        serial_putc_all(buf[--i]);
    }
}

void console_set_color_debug(uint32_t fg, uint32_t bg)
{

    (void)fg;
    (void)bg;
}

void console_put_char_debug(char c)
{
    if (!g_console_debug_enabled)
        return;
    serial_putc_all(c);
}

static void console_redraw_char_internal(char c)
{
    if (c == 0)
        c = ' ';
    draw_char_pixels_at(c, g_cursor_x, g_cursor_y, g_color_fg);
    console_flush_if_needed();
}

void console_redraw_char(char c)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    console_redraw_char_internal(c);
    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_backspace()
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (g_cursor_x > 0)
    {
        g_cursor_x--;

        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        g_history[actual_line][g_cursor_x].c = 0;

        console_redraw_char_internal(' ');
    }
    else if (g_cursor_y > 0)
    {
        g_cursor_y--;
        g_cursor_x = g_max_cols - 1;

        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        g_history[actual_line][g_cursor_x].c = 0;

        console_redraw_char_internal(' ');
    }

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_move_left()
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (g_max_cols == 0 || g_max_rows == 0)
    {
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    if (g_cursor_x > 0)
    {
        g_cursor_x--;
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    if (g_cursor_y > 0)
    {
        g_cursor_y--;
        g_cursor_x = g_max_cols - 1;
    }

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_move_right()
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (g_max_cols == 0 || g_max_rows == 0)
    {
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    if (g_cursor_x + 1 < g_max_cols)
    {
        g_cursor_x++;
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    g_cursor_x = 0;
    g_cursor_y++;

    if (g_cursor_y >= g_max_rows)
    {
        console_scroll();
    }

    spin_unlock_irqrestore(&g_console_lock, flags);
}

static void console_status_restore_internal(void)
{
    if (!g_status_visible || !g_font || !graphics_framebuffer_is_bound() ||
        g_max_cols == 0 || g_max_rows == 0)
        return;

    uint32_t actual_line = (g_view_start_line + g_status_row) %
                           CONSOLE_MAX_HISTORY;
    uint32_t old_bg = g_color_bg;
    for (uint32_t x = 0; x < g_max_cols; x++)
    {
        ConsoleCell *cell = &g_history[actual_line][x];
        char value = cell->c ? cell->c : ' ';
        g_color_bg = cell->bg;
        draw_char_pixels_at(value, x, g_status_row, cell->fg);
    }
    g_color_bg = old_bg;
}

static void console_status_repaint_internal(void)
{
    if (!g_status_visible || !g_font || !graphics_framebuffer_is_bound() ||
        g_max_cols == 0 || g_max_rows == 0)
        return;

    uint32_t old_bg = g_color_bg;
    g_color_bg = g_status_bg;
    for (uint32_t x = 0; x < g_max_cols; x++)
        draw_char_pixels_at(' ', x, g_status_row, CONSOLE_COLOR_YELLOW);
    for (uint32_t x = 0; g_status_message[x] && x < g_max_cols; x++)
        draw_char_pixels_at(g_status_message[x], x, g_status_row,
                            CONSOLE_COLOR_YELLOW);
    g_color_bg = old_bg;
}

void console_status_set(const char *msg)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (!g_font || !graphics_framebuffer_is_bound() ||
        g_max_cols == 0 || g_max_rows == 0)
    {
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    console_begin_batch_internal();
    if (g_status_visible)
        console_status_restore_internal();

    g_status_row = (g_cursor_y == (g_max_rows - 1) && g_max_rows > 1)
        ? (g_max_rows - 2) : (g_max_rows - 1);
    g_status_bg = g_color_bg;

    uint32_t length = 0;
    bool truncated = false;
    if (!msg)
        msg = "";
    while (msg[length] && length < CONSOLE_STATUS_MESSAGE_MAX - 1u)
    {
        char value = msg[length];
        if (value == '\n' || value == '\r' || value == '\t')
            value = ' ';
        g_status_message[length++] = value;
    }
    truncated = msg[length] != '\0';
    if (truncated && length >= 3u)
    {
        g_status_message[length - 3u] = '.';
        g_status_message[length - 2u] = '.';
        g_status_message[length - 1u] = '.';
    }
    g_status_message[length] = '\0';
    g_status_visible = true;
    console_status_repaint_internal();
    console_end_batch_internal();
    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_status_clear(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (g_status_visible)
    {
        console_begin_batch_internal();
        console_status_restore_internal();
        g_status_visible = false;
        g_status_message[0] = '\0';
        console_end_batch_internal();
    }

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_draw_cursor(char underlying_char)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (!g_font || !graphics_framebuffer_is_bound())
    {
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }
    if (g_max_cols == 0 || g_max_rows == 0)
    {
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    if (g_cursor_x >= g_max_cols)
        g_cursor_x = g_max_cols - 1;
    if (g_cursor_y >= g_max_rows)
        g_cursor_y = g_max_rows - 1;

    if (underlying_char == 0)
    {
        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        underlying_char = g_history[actual_line][g_cursor_x].c;
        if (underlying_char == 0)
            underlying_char = ' ';
    }

    uint32_t cursor_fg = CONSOLE_COLOR_BLACK;
    uint32_t cursor_bg = CONSOLE_COLOR_WHITE;

    uint32_t cx = CONSOLE_PAD_X + (g_cursor_x * 8);
    uint32_t cy = CONSOLE_PAD_Y + (g_cursor_y * g_font->psf_header->charsize);

    char *fontPtr = (char *)g_font->glyph_buffer + (underlying_char * g_font->psf_header->charsize);

    for (uint32_t py = 0; py < g_font->psf_header->charsize; py++)
    {
        char row = *fontPtr;
        for (uint32_t px = 0; px < 8; px++)
        {
            if (row & (0x80 >> px))
            {
                put_pixel(cx + px, cy + py, cursor_fg);
            }
            else
            {
                put_pixel(cx + px, cy + py, cursor_bg);
            }
        }
        fontPtr++;
    }

    console_flush_if_needed();

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_print_hex(uint64_t n)
{
    char buffer[19];
    buffer[0] = '0';
    buffer[1] = 'x';

    for (int i = 0; i < 16; i++)
    {
        uint8_t nibble = (n >> ((15 - i) * 4)) & 0xF;
        buffer[2 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buffer[18] = '\0';
    console_write(buffer);
}

void console_print_dec(uint64_t n)
{
    char buffer[32];
    int i = 0;

    if (n == 0)
    {
        console_put_char('0');
        return;
    }

    while (n > 0)
    {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }

    for (int j = i - 1; j >= 0; j--)
    {
        console_put_char(buffer[j]);
    }
}

void console_set_color(uint32_t fg, uint32_t bg)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    g_color_fg = fg;
    g_color_bg = bg;
    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_get_cursor(uint32_t *x, uint32_t *y)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (x)
        *x = g_cursor_x;
    if (y)
        *y = g_cursor_y;

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_set_cursor(uint32_t x, uint32_t y)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (g_max_cols == 0 || g_max_rows == 0)
    {
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    if (x >= g_max_cols)
        x = g_max_cols - 1;
    if (y >= g_max_rows)
        y = g_max_rows - 1;

    g_cursor_x = x;
    g_cursor_y = y;

    spin_unlock_irqrestore(&g_console_lock, flags);
}

uint32_t console_get_max_cols(void)
{
    return g_max_cols;
}

uint32_t console_get_max_rows(void)
{
    return g_max_rows;
}

static bool region_valid(const console_region_t *r)
{
    if (!r || !r->width || !r->height || r->x >= g_max_cols || r->y >= g_max_rows)
        return false;
    if (r->width > g_max_cols - r->x || r->height > g_max_rows - r->y)
        return false;
    return true;
}

static void region_cell(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
    uint32_t history = (g_view_start_line + y) % CONSOLE_MAX_HISTORY;
    g_history[history][x].c = c;
    g_history[history][x].fg = fg;
    g_history[history][x].bg = bg;
    uint32_t saved_bg = g_color_bg;
    g_color_bg = bg;
    draw_char_pixels_at(c, x, y, fg);
    g_color_bg = saved_bg;
}

bool console_region_clear(const console_region_t *region, uint32_t bg)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    if (!region_valid(region)) {
        g_region_stats.rejected_regions++;
        spin_unlock_irqrestore(&g_console_lock, flags);
        return false;
    }
    for (uint32_t y = 0; y < region->height; y++)
        for (uint32_t x = 0; x < region->width; x++)
            region_cell(region->x + x, region->y + y, ' ', CONSOLE_COLOR_WHITE, bg);
    g_region_stats.clears++;
    console_flush_if_needed();
    spin_unlock_irqrestore(&g_console_lock, flags);
    return true;
}

bool console_region_write_line(const console_region_t *region, uint32_t row,
                               const char *text, uint32_t fg, uint32_t bg)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    if (!region_valid(region) || row >= region->height || !text) {
        g_region_stats.rejected_regions++;
        spin_unlock_irqrestore(&g_console_lock, flags);
        return false;
    }
    uint32_t n = 0;
    while (text[n] && n < region->width) {
        char c = text[n];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        region_cell(region->x + n, region->y + row, c, fg, bg);
        n++;
    }
    if (text[n]) g_region_stats.clipped_writes++;
    while (n < region->width) {
        region_cell(region->x + n, region->y + row, ' ', fg, bg);
        n++;
    }
    g_region_stats.writes++;
    console_flush_if_needed();
    spin_unlock_irqrestore(&g_console_lock, flags);
    return true;
}

bool console_region_present_row(const console_region_t *region, uint32_t row,
                                const ConsoleCell *cells, uint32_t cell_count,
                                ConsoleCell fill,
                                console_region_present_result_t *out)
{
    console_region_present_result_t result = {0};
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    if (!region_valid(region) || row >= region->height ||
        (!cells && cell_count) || cell_count > region->width) {
        g_region_stats.rejected_regions++;
        spin_unlock_irqrestore(&g_console_lock, flags);
        if (out) *out = result;
        return false;
    }

    for (uint32_t x = 0; x < region->width; x++) {
        ConsoleCell target = x < cell_count ? cells[x] : fill;
        if (target.c == '\n' || target.c == '\r' || target.c == '\t' ||
            target.c == 0)
            target.c = ' ';
        uint32_t history = (g_view_start_line + region->y + row) %
                           CONSOLE_MAX_HISTORY;
        ConsoleCell *current = &g_history[history][region->x + x];
        bool glyph_changed = current->c != target.c;
        bool style_changed = current->fg != target.fg ||
                             current->bg != target.bg;
        bool changed = glyph_changed || style_changed;
#ifdef HOBBYOS_CONSOLE_NEGATIVE_PRESENT_ALWAYS_DIRTY
        changed = true;
#endif
        result.cells_examined++;
        if (changed) {
            region_cell(region->x + x, region->y + row, target.c,
                        target.fg, target.bg);
            result.cells_changed++;
            result.glyph_changes += glyph_changed;
            result.style_changes += style_changed;
            result.row_dirty = 1;
        } else {
            result.cells_unchanged++;
        }
    }

    g_region_stats.present_rows++;
    g_region_stats.present_cells += result.cells_examined;
    g_region_stats.changed_cells += result.cells_changed;
    g_region_stats.unchanged_cells += result.cells_unchanged;
    g_region_stats.glyph_changes += result.glyph_changes;
    g_region_stats.style_changes += result.style_changes;
    if (!result.row_dirty) g_region_stats.zero_change_rows++;
    if (result.row_dirty) console_flush_if_needed();
    spin_unlock_irqrestore(&g_console_lock, flags);
    if (out) *out = result;
    return true;
}

void console_region_stats_snapshot(console_region_stats_t *out)
{
    if (!out) return;
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    *out = g_region_stats;
    spin_unlock_irqrestore(&g_console_lock, flags);
}

uint64_t console_scroll_count(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    uint64_t value = g_region_stats.scroll_count;
    spin_unlock_irqrestore(&g_console_lock, flags);
    return value;
}

bool console_region_count_nonblank(const console_region_t *region,uint32_t*out_count)
{
    if(!out_count)return false;
    irq_flags_t flags=spin_lock_irqsave(&g_console_lock);
    if(!region_valid(region)){g_region_stats.rejected_regions++;spin_unlock_irqrestore(&g_console_lock,flags);return false;}
    uint32_t n=0;
    for(uint32_t y=0;y<region->height;y++)for(uint32_t x=0;x<region->width;x++){
        uint32_t history=(g_view_start_line+region->y+y)%CONSOLE_MAX_HISTORY;
        if(g_history[history][region->x+x].c!=' ')n++;
    }
    *out_count=n;spin_unlock_irqrestore(&g_console_lock,flags);return true;
}

#ifdef HOBBYOS_SELFTEST
bool console_test_history_contains(const char *needle)
{
    if (!needle || !needle[0])
        return false;
    uint32_t needle_length = 0;
    while (needle[needle_length] && needle_length < CONSOLE_STATUS_MESSAGE_MAX)
        needle_length++;
    if (!needle_length || needle[needle_length] || needle_length > g_max_cols)
        return false;

    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    bool found = false;
    for (uint32_t y = 0; y < g_max_rows && !found; y++)
    {
        uint32_t history = (g_view_start_line + y) % CONSOLE_MAX_HISTORY;
        for (uint32_t x = 0; x + needle_length <= g_max_cols; x++)
        {
            uint32_t index = 0;
            while (index < needle_length &&
                   g_history[history][x + index].c == needle[index])
                index++;
            if (index == needle_length)
            {
                found = true;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&g_console_lock, flags);
    return found;
}

bool console_test_status_equals(const char *expected)
{
    if (!expected)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    bool equal = g_status_visible && strcmp(g_status_message, expected) == 0;
    spin_unlock_irqrestore(&g_console_lock, flags);
    return equal;
}

bool console_test_status_visible(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);
    bool visible = g_status_visible;
    spin_unlock_irqrestore(&g_console_lock, flags);
    return visible;
}
#endif
