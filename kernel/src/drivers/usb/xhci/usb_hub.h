/*
 * usb_hub.h - USB Hub Support for HobbyOS
 *
 * Provides support for USB Hub enumeration and device discovery
 * behind hubs, including nested hubs.
 */

#ifndef USB_HUB_H
#define USB_HUB_H

#include <stdint.h>
#include <stddef.h>

/* Maximum hub depth allowed (USB spec says max 5 for USB 2.0, 5 for USB 3.0) */
#define USB_HUB_MAX_DEPTH 5

/* Maximum number of ports per hub */
#define USB_HUB_MAX_PORTS 15

/* Maximum hubs we can track */
#define USB_HUB_MAX_HUBS 16

/* Hub class code */
#define USB_CLASS_HUB 0x09

/* Hub descriptor types */
#define USB_HUB_DESCRIPTOR_TYPE 0x29
#define USB_HUB3_DESCRIPTOR_TYPE 0x2A

/* Hub class-specific requests */
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

/* Hub port features (for SET_FEATURE/CLEAR_FEATURE) */
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

/* Hub port status bits */
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

/* USB speeds */
#define USB_SPEED_INVALID 0
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4
#define USB_SPEED_SUPER_PLUS 5

/* Hub descriptor (USB 2.0) */
typedef struct __attribute__((packed))
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood;
    uint8_t bHubContrCurrent;
    /* Variable length DeviceRemovable and PortPwrCtrlMask follow */
} usb_hub_descriptor_t;

/* Hub descriptor (USB 3.0) */
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

/* Hub port status (returned by GET_STATUS) */
typedef struct __attribute__((packed))
{
    uint16_t wPortStatus;
    uint16_t wPortChange;
} usb_hub_port_status_t;

/* Hub information structure - tracks each discovered hub */
typedef struct
{
    uint8_t valid;     /* Is this entry in use? */
    uint8_t slot_id;   /* XHCI slot ID of this hub */
    uint8_t hub_depth; /* Depth in the hub tree (0 = root hub) */
    uint8_t num_ports; /* Number of downstream ports */

    uint8_t root_port;     /* Root hub port number */
    uint32_t route_string; /* Route string to reach this hub */

    uint8_t parent_hub_idx; /* Index in usb_hub_list of parent hub (0xFF if root) */
    uint8_t port_on_parent; /* Port number on parent hub */

    uint8_t is_usb3;       /* Is this a USB 3.x hub? */
    uint8_t tt_think_time; /* Transaction translator think time */

    uint8_t interface_num; /* Interface number for hub */
    uint8_t config_value;  /* Configuration value */

    /* Child device tracking */
    uint8_t child_slot[USB_HUB_MAX_PORTS + 1]; /* Slot ID of device on each port (0 = none) */

} usb_hub_info_t;

/* Context for device enumeration through hub */
typedef struct usb_device_context
{
    uint8_t slot_id;
    uint8_t root_port;     /* Root hub port (1-based, as stored in slot context) */
    uint32_t route_string; /* Complete route string to reach device */
    uint8_t hub_depth;     /* Current hub depth */

    /* Transaction Translator info (for low/full speed behind high-speed hub) */
    uint8_t tt_hub_slot_id; /* Slot ID of TT hub (0 if N/A) */
    uint8_t tt_port_num;    /* Port on TT hub */

    /* Parent hub info */
    uint8_t parent_hub_slot; /* Slot ID of parent hub (0 if direct to root hub) */
    uint8_t port_on_parent;  /* Port number on parent hub */

    uint8_t speed; /* Device speed */

} usb_device_context_t;

/*
 * Initialize USB hub subsystem
 */
void usb_hub_init(void);

/*
 * Check if a device is a USB hub based on device descriptor
 *
 * @param dev_class     Device class from device descriptor
 * @return              1 if hub, 0 otherwise
 */
int usb_is_hub_device(uint8_t dev_class);

/*
 * Initialize and enumerate a USB hub
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param root_port     Root hub port (1-based)
 * @param route_string  Route string to reach this hub
 * @param hub_depth     Current depth in hub tree
 * @param parent_hub_slot Parent hub slot ID (0 if root hub)
 * @param port_on_parent Port on parent hub (1-based)
 * @param is_usb3       1 if USB 3.x hub, 0 otherwise
 * @return              Hub index on success, -1 on failure
 */
int usb_hub_enumerate(
    uint8_t slot_id,
    uint8_t root_port,
    uint32_t route_string,
    uint8_t hub_depth,
    uint8_t parent_hub_slot,
    uint8_t port_on_parent,
    int is_usb3);

