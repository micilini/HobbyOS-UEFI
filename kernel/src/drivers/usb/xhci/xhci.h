#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include <stddef.h>

#define XHCI_KBD_PIPE_DEPTH 1

typedef struct
{
    uint8_t CapLength;
    uint8_t Reserved;
    uint16_t HciVersion;
    uint32_t HcsParams1;
    uint32_t HcsParams2;
    uint32_t HcsParams3;
    uint32_t HccParams1;
    uint32_t DbOff;
    uint32_t RtsOff;
    uint32_t HccParams2;
} __attribute__((packed)) xhci_cap_regs_t;

#define XHCI_MAX_SLOTS(x) ((x) & 0xFF)
#define XHCI_MAX_PORTS(x) (((x) >> 24) & 0xFF)

typedef struct
{
    uint32_t UsbCmd;
    uint32_t UsbSts;
    uint32_t PageSize;
    uint32_t Reserved1[2];
    uint32_t DnCtrl;
    uint64_t Crcr;
    uint32_t Reserved2[4];
    uint64_t Dcbaap;
    uint32_t Config;
} __attribute__((packed)) xhci_op_regs_t;

#define USBCMD_RUN_STOP (1 << 0)
#define USBCMD_HC_RESET (1 << 1)
#define USBCMD_INTE (1 << 2)
#define USBSTS_HC_HALTED (1 << 0)
#define USBSTS_EINT (1 << 3)

typedef struct
{
    uint32_t PortSC;
    uint32_t PortPMSC;
    uint32_t PortLI;
    uint32_t PortHLPMC;
} __attribute__((packed)) xhci_port_regs_t;

typedef struct
{
    volatile uint32_t Iman;
    volatile uint32_t Imod;
    volatile uint32_t Erstsz;
    volatile uint32_t Reserved;
    volatile uint64_t Erstba;
    volatile uint64_t Erdp;
} __attribute__((packed)) xhci_interrupter_regs_t;

#define XHCI_IMAN_IP (1 << 0)
#define XHCI_IMAN_IE (1 << 1)

typedef struct
{
    uint32_t MicroFrameIndex;
    uint8_t Reserved[28];
    xhci_interrupter_regs_t Interrupters[];
} __attribute__((packed)) xhci_runtime_regs_t;

typedef struct
{
    uint32_t Target;
} __attribute__((packed)) xhci_doorbell_regs_t;

typedef struct
{
    uint64_t BaseAddress;
    uint32_t Size;
    uint32_t Reserved;
} __attribute__((packed)) xhci_erst_entry_t;

typedef struct
{
    uint32_t RouteString : 20;
    uint32_t Speed : 4;
    uint32_t RZ : 1;
    uint32_t MTT : 1;
    uint32_t Hub : 1;
    uint32_t ContextEntries : 5;

    uint16_t MaxExitLatency;
    uint8_t RootHubPortNum;
    uint8_t NumberPorts;

    uint8_t TTHubSlotId;
    uint8_t TTPortNum;
    uint16_t TTT : 2;
    uint16_t RsvdZ : 4;
    uint16_t InterruptTarget : 10;

    uint32_t DeviceAddress : 8;
    uint32_t RsvdZ2 : 19;
    uint32_t SlotState : 5;

    uint32_t Reserved[4];
} __attribute__((packed)) xhci_slot_context_t;

typedef struct
{
    uint32_t EpState : 3;
    uint32_t RsvdZ : 5;
    uint32_t Mult : 2;
    uint32_t MaxPrimaryStreams : 5;
    uint32_t LSA : 1;
    uint32_t Interval : 8;
    uint32_t MaxEsitHigh : 8;

    uint32_t RsvdZ2 : 1;
    uint32_t CErr : 2;
    uint32_t EpType : 3;
    uint32_t RsvdZ3 : 1;
    uint32_t HID : 1;
    uint32_t MaxBurstSize : 8;
    uint32_t MaxPacketSize : 16;

    uint64_t TRDequeuePtr;

    uint16_t AverageTrbLength;
    uint16_t MaxEsitLow;
    uint32_t Reserved[3];
} __attribute__((packed)) xhci_endpoint_context_t;

typedef struct
{
    uint64_t Parameter;
    uint32_t Status;
    uint32_t Control;
} __attribute__((packed)) xhci_trb_t;

#define TRB_TYPE_NORMAL 1
#define TRB_TYPE_SETUP_STAGE 2
#define TRB_TYPE_DATA_STAGE 3
#define TRB_TYPE_STATUS_STAGE 4
#define TRB_TYPE_LINK 6
#define TRB_TYPE_ENABLE_SLOT 9
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_CONFIG_EP 12
#define TRB_TYPE_EVALUATE_CONTEXT 13
#define TRB_TYPE_NOOP 23

#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_CMD_COMPLETE 33
#define TRB_TYPE_PORT_STATUS 34

#define USB_REQ_TYPE_STANDARD (0x00 << 5)
#define USB_REQ_TYPE_CLASS (0x01 << 5)
#define USB_REQ_TYPE_VENDOR (0x02 << 5)

#define USB_REQ_RECIP_DEVICE 0x00
#define USB_REQ_RECIP_INTERFACE 0x01
#define USB_REQ_RECIP_ENDPOINT 0x02

#define USB_DIR_OUT 0
#define USB_DIR_IN 0x80

