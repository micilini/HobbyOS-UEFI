#ifndef USB_HOTPLUG_H
#define USB_HOTPLUG_H

#include <stdint.h>

void usb_hotplug_init(void);

void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc);

void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id);

int usb_hotplug_process_pending(void);

#endif