/*
 * Get the hub descriptor from a hub device
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param is_usb3       1 for USB 3.x hub descriptor, 0 for USB 2.0
 * @param out_desc      Buffer to receive descriptor (at least 12 bytes)
 * @return              1 on success, 0 on failure
 */
int usb_hub_get_descriptor(uint8_t slot_id, int is_usb3, void *out_desc);

/*
 * Get port status from a hub
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param port          Port number (1-based)
 * @param out_status    Pointer to receive status
 * @return              1 on success, 0 on failure
 */
int usb_hub_get_port_status(uint8_t slot_id, uint8_t port, usb_hub_port_status_t *out_status);

/*
 * Set a feature on a hub port
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param port          Port number (1-based)
 * @param feature       Feature selector (HUB_PORT_xxx)
 * @return              1 on success, 0 on failure
 */
int usb_hub_set_port_feature(uint8_t slot_id, uint8_t port, uint8_t feature);

/*
 * Clear a feature on a hub port
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param port          Port number (1-based)
 * @param feature       Feature selector (HUB_PORT_xxx or HUB_C_PORT_xxx)
 * @return              1 on success, 0 on failure
 */
int usb_hub_clear_port_feature(uint8_t slot_id, uint8_t port, uint8_t feature);

/*
 * Reset a port on a hub
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param port          Port number (1-based)
 * @return              1 on success, 0 on failure
 */
int usb_hub_reset_port(uint8_t slot_id, uint8_t port);

/*
 * Set hub depth for USB 3.x hubs
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param depth         Hub depth value
 * @return              1 on success, 0 on failure
 */
int usb_hub_set_depth(uint8_t slot_id, uint8_t depth);

/*
 * Power on all ports of a hub
 *
 * @param slot_id       XHCI slot ID of the hub
 * @param num_ports     Number of ports on the hub
 * @return              Number of ports successfully powered on
 */
int usb_hub_power_on_ports(uint8_t slot_id, uint8_t num_ports);

/*
 * Calculate route string for a device on a hub port
 *
 * @param parent_route  Parent hub's route string
 * @param port          Port number on parent hub (1-based)
 * @return              New route string for device
 */
uint32_t usb_hub_calc_route_string(uint32_t parent_route, uint8_t port);

/*
 * Get device speed from hub port status
 *
 * @param port_status   Port status word from GET_STATUS
 * @param is_usb3_hub   1 if this is a USB 3.x hub
 * @return              USB_SPEED_xxx value
 */
uint8_t usb_hub_get_port_speed(uint16_t port_status, int is_usb3_hub);

/*
 * Find hub info by slot ID
 *
 * @param slot_id       XHCI slot ID
 * @return              Pointer to hub info, or NULL if not found
 */
usb_hub_info_t *usb_hub_find_by_slot(uint8_t slot_id);

/*
 * Get the root port for a device (following the hub chain up)
 *
 * @param ctx           Device context
 * @return              Root port number (1-based)
 */
uint8_t usb_hub_get_root_port(usb_device_context_t *ctx);

/*
 * Build device context for a device behind a hub
 *
 * @param parent_hub_slot  Slot ID of parent hub
 * @param port_on_hub      Port number on hub (1-based)
 * @param device_speed     Speed of the device
 * @param out_ctx          Pointer to receive device context
 * @return                 1 on success, 0 on failure
 */
int usb_hub_build_device_context(
    uint8_t parent_hub_slot,
    uint8_t port_on_hub,
    uint8_t device_speed,
    usb_device_context_t *out_ctx);

/*
 * Check if a device needs Transaction Translator
 *
 * @param device_speed      Speed of the device
 * @param parent_hub_speed  Speed of parent hub (or 0 for root hub)
 * @return                  1 if TT needed, 0 otherwise
 */
int usb_hub_needs_tt(uint8_t device_speed, uint8_t parent_hub_speed);

/*
 * Probe all ports of a hub and enumerate connected devices
 *
 * @param hub_idx       Index in hub list
 * @return              Number of devices found
 */
int usb_hub_probe_ports(int hub_idx);

/*
 * Debug: Print hub tree
 */
void usb_hub_print_tree(void);

/*
 * Get the global hub list (for external access if needed)
 */
usb_hub_info_t *usb_hub_get_list(void);

/*
 * Get count of registered hubs
 */
int usb_hub_get_count(void);

#endif /* USB_HUB_H */