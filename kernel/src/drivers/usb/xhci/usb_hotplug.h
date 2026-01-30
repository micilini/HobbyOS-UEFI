#ifndef USB_HOTPLUG_H
#define USB_HOTPLUG_H

#include <stdint.h>

/*
 * usb_hotplug.h - Minimal HOTPLUG logging for HobbyOS
 *
 * Current goal:
 *  - Detect connect/disconnect on xHCI root ports
 *  - Print a single log line per transition
 *
 * NOTE: This module intentionally does NOT re-enumerate devices yet.
 */

void usb_hotplug_init(void);

/* Called when we receive a Port Status Change Event for a root port (1-based). */
void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc);

/* Called when a device attached directly to a root port is configured and gets a Slot ID. */
void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id);

#endif /* USB_HOTPLUG_H */