#include "console.h"
#include "graphics.h"
#include "../utils/utils.h"
#include "../libc/memory.h"
#include "../core/spinlock.h"

static Framebuffer *g_fb = NULL;
static Psf1_Font *g_font = NULL;

volatile uint8_t g_console_debug_enabled = 1;

static spinlock_t g_console_lock;

static ConsoleCell g_history[CONSOLE_MAX_HISTORY][CONSOLE_MAX_COLS_STORAGE];

static uint32_t g_view_start_line = 0;

static uint32_t g_cursor_x = 0;
static uint32_t g_cursor_y = 0;

static uint32_t g_color_fg = CONSOLE_COLOR_WHITE;
static uint32_t g_color_bg = CONSOLE_COLOR_HOBBYOS_BLUE;

static int g_console_batch = 0;

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
    if (g_console_batch == 0)
        swap_buffers();
}

static uint32_t g_max_cols = 0;
static uint32_t g_max_rows = 0;

static void compute_max_dimensions(void)
{
    if (!g_fb || !g_font || !g_font->psf_header)
        return;

    uint32_t fb_width = g_fb->Width;
    if (g_fb->PixelsPerScanLine && g_fb->PixelsPerScanLine < fb_width)
    {
        fb_width = g_fb->PixelsPerScanLine;
    }

    uint32_t fb_height = g_fb->Height;

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

    g_view_start_line = (g_view_start_line + 1) % CONSOLE_MAX_HISTORY;

    uint32_t bottom_line = (g_view_start_line + g_max_rows - 1) % CONSOLE_MAX_HISTORY;
    for (uint32_t x = 0; x < g_max_cols; x++)
    {
        g_history[bottom_line][x].c = 0;
        g_history[bottom_line][x].fg = g_color_fg;
        g_history[bottom_line][x].bg = g_color_bg;
    }

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

    console_flush_if_needed();
}

void console_init(BootInfo *boot_info)
{
    spinlock_init(&g_console_lock);

    g_fb = boot_info->framebuffer;
    g_font = boot_info->font;

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
    if (!g_font || !g_fb)
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
}

static void console_put_char_internal(char c)
{
    if (!g_font || !g_fb)
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
    console_write(str);
}

void console_print_hex_debug(uint64_t n)
{
    if (!g_console_debug_enabled)
        return;
    console_print_hex(n);
}

void console_print_dec_debug(uint64_t n)
{
    if (!g_console_debug_enabled)
        return;
    console_print_dec(n);
}

void console_set_color_debug(uint32_t fg, uint32_t bg)
{
    if (!g_console_debug_enabled)
        return;
    console_set_color(fg, bg);
}

void console_put_char_debug(char c)
{
    if (!g_console_debug_enabled)
        return;
    console_put_char(c);
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

static bool g_status_visible = false;
static uint32_t g_status_row = 0;

static void status_clear_row(uint32_t row_y, uint32_t bg)
{
    if (!g_font || !g_fb || g_max_cols == 0 || g_max_rows == 0)
        return;

    uint32_t actual_line = (g_view_start_line + row_y) % CONSOLE_MAX_HISTORY;

    uint32_t old_bg = g_color_bg;
    g_color_bg = bg;

    for (uint32_t x = 0; x < g_max_cols; x++)
    {
        g_history[actual_line][x].c = 0;
        g_history[actual_line][x].bg = bg;
        draw_char_pixels_at(' ', x, row_y, g_color_fg);
    }

    g_color_bg = old_bg;
}

void console_status_set(const char *msg)
{
    if (!g_font || !g_fb || g_max_cols == 0 || g_max_rows == 0)
        return;

    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    uint32_t row = (g_cursor_y == (g_max_rows - 1) && g_max_rows > 1) ? (g_max_rows - 2) : (g_max_rows - 1);
    g_status_row = row;

    console_begin_batch_internal();

    uint32_t saved_x = g_cursor_x;
    uint32_t saved_y = g_cursor_y;
    uint32_t saved_fg = g_color_fg;
    uint32_t saved_bg = g_color_bg;

    uint32_t status_fg = CONSOLE_COLOR_YELLOW;
    uint32_t status_bg = saved_bg;

    g_color_fg = status_fg;
    g_color_bg = status_bg;
    status_clear_row(row, status_bg);

    uint32_t actual_line = (g_view_start_line + row) % CONSOLE_MAX_HISTORY;
    uint32_t x = 0;
    if (!msg)
        msg = "";

    while (msg[x] && x < g_max_cols)
    {
        char c = msg[x];
        g_history[actual_line][x].c = c;
        g_history[actual_line][x].bg = status_bg;
        draw_char_pixels_at(c, x, row, status_fg);
        x++;
    }

    g_color_fg = saved_fg;
    g_color_bg = saved_bg;
    g_cursor_x = saved_x;
    g_cursor_y = saved_y;

    g_status_visible = true;

    console_end_batch_internal();

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_status_clear(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (!g_status_visible)
    {
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    if (!g_font || !g_fb || g_max_cols == 0 || g_max_rows == 0)
    {
        g_status_visible = false;
        spin_unlock_irqrestore(&g_console_lock, flags);
        return;
    }

    console_begin_batch_internal();

    uint32_t saved_x = g_cursor_x;
    uint32_t saved_y = g_cursor_y;
    uint32_t saved_fg = g_color_fg;
    uint32_t saved_bg = g_color_bg;

    status_clear_row(g_status_row, saved_bg);

    g_color_fg = saved_fg;
    g_color_bg = saved_bg;
    g_cursor_x = saved_x;
    g_cursor_y = saved_y;

    g_status_visible = false;

    console_end_batch_internal();

    spin_unlock_irqrestore(&g_console_lock, flags);
}

void console_draw_cursor(char underlying_char)
{
    irq_flags_t flags = spin_lock_irqsave(&g_console_lock);

    if (!g_font || !g_fb)
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
