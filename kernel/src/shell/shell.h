#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint8_t initialized;
    uint8_t thread_started;
    uint8_t active;
    uint8_t modal_paused;
} shell_runtime_snapshot_t;

#define SHELL_CMD_BUFFER_SIZE 256
#define SHELL_STATUS_MESSAGE_MAX 128

typedef struct
{
    char message[SHELL_STATUS_MESSAGE_MAX];
    uint64_t generation;
    uint64_t published;
    uint64_t deferred;
    uint64_t flushed;
    uint64_t replaced;
    uint64_t cleared;
    uint8_t pending;
    uint8_t visible;
    uint8_t terminal;
} shell_status_snapshot_t;

#include "../core/input_event.h"

void shell_init();
void shell_receive_char(char c);
void shell_receive_special(uint8_t key);
void shell_on_tick();

void shell_refresh_view();

void shell_thread_entry(void *arg);
bool shell_runtime_snapshot(shell_runtime_snapshot_t *out);
uint64_t shell_status_begin(const char *message);
bool shell_status_complete(uint64_t generation, const char *message);
void shell_clear_status_on_input(void);
bool shell_status_snapshot(shell_status_snapshot_t *out);

#ifdef HOBBYOS_SELFTEST
int shell_execute_command_line_for_selftest(const char *line);
void shell_test_status_reset(void);
void shell_test_status_set_runtime(bool active, bool modal_paused);
bool shell_test_status_set_input(const char *text, int position);
bool shell_test_status_input_equals(const char *text, int position);
#endif

void shell_pause_input_for_modal_ui(void);
void shell_resume_input_from_modal_ui(void);
bool shell_is_input_paused_for_modal_ui(void);
uint64_t shell_modal_pause_drop_count(void);
void shell_test_pause_boundary_arm(void);
bool shell_test_pause_boundary_entered(void);
void shell_test_pause_boundary_release(void);
bool shell_test_consume_default_event(bool apply_to_shell);

#endif
