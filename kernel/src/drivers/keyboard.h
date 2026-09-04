#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include "../core/input_event.h"
#include "../core/input_queue.h"

void keyboard_init();
void keyboard_handle_interrupt();

void keyboard_push_usb_event(uint8_t modifiers, uint8_t keycode);

uint32_t keyboard_usb_fifo_drops(void);
bool keyboard_input_queue_snapshot(input_queue_stats_t *out);
bool keyboard_input_queue_validate(input_queue_validation_t *out);
bool keyboard_test_enqueue_event(input_event_t event);

void input_thread_entry(void *arg);

#endif
