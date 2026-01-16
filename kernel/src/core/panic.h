#include <stdint.h>

#ifndef PANIC_H
#define PANIC_H

typedef enum
{
    PANIC_ACTION_HALT = 0,
    PANIC_ACTION_RESTART = 1,
    PANIC_ACTION_SHUTDOWN = 2
} PanicAction;

void panic_config(PanicAction action, uint32_t timeout_seconds);

void kpanic(char *message);

void kpanic_exception_ex(const char *title,
                         uint8_t vector,
                         void *frame,
                         uint64_t error_code,
                         int has_error_code,
                         uint64_t cr2,
                         int has_cr2);

#endif