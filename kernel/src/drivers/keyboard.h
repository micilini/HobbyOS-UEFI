#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

void keyboard_init();
void keyboard_handle_interrupt();

void keyboard_push_usb_event(uint8_t modifiers, uint8_t keycode);

#endif