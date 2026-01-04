#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include "../../../shared/protocol.h"


#define CONSOLE_PAD_X 20
#define CONSOLE_PAD_Y 20



#define CONSOLE_MAX_HISTORY 2000
#define CONSOLE_MAX_COLS_STORAGE 256


#define CONSOLE_COLOR_WHITE        0xFFFFFFFF
#define CONSOLE_COLOR_BLACK        0xFF000000
#define CONSOLE_COLOR_BLUE         0xFF0000AA
#define CONSOLE_COLOR_RED          0xFFFF0000
#define CONSOLE_COLOR_GREEN        0xFF00FF00
#define CONSOLE_COLOR_YELLOW       0xFFFFFF00


#define CONSOLE_COLOR_HOBBYOS_BLUE 0xFF000022 



typedef struct {
    char c;
    uint32_t fg;
    uint32_t bg;
} ConsoleCell;


void console_init(BootInfo* boot_info);


void console_clear(uint32_t bg_color);


void console_put_char(char c);
void console_write(const char* str);


void console_redraw_char(char c);



void console_backspace();
void console_move_left();
void console_move_right();


void console_draw_cursor(char underlying_char);


void console_set_color(uint32_t fg, uint32_t bg);

#endif