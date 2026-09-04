#ifndef SPLASH_H
#define SPLASH_H

#include "../../../shared/protocol.h"
#include <stdbool.h>

typedef enum {
    SPLASH_RESULT_COMPLETE = 0,
    SPLASH_RESULT_SKIPPED_NO_LOGO,
    SPLASH_RESULT_SKIPPED_INVALID_IMAGE,
    SPLASH_RESULT_ABORTED_CLOCK_STALL,
    SPLASH_RESULT_ABORTED_RENDER_BUDGET,
    SPLASH_RESULT_ABORTED_CONSOLE_BUSY,
} splash_result_t;

splash_result_t play_splash_screen(BootInfo *boot_info);
const char *splash_result_name(splash_result_t result);
bool splash_model_selftest(void);

void discard_splash_memory(BootInfo *boot_info);

#endif
