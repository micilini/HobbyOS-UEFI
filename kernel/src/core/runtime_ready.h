#ifndef HOBBYOS_RUNTIME_READY_H
#define HOBBYOS_RUNTIME_READY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    RUNTIME_READY_NOT_STARTED = 0,
    RUNTIME_READY_WAITING_SERVICES,
    RUNTIME_READY_WAITING_DRIVERS,
    RUNTIME_READY_READY,
    RUNTIME_READY_TESTING,
    RUNTIME_READY_TEST_READY,
    RUNTIME_READY_FAILED
} runtime_ready_state_t;

typedef struct
{
    runtime_ready_state_t state;
    uint32_t cpus_expected;
    uint32_t cpus_online;
    uint8_t scheduler_started;
    uint8_t dpc_initialized;
    uint8_t shell_initialized;
    uint8_t shell_thread_started;
    uint8_t input_valid;
    uint8_t modal_session_valid;
    uint8_t modal_ui_valid;
    uint8_t pci_scan_complete;
    uint8_t hotplug_initialized;
    uint32_t hotplug_active;
    uint64_t hotplug_generation;
    uint64_t hotplug_quiet_ms;
    uint8_t autorun_enabled;
    uint8_t autorun_passed;
    uint64_t published_ns;
    uint64_t test_ready_ns;
    uint64_t violations;
} runtime_ready_snapshot_t;

void runtime_ready_init(void);
void runtime_ready_mark_pci_scan_complete(void);
bool runtime_ready_wait_and_publish(uint64_t timeout_ms,
                                    uint64_t required_quiet_ms);
void runtime_ready_mark_testing(bool autorun_enabled);
void runtime_ready_mark_test_complete(bool autorun_enabled, bool passed);
bool runtime_ready_is_ready(void);
bool runtime_ready_is_test_ready(void);
bool runtime_ready_snapshot(runtime_ready_snapshot_t *out);
bool runtime_ready_validate(uint64_t *out_violations);

#endif
