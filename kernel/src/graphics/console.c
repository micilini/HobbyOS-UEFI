#include "console.h"
#include "graphics.h"
#include "../utils/utils.h" 
#include "../libc/memory.h" 

static Framebuffer* g_fb = NULL;
static Psf1_Font* g_font = NULL;



static ConsoleCell g_history[CONSOLE_MAX_HISTORY][CONSOLE_MAX_COLS_STORAGE];


static uint32_t g_view_start_line = 0;


static uint32_t g_cursor_x = 0;
static uint32_t g_cursor_y = 0; 


static uint32_t g_color_fg = CONSOLE_COLOR_WHITE;
static uint32_t g_color_bg = CONSOLE_COLOR_HOBBYOS_BLUE;


static uint32_t g_max_cols = 0;
static uint32_t g_max_rows = 0;

void console_init(BootInfo* boot_info) {
    g_fb = boot_info->framebuffer;
    g_font = boot_info->font;

    g_cursor_x = 0;
    g_cursor_y = 0;
    g_view_start_line = 0;
    g_color_fg = CONSOLE_COLOR_WHITE;
    g_color_bg = CONSOLE_COLOR_HOBBYOS_BLUE; 

    
    for (int y = 0; y < CONSOLE_MAX_HISTORY; y++) {
        for (int x = 0; x < CONSOLE_MAX_COLS_STORAGE; x++) {
            g_history[y][x].c = 0; 
            g_history[y][x].fg = g_color_fg;
            g_history[y][x].bg = g_color_bg;
        }
    }

    if (g_font && g_fb) {
        uint32_t usable_width = g_fb->Width - (CONSOLE_PAD_X * 2);
        uint32_t usable_height = g_fb->Height - (CONSOLE_PAD_Y * 2);

        g_max_cols = usable_width / 8; 
        g_max_rows = usable_height / g_font->psf_header->charsize;
        
        
        if (g_max_cols > CONSOLE_MAX_COLS_STORAGE) {
            g_max_cols = CONSOLE_MAX_COLS_STORAGE;
        }
    }
}

void console_set_color(uint32_t fg, uint32_t bg) {
    g_color_fg = fg;
    g_color_bg = bg;
}



static void console_render_full() {
    if (!g_fb || !g_font) return;

    
    
    clear_screen(g_color_bg);

    uint32_t char_height = g_font->psf_header->charsize;
    
    
    for (uint32_t screen_y = 0; screen_y < g_max_rows; screen_y++) {
        
        
        
        uint32_t history_idx = (g_view_start_line + screen_y) % CONSOLE_MAX_HISTORY;
        
        uint32_t cy_pixel = CONSOLE_PAD_Y + (screen_y * char_height);

        
        for (uint32_t x = 0; x < g_max_cols; x++) {
            ConsoleCell* cell = &g_history[history_idx][x];
            
            
            if (cell->c != 0 && cell->c != ' ') {
                
                uint32_t cx_pixel = CONSOLE_PAD_X + (x * 8);
                char* fontPtr = (char*)g_font->glyph_buffer + (cell->c * char_height);
                
                for (uint32_t py = 0; py < char_height; py++) {
                    for (uint32_t px = 0; px < 8; px++) {
                        if ((*fontPtr & (0b10000000 >> px)) > 0) {
                            put_pixel(cx_pixel + px, cy_pixel + py, cell->fg);
                        } 
                        
                    }
                    fontPtr++;
                }
            }
        }
    }
    
    
    swap_buffers();
}

void console_clear(uint32_t bg_color) {
    g_color_bg = bg_color;
    g_cursor_x = 0;
    g_cursor_y = 0;
    
    
    g_view_start_line = 0;

    
    for (int y = 0; y < CONSOLE_MAX_HISTORY; y++) {
        for (int x = 0; x < CONSOLE_MAX_COLS_STORAGE; x++) {
            g_history[y][x].c = 0;
            g_history[y][x].bg = bg_color;
        }
    }
    
    console_render_full();
}


static void console_scroll() {
    
    g_view_start_line++;
    
    
    if (g_view_start_line >= CONSOLE_MAX_HISTORY) {
        g_view_start_line = 0;
    }

    
    
    uint32_t new_bottom_line_idx = (g_view_start_line + g_max_rows - 1) % CONSOLE_MAX_HISTORY;
    
    for (int x = 0; x < CONSOLE_MAX_COLS_STORAGE; x++) {
        g_history[new_bottom_line_idx][x].c = 0;
        g_history[new_bottom_line_idx][x].fg = g_color_fg;
        g_history[new_bottom_line_idx][x].bg = g_color_bg;
    }

    
    g_cursor_y = g_max_rows - 1;
    
    
    console_render_full();
}