#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIGURATION 0x02
#define USB_DESC_STRING 0x03
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05

#define USB_REQ_SET_INTERFACE 0x0B
#define USB_REQ_SET_PROTOCOL 0x0B

typedef struct
{
    uint8_t RequestType;
    uint8_t Request;
    uint16_t Value;
    uint16_t Index;
    uint16_t Length;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct
{
    uint8_t Length;
    uint8_t DescriptorType;
    uint16_t BcdUSB;
    uint8_t DeviceClass;
    uint8_t DeviceSubclass;
    uint8_t DeviceProtocol;
    uint8_t MaxPacketSize0;
    uint16_t VendorID;
    uint16_t ProductID;
    uint16_t BcdDevice;
    uint8_t ManufacturerIdx;
    uint8_t ProductIdx;
    uint8_t SerialIdx;
    uint8_t NumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed)) usb_config_descriptor_t;

typedef struct
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

typedef struct
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wDescriptorLength;
} __attribute__((packed)) usb_hid_descriptor_t;

#define XHCI_KBD_EP_CANDIDATES 8

typedef struct
{
    uint8_t found;

    uint8_t config_value;

    uint8_t interface_num;
    uint8_t interface_alt;
    uint8_t endpoint_addr;
    uint16_t endpoint_mps;
    uint8_t endpoint_interval;

    uint8_t cand_count;
    uint8_t cand_iface[XHCI_KBD_EP_CANDIDATES];
    uint8_t cand_alt[XHCI_KBD_EP_CANDIDATES];
    uint8_t cand_epaddr[XHCI_KBD_EP_CANDIDATES];
    uint16_t cand_mps[XHCI_KBD_EP_CANDIDATES];
    uint8_t cand_interval[XHCI_KBD_EP_CANDIDATES];
} usb_device_info_t;

typedef struct
{
    uint64_t pci_base_address;
    uint64_t virtual_base_address;

    xhci_cap_regs_t *cap_regs;
    xhci_op_regs_t *op_regs;
    xhci_runtime_regs_t *run_regs;
    xhci_doorbell_regs_t *db_regs;
    xhci_port_regs_t *port_regs;

    uint8_t max_slots;
    uint8_t max_ports;

    uint64_t *dcbaa;

    xhci_trb_t *cmd_ring_base;
    uint64_t cmd_ring_size;
    uint64_t cmd_ring_enqueue_idx;
    uint8_t cmd_ring_cycle_bit;

    xhci_erst_entry_t *erst;
    xhci_trb_t *event_ring;
    uint64_t event_ring_size;
    uint64_t event_ring_dequeue_idx;
    uint8_t event_ring_cycle_bit;

    xhci_trb_t *slot_ep0_rings[64];
    uint64_t slot_ep0_enqueue[64];
    uint8_t slot_ep0_cycle[64];

    xhci_trb_t *slot_kbd_rings[64];
    uint64_t slot_kbd_enqueue[64];
    uint8_t slot_kbd_cycle[64];
    uint8_t *slot_kbd_buffer[64];

    uint8_t *slot_kbd_buffers[64][XHCI_KBD_PIPE_DEPTH];
    uint64_t slot_kbd_buffers_phys[64][XHCI_KBD_PIPE_DEPTH];

    uint64_t slot_kbd_ring_phys[64];
    uint8_t *slot_kbd_trb_buf[64][256];

    uint8_t *slot_led_buffer[64];
    uint8_t slot_kbd_dci[64];

    uint8_t slot_kbd_prev_keys[64][6];
    uint8_t slot_kbd_prev_mods[64];

    uint8_t slot_kbd_repeat_key[64];
    uint8_t slot_kbd_repeat_mods[64];
    uint8_t slot_kbd_repeat_active[64];
    uint64_t slot_kbd_repeat_next_ms[64];

    uint32_t slot_kbd_pending[64];

} xhci_controller_t;

extern volatile uint32_t xhci_debug_flags;

#define XHCI_DBG_WATCH (1u << 0)
#define XHCI_DBG_DUMP (1u << 1)

void xhci_set_debug_flags(uint32_t flags);
uint32_t xhci_get_debug_flags(void);

void xhci_init(uint64_t base_address);
void xhci_handle_interrupt(void);
void xhci_process_events(void);
void xhci_poll_events(void);
void xhci_poll_keyboard_test(uint8_t slot_id);
void xhci_kbd_recover_poll(void);
int xhci_set_interface(uint8_t slot_id, uint8_t interface_num, uint8_t alt_setting);
int xhci_get_endpoint_status(uint8_t slot_id, uint8_t ep_addr, uint16_t *out_status);
int xhci_set_idle(uint8_t slot_id, uint8_t interface_num, uint8_t duration, uint8_t report_id);

/* =========================================================================
 * USB HUB Support
 * ========================================================================= */

#define USB_CLASS_HUB 0x09

/* Forward declaration - full definition in usb_hub.h */
struct usb_device_context;
typedef struct usb_device_context usb_device_context_t;

/* Hub support functions */
int xhci_configure_device_with_context(int port_id, int speed_id, usb_device_context_t *ctx);
int xhci_get_descriptor_device(uint8_t slot_id, usb_device_descriptor_t *out_desc, uint16_t len);
void *xhci_get_config_descriptor(uint8_t slot_id, uint16_t *out_len);

#endif /* XHCI_H */