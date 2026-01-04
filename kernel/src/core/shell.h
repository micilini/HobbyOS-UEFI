#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stdbool.h>

#define SHELL_CMD_BUFFER_SIZE 256

#define KEY_SPECIAL_LEFT  0xF1
#define KEY_SPECIAL_RIGHT 0xF2
#define KEY_SPECIAL_UP    0xF3
#define KEY_SPECIAL_DOWN  0xF4

void shell_init();
void shell_receive_char(char c);
void shell_receive_special(uint8_t key);
void shell_on_tick();


void shell_refresh_view();

#endif