static void draw_char_pixels_at(char c, uint32_t x, uint32_t y, uint32_t fg) {
    if (!g_fb || !g_font) return;

    uint32_t cx = CONSOLE_PAD_X + (x * 8); 
    uint32_t cy = CONSOLE_PAD_Y + (y * g_font->psf_header->charsize);
    char* fontPtr = (char*)g_font->glyph_buffer + (c * g_font->psf_header->charsize);
    
    for (unsigned long py = 0; py < g_font->psf_header->charsize; py++) {
        for (unsigned long px = 0; px < 8; px++) {
            if ((*fontPtr & (0b10000000 >> px)) > 0) {
                put_pixel(cx + px, cy + py, fg);
            } else {
                put_pixel(cx + px, cy + py, g_color_bg);
            }
        }
        fontPtr++;
    }
}


static void console_put_char_internal(char c) {
    if (c == '\n') {
        g_cursor_x = 0;
        g_cursor_y++;
    } 
    else if (c == '\r') {
        g_cursor_x = 0;
    }
    else {
        
        
        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        
        if (g_cursor_x < CONSOLE_MAX_COLS_STORAGE) {
            g_history[actual_line][g_cursor_x].c = c;
            g_history[actual_line][g_cursor_x].fg = g_color_fg;
            g_history[actual_line][g_cursor_x].bg = g_color_bg;
        }

        
        draw_char_pixels_at(c, g_cursor_x, g_cursor_y, g_color_fg);
        
        g_cursor_x++;
    }

    
    if (g_cursor_x >= g_max_cols) {
        g_cursor_x = 0;
        g_cursor_y++;
    }

    
    if (g_cursor_y >= g_max_rows) {
        console_scroll(); 
    }
}

void console_put_char(char c) {
    console_put_char_internal(c);
    
    if (g_cursor_y < g_max_rows) {
        swap_buffers();
    }
}

void console_write(const char* str) {
    while (*str) {
        console_put_char_internal(*str);
        str++;
    }
    
    swap_buffers();
}

void console_redraw_char(char c) {
    if (c == 0) c = ' ';
    draw_char_pixels_at(c, g_cursor_x, g_cursor_y, g_color_fg);
    swap_buffers(); 
}

void console_backspace() {
    if (g_cursor_x > 0) {
        g_cursor_x--;
        
        
        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        g_history[actual_line][g_cursor_x].c = 0;

        
        console_redraw_char(' '); 

    } else if (g_cursor_y > 0) {
        g_cursor_y--;
        g_cursor_x = g_max_cols - 1;
        
        
        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        g_history[actual_line][g_cursor_x].c = 0;

        console_redraw_char(' ');
    }
}

void console_move_left() {
    if (g_cursor_x > 0) g_cursor_x--;
}

void console_move_right() {
    if (g_cursor_x < g_max_cols - 1) g_cursor_x++;
}

void console_draw_cursor(char underlying_char) {
    if (!g_fb || !g_font) return;

    
    if (underlying_char == 0) {
        uint32_t actual_line = (g_view_start_line + g_cursor_y) % CONSOLE_MAX_HISTORY;
        underlying_char = g_history[actual_line][g_cursor_x].c;
        if (underlying_char == 0) underlying_char = ' ';
    }

    
    uint32_t cursor_fg = CONSOLE_COLOR_BLACK;
    uint32_t cursor_bg = CONSOLE_COLOR_WHITE;

    
    uint32_t cx = CONSOLE_PAD_X + (g_cursor_x * 8); 
    uint32_t cy = CONSOLE_PAD_Y + (g_cursor_y * g_font->psf_header->charsize);
    char* fontPtr = (char*)g_font->glyph_buffer + (underlying_char * g_font->psf_header->charsize);
    
    for (unsigned long py = 0; py < g_font->psf_header->charsize; py++) {
        for (unsigned long px = 0; px < 8; px++) {
            if ((*fontPtr & (0b10000000 >> px)) > 0) {
                put_pixel(cx + px, cy + py, cursor_fg);
            } else {
                put_pixel(cx + px, cy + py, cursor_bg); 
            }
        }
        fontPtr++;
    }

    swap_buffers();
}