#include "shell.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h" 

static char g_buffer[SHELL_CMD_BUFFER_SIZE];
static int g_len = 0; 
static int g_pos = 0; 
static bool g_cursor_visible = true; 
static bool g_shell_active = false;

#define SHELL_COLOR_ACCENT 0xFF00FFFF 
const char* PROMPT = "HobbyOS> ";

static void shell_show_cursor() {
    console_draw_cursor(g_buffer[g_pos]);
    g_cursor_visible = true;
}

static void shell_hide_cursor() {
    console_redraw_char(g_buffer[g_pos]);
    g_cursor_visible = false;
}

void shell_on_tick() {
    if (!g_shell_active) return;

    if (g_cursor_visible) {
        shell_hide_cursor();
    } else {
        shell_show_cursor();
    }
}

static void shell_reset_buffer() {
    memset(g_buffer, 0, SHELL_CMD_BUFFER_SIZE);
    g_len = 0;
    g_pos = 0;
    
    console_set_color(SHELL_COLOR_ACCENT, CONSOLE_COLOR_HOBBYOS_BLUE);
    console_write(PROMPT);
    console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
    
    shell_show_cursor();
}

static void shell_execute_command() {
    shell_hide_cursor();
    console_put_char('\n'); 

    if (g_len == 0) {
        shell_reset_buffer();
        return;
    }

    if (strcmp(g_buffer, "ajuda") == 0 || strcmp(g_buffer, "help") == 0) {
        console_set_color(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("Comandos disponiveis:\n");
        console_write("  ajuda   - Mostra esta mensagem\n");
        console_write("  limpar  - Limpa a tela\n");
        console_write("  sobre   - Sobre o HobbyOS\n");
    }
    else if (strcmp(g_buffer, "limpar") == 0 || strcmp(g_buffer, "cls") == 0) {
        console_clear(CONSOLE_COLOR_HOBBYOS_BLUE);
    }
    else if (strcmp(g_buffer, "sobre") == 0) {
        console_set_color(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("HobbyOS v0.2 - Kernel Educacional.\n");
        console_write("Criado com ajuda de IA e muita cafeina.\n");
    }
    else {
        console_set_color(CONSOLE_COLOR_RED, CONSOLE_COLOR_HOBBYOS_BLUE);
        console_write("O comando '");
        console_write(g_buffer);
        console_write("' nao foi reconhecido.\n");
    }

    shell_reset_buffer();
}

void shell_init() {
    g_shell_active = true;
    shell_reset_buffer();
}


void shell_receive_char(char c) {
    if (!g_shell_active) return;
    
    
    if (g_cursor_visible) shell_hide_cursor();

    if (c == '\n') {
        shell_execute_command();
        return;
    }

    if (c == '\b') {
        if (g_pos > 0) {
            if (g_pos == g_len) {
                g_pos--; 
                g_len--;
                g_buffer[g_pos] = 0;
                console_backspace(); 
            } 
            else {
                memmove(&g_buffer[g_pos - 1], &g_buffer[g_pos], g_len - g_pos);
                g_len--;
                g_pos--;
                g_buffer[g_len] = 0;
                console_backspace(); 
                
                int saved_pos = g_pos; 
                console_write(&g_buffer[g_pos]);
                console_put_char(' '); 

                int steps_back = (g_len - saved_pos) + 1;
                for(int i=0; i < steps_back; i++) console_move_left();
            }
        }
        
        return;
    }

    if (g_len < SHELL_CMD_BUFFER_SIZE - 1) {
        if (g_pos == g_len) {
            g_buffer[g_pos] = c;
            g_len++;
            g_pos++;
            g_buffer[g_len] = 0; 
            console_put_char(c);
        } 
        else {
            memmove(&g_buffer[g_pos + 1], &g_buffer[g_pos], g_len - g_pos);
            g_buffer[g_pos] = c;
            g_len++;
            g_pos++;
            g_buffer[g_len] = 0;
            console_put_char(c);
            console_write(&g_buffer[g_pos]);
            int steps_back = g_len - g_pos;
            for(int i=0; i < steps_back; i++) console_move_left();
        }
    }
}


void shell_refresh_view() {
    if (!g_shell_active) return;
    shell_show_cursor();
}

void shell_receive_special(uint8_t key) {
    if (!g_shell_active) return;
    if (g_cursor_visible) shell_hide_cursor();

    if (key == KEY_SPECIAL_LEFT) {
        if (g_pos > 0) {
            g_pos--;
            console_move_left();
        }
    }
    else if (key == KEY_SPECIAL_RIGHT) {
        if (g_pos < g_len) {
            g_pos++;
            console_move_right();
        }
    }
    
    shell_show_cursor();
}