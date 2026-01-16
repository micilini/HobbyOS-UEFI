#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include "../../../shared/protocol.h"

#define CONSOLE_PAD_X 20
#define CONSOLE_PAD_Y 20

#define CONSOLE_MAX_HISTORY 2000
#define CONSOLE_MAX_COLS_STORAGE 512

#define CONSOLE_COLOR_WHITE 0xFFFFFFFF
#define CONSOLE_COLOR_BLACK 0xFF000000
#define CONSOLE_COLOR_BLUE 0xFF0000AA
#define CONSOLE_COLOR_HOBBYOS_BLUE 0xFF000022
#define CONSOLE_COLOR_GREEN 0xFF00FF00
#define CONSOLE_COLOR_YELLOW 0xFFFFFF00
#define CONSOLE_COLOR_RED 0xFFFF0000

typedef struct
{
    char c;
    uint32_t fg;
    uint32_t bg;
} ConsoleCell;

void console_init(BootInfo *boot_info);
void console_render_full();

void console_redraw_char(char c);

void console_clear(uint32_t bg_color);

void console_put_char(char c);
void console_write(const char *str);

extern volatile uint8_t g_console_debug_enabled;

void console_set_debug_enabled(uint8_t enabled);

void console_write_debug(const char *str);
void console_print_hex_debug(uint64_t n);
void console_print_dec_debug(uint64_t n);
void console_set_color_debug(uint32_t fg, uint32_t bg);
void console_put_char_debug(char c);

void console_backspace();
void console_move_left();
void console_move_right();

void console_status_set(const char *msg);
void console_status_clear(void);

void console_begin_batch();
void console_end_batch();

void console_draw_cursor(char underlying_char);

void console_set_color(uint32_t fg, uint32_t bg);

void console_print_hex(uint64_t n);
void console_print_dec(uint64_t n);

void console_get_cursor(uint32_t *x, uint32_t *y);
void console_set_cursor(uint32_t x, uint32_t y);

uint32_t console_get_max_cols(void);
uint32_t console_get_max_rows(void);

#endif