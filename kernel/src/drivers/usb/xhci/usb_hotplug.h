#ifndef USB_HOTPLUG_H
#define USB_HOTPLUG_H

#include <stdint.h>
#include <stdbool.h>
#include "../../../core/timers.h"
#include "usb_hub.h"
#include "usb_ownership.h"

typedef enum
{
    HP_STATE_IDLE = 0,
    HP_STATE_WAIT_CONNECTION_STABLE,
    HP_STATE_RESET_PORT,
    HP_STATE_WAIT_RESET,
    HP_STATE_ADDRESS_DEVICE,
    HP_STATE_CONFIGURE_DEVICE,
    HP_STATE_DONE,
    HP_STATE_ERROR
} hp_state_t;

typedef struct
{
    hp_state_t state;
    uint8_t root_port_1based;
    uint8_t retries;
    uint32_t flags;
    uint8_t slot_id;
    uint8_t readiness_active;
    usb_root_owner_t owner;
    uint64_t pending_ticket, pending_generation;
    uint8_t retired_slot;
} hp_port_context_t;

typedef struct
{
    uint8_t initialized;
    bool failed;
    uint64_t poll_completions;
    uint32_t active_enumerations;
    uint64_t activity_generation;
    uint64_t last_activity_ns;
} usb_hotplug_readiness_snapshot_t;

void usb_hotplug_init(void);

void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc);

bool usb_hotplug_begin_root(uint8_t port, uint64_t *generation);
bool usb_hotplug_root_address(uint8_t port, uint64_t generation, uint8_t slot);
bool usb_hotplug_finish_root(uint8_t port, uint64_t generation, bool success);
bool usb_hotplug_root_current(uint8_t port, uint64_t generation);
bool usb_hotplug_root_identity(uint8_t port, uint64_t *generation);

void usb_hotplug_process_pending(void);
bool usb_hotplug_readiness_snapshot(usb_hotplug_readiness_snapshot_t *out);
#ifdef HOBBYOS_SELFTEST
bool usb_hotplug_owner_snapshot(uint8_t port, usb_root_owner_t *out);
#endif

#endif
