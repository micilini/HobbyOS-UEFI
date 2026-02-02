#ifndef USB_HOTPLUG_H
#define USB_HOTPLUG_H

#include <stdint.h>
#include "../../../core/timers.h"

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
} hp_port_context_t;

void usb_hotplug_init(void);

void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc);

void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id);

void usb_hotplug_process_pending(void);

#endif