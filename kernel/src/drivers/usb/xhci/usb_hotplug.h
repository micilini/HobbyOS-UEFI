#ifndef USB_HOTPLUG_H
#define USB_HOTPLUG_H

#include <stdint.h>

/*
 * usb_hotplug.h - USB Hotplug support for HobbyOS
 *
 * Features:
 *  - Detect connect/disconnect on xHCI root ports
 *  - Auto-enumerate new devices on connect
 *  - Print log lines for transitions
 */

void usb_hotplug_init(void);

/* Called when we receive a Port Status Change Event for a root port (1-based). */
void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc);

/* Called when a device attached directly to a root port is configured and gets a Slot ID. */
void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id);

/*
 * Process any pending hotplug enumerations.
 * This MUST be called from outside the event processing loop (e.g., from xhci_bottom_half).
 * Returns: number of devices processed
 */
int usb_hotplug_process_pending(void);

#endif /* USB_HOTPLUG_H */