

#ifndef USB_HUB_H
#define USB_HUB_H

#include <stdint.h>
#include <stddef.h>

#define USB_HUB_MAX_DEPTH 5

#define USB_HUB_MAX_PORTS 15

#define USB_HUB_MAX_HUBS 16

#define USB_CLASS_HUB 0x09

#define USB_HUB_DESCRIPTOR_TYPE 0x29
#define USB_HUB3_DESCRIPTOR_TYPE 0x2A

#define HUB_REQUEST_GET_STATUS 0
#define HUB_REQUEST_CLEAR_FEATURE 1
#define HUB_REQUEST_SET_FEATURE 3
#define HUB_REQUEST_GET_DESCRIPTOR 6
#define HUB_REQUEST_SET_DESCRIPTOR 7
#define HUB_REQUEST_CLEAR_TT_BUFFER 8
#define HUB_REQUEST_RESET_TT 9
#define HUB_REQUEST_GET_TT_STATE 10
#define HUB_REQUEST_STOP_TT 11
#define HUB_REQUEST_SET_HUB_DEPTH 12

#define HUB_PORT_CONNECTION 0
#define HUB_PORT_ENABLE 1
#define HUB_PORT_SUSPEND 2
#define HUB_PORT_OVER_CURRENT 3
#define HUB_PORT_RESET 4
#define HUB_PORT_LINK_STATE 5
#define HUB_PORT_POWER 8
#define HUB_PORT_LOW_SPEED 9
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_ENABLE 17
#define HUB_C_PORT_SUSPEND 18
#define HUB_C_PORT_OVER_CURRENT 19
#define HUB_C_PORT_RESET 20

#define HUB_PORT_STATUS_CONNECTION (1 << 0)
#define HUB_PORT_STATUS_ENABLE (1 << 1)
#define HUB_PORT_STATUS_SUSPEND (1 << 2)
#define HUB_PORT_STATUS_OVER_CURRENT (1 << 3)
#define HUB_PORT_STATUS_RESET (1 << 4)
#define HUB_PORT_STATUS_POWER (1 << 8)
#define HUB_PORT_STATUS_LOW_SPEED (1 << 9)
#define HUB_PORT_STATUS_HIGH_SPEED (1 << 10)
#define HUB_PORT_STATUS_TEST (1 << 11)
#define HUB_PORT_STATUS_INDICATOR (1 << 12)

#define USB_SPEED_INVALID 0
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4
#define USB_SPEED_SUPER_PLUS 5

typedef struct __attribute__((packed))
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;

} usb_hub_descriptor_t;

typedef struct __attribute__((packed))
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
    uint8_t bHubHdrDecLat;
    uint16_t wHubDelay;
    uint16_t DeviceRemovable;
} usb3_hub_descriptor_t;

typedef struct __attribute__((packed))
{
    uint16_t wPortStatus;
    uint16_t wPortChange;
} usb_hub_port_status_t;

typedef struct
{
    uint8_t valid;
    uint8_t slot_id;
    uint8_t hub_depth;
    uint8_t num_ports;

    uint8_t root_port;
    uint32_t route_string;

    uint8_t parent_hub_idx;
    uint8_t port_on_parent;

    uint8_t is_usb3;
    uint8_t tt_think_time;

    uint8_t interface_num;
    uint8_t config_value;

    uint8_t child_slot[USB_HUB_MAX_PORTS + 1];

} usb_hub_info_t;

typedef struct usb_device_context
{
    uint8_t slot_id;
    uint8_t root_port;
    uint32_t route_string;
    uint8_t hub_depth;

    uint8_t tt_hub_slot_id;
    uint8_t tt_port_num;

    uint8_t parent_hub_slot;
    uint8_t port_on_parent;

    uint8_t speed;

} usb_device_context_t;

void usb_hub_init(void);

int usb_is_hub_device(uint8_t dev_class);

int usb_hub_enumerate(
    uint8_t slot_id,
    uint8_t root_port,
    uint32_t route_string,
    uint8_t hub_depth,
    uint8_t parent_hub_slot,
    uint8_t port_on_parent,
    int is_usb3);

int usb_hub_get_descriptor(uint8_t slot_id, int is_usb3, void *out_desc);

int usb_hub_get_port_status(uint8_t slot_id, uint8_t port, usb_hub_port_status_t *out_status);

int usb_hub_set_port_feature(uint8_t slot_id, uint8_t port, uint8_t feature);

int usb_hub_clear_port_feature(uint8_t slot_id, uint8_t port, uint8_t feature);

int usb_hub_reset_port(uint8_t slot_id, uint8_t port);

int usb_hub_set_depth(uint8_t slot_id, uint8_t depth);

int usb_hub_power_on_ports(uint8_t slot_id, uint8_t num_ports);

uint32_t usb_hub_calc_route_string(uint32_t parent_route, uint8_t port);

uint8_t usb_hub_get_port_speed(uint16_t port_status, int is_usb3_hub);

usb_hub_info_t *usb_hub_find_by_slot(uint8_t slot_id);

uint8_t usb_hub_get_root_port(usb_device_context_t *ctx);

int usb_hub_build_device_context(
    uint8_t parent_hub_slot,
    uint8_t port_on_hub,
    uint8_t device_speed,
    usb_device_context_t *out_ctx);

int usb_hub_needs_tt(uint8_t device_speed, uint8_t parent_hub_speed);

int usb_hub_probe_ports(int hub_idx);

void usb_hub_print_tree(void);

usb_hub_info_t *usb_hub_get_list(void);

int usb_hub_get_count(void);

#endif