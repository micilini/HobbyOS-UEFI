#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>


#define KEY_SPECIAL_UP    0xF1
#define KEY_SPECIAL_DOWN  0xF2
#define KEY_SPECIAL_LEFT  0xF3
#define KEY_SPECIAL_RIGHT 0xF4


void keyboard_init();


void keyboard_handle_interrupt();

#endif