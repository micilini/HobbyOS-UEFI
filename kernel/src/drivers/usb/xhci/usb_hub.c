

#include "usb_hub.h"
#include "xhci.h"
#include "../../../core/spinlock.h"

extern int xhci_control_transfer(uint8_t slot_id, usb_setup_packet_t *setup, void *data);
extern int xhci_set_configuration(uint8_t slot_id, uint8_t config_value);
extern int xhci_get_descriptor_device(uint8_t slot_id, usb_device_descriptor_t *out_desc, uint16_t len);
extern void *xhci_get_config_descriptor(uint8_t slot_id, uint16_t *out_len);

extern int xhci_configure_device_with_context(int port_id, int speed_id, usb_device_context_t *ctx);
extern void console_write_debug(const char *str);
extern void console_print_dec_debug(uint64_t val);
extern void console_print_hex_debug(uint64_t val);
extern void console_set_color_debug(uint32_t fg, uint32_t bg);
extern void timer_sleep(uint32_t ms);
extern void *kmalloc_aligned(size_t size, size_t align);
extern void memset(void *dst, int val, size_t n);
extern void memcpy(void *dst, const void *src, size_t n);

#define CONSOLE_COLOR_WHITE 0xFFFFFFFF
#define CONSOLE_COLOR_BLACK 0xFF000000
#define CONSOLE_COLOR_GREEN 0xFF00FF00
#define CONSOLE_COLOR_CYAN 0xFF00FFFF
#define CONSOLE_COLOR_YELLOW 0xFFFFFF00
#define CONSOLE_COLOR_ORANGE 0xFFFFA500

static usb_hub_info_t usb_hub_list[USB_HUB_MAX_HUBS];
static int usb_hub_count = 0;

static spinlock_t g_usb_hub_lock;

static uint8_t g_usb_hub_initializing[USB_HUB_MAX_HUBS];

void usb_hub_init(void)
{
    spinlock_init(&g_usb_hub_lock);

    memset(usb_hub_list, 0, sizeof(usb_hub_list));
    memset(g_usb_hub_initializing, 0, sizeof(g_usb_hub_initializing));
    usb_hub_count = 0;

    console_write_debug("[USB_HUB] Hub subsystem initialized\n");
}

int usb_is_hub_device(uint8_t dev_class)
{
    return (dev_class == USB_CLASS_HUB) ? 1 : 0;
}

usb_hub_info_t *usb_hub_get_list(void)
{
    return usb_hub_list;
}

int usb_hub_get_count(void)
{
    int count;
    irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
    count = usb_hub_count;
    spin_unlock_irqrestore(&g_usb_hub_lock, flags);
    return count;
}

static int usb_hub_alloc_slot(void)
{
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++)
    {
        if (!usb_hub_list[i].valid && !g_usb_hub_initializing[i])
        {
            return i;
        }
    }
    return -1;
}

usb_hub_info_t *usb_hub_find_by_slot(uint8_t slot_id)
{
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++)
    {
        if (usb_hub_list[i].valid && usb_hub_list[i].slot_id == slot_id)
        {
            return &usb_hub_list[i];
        }
    }
    return NULL;
}

uint32_t usb_hub_calc_route_string(uint32_t parent_route, uint8_t port)
{

    uint8_t port_val = port;
    if (port_val > 15)
        port_val = 15;

    return (parent_route << 4) | (port_val & 0x0F);
}

uint8_t usb_hub_get_port_speed(uint16_t port_status, int is_usb3_hub)
{
    if (is_usb3_hub)
    {

        return USB_SPEED_SUPER;
    }

    if (port_status & HUB_PORT_STATUS_HIGH_SPEED)
    {
        return USB_SPEED_HIGH;
    }
    else if (port_status & HUB_PORT_STATUS_LOW_SPEED)
    {
        return USB_SPEED_LOW;
    }
    else
    {
        return USB_SPEED_FULL;
    }
}

int usb_hub_needs_tt(uint8_t device_speed, uint8_t parent_hub_speed)
{

    if (device_speed <= USB_SPEED_FULL && parent_hub_speed == USB_SPEED_HIGH)
    {
        return 1;
    }
    return 0;
}

int usb_hub_get_descriptor(uint8_t slot_id, int is_usb3, void *out_desc)
{
    usb_setup_packet_t setup;
    uint8_t buffer[16];

    memset(buffer, 0, sizeof(buffer));

    setup.RequestType = 0xA0;
    setup.Request = HUB_REQUEST_GET_DESCRIPTOR;
    setup.Value = (is_usb3 ? USB_HUB3_DESCRIPTOR_TYPE : USB_HUB_DESCRIPTOR_TYPE) << 8;
    setup.Index = 0;
    setup.Length = is_usb3 ? 12 : 9;

    if (!xhci_control_transfer(slot_id, &setup, buffer))
    {
        console_write_debug("[USB_HUB] Failed to get hub descriptor\n");
        return 0;
    }

    if (out_desc)
    {
        memcpy(out_desc, buffer, setup.Length);
    }

    return 1;
}

int usb_hub_get_port_status(uint8_t slot_id, uint8_t port, usb_hub_port_status_t *out_status)
{
    usb_setup_packet_t setup;
    uint8_t buffer[4];

    memset(buffer, 0, sizeof(buffer));

    setup.RequestType = 0xA3;
    setup.Request = HUB_REQUEST_GET_STATUS;
    setup.Value = 0;
    setup.Index = port;
    setup.Length = 4;

    if (!xhci_control_transfer(slot_id, &setup, buffer))
    {
        console_write_debug("[USB_HUB] Failed to get port ");
        console_print_dec_debug(port);
        console_write_debug(" status\n");
        return 0;
    }

    if (out_status)
    {
        out_status->wPortStatus = (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
        out_status->wPortChange = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
    }

    return 1;
}

int usb_hub_set_port_feature(uint8_t slot_id, uint8_t port, uint8_t feature)
{
    usb_setup_packet_t setup;

    setup.RequestType = 0x23;
    setup.Request = HUB_REQUEST_SET_FEATURE;
    setup.Value = feature;
    setup.Index = port;
    setup.Length = 0;

    return xhci_control_transfer(slot_id, &setup, NULL);
}

int usb_hub_clear_port_feature(uint8_t slot_id, uint8_t port, uint8_t feature)
{
    usb_setup_packet_t setup;

    setup.RequestType = 0x23;
    setup.Request = HUB_REQUEST_CLEAR_FEATURE;
    setup.Value = feature;
    setup.Index = port;
    setup.Length = 0;

    return xhci_control_transfer(slot_id, &setup, NULL);
}

int usb_hub_set_depth(uint8_t slot_id, uint8_t depth)
{
    usb_setup_packet_t setup;

    setup.RequestType = 0x20;
    setup.Request = HUB_REQUEST_SET_HUB_DEPTH;
    setup.Value = depth;
    setup.Index = 0;
    setup.Length = 0;

    if (!xhci_control_transfer(slot_id, &setup, NULL))
    {
        console_write_debug("[USB_HUB] Failed to set hub depth to ");
        console_print_dec_debug(depth);
        console_write_debug("\n");
        return 0;
    }

    console_write_debug("[USB_HUB] Set hub depth to ");
    console_print_dec_debug(depth);
    console_write_debug("\n");

    return 1;
}

int usb_hub_reset_port(uint8_t slot_id, uint8_t port)
{
    usb_hub_port_status_t status;
    int timeout;

    console_write_debug("[USB_HUB] Resetting port ");
    console_print_dec_debug(port);
    console_write_debug("...\n");

    if (!usb_hub_set_port_feature(slot_id, port, HUB_PORT_RESET))
    {
        console_write_debug("[USB_HUB] Failed to start port reset\n");
        return 0;
    }

    timeout = 100;
    while (timeout > 0)
    {
        timer_sleep(10);
        timeout -= 10;

        if (!usb_hub_get_port_status(slot_id, port, &status))
        {
            continue;
        }

        if (!(status.wPortStatus & HUB_PORT_STATUS_RESET))
        {
            break;
        }
    }

    if (timeout <= 0)
    {
        console_write_debug("[USB_HUB] Port reset timeout\n");
        return 0;
    }

    usb_hub_clear_port_feature(slot_id, port, HUB_C_PORT_RESET);

    if (status.wPortChange & HUB_PORT_STATUS_CONNECTION)
    {
        usb_hub_clear_port_feature(slot_id, port, HUB_C_PORT_CONNECTION);
    }

    timer_sleep(20);

    console_write_debug("[USB_HUB] Port reset complete\n");
    return 1;
}

int usb_hub_power_on_ports(uint8_t slot_id, uint8_t num_ports)
{
    int powered = 0;

    for (uint8_t port = 1; port <= num_ports; port++)
    {
        if (usb_hub_set_port_feature(slot_id, port, HUB_PORT_POWER))
        {
            powered++;
        }
    }

    timer_sleep(100);

    return powered;
}

int usb_hub_build_device_context(
    uint8_t parent_hub_slot,
    uint8_t port_on_hub,
    uint8_t device_speed,
    usb_device_context_t *out_ctx)
{
    usb_hub_info_t *parent_hub;

    if (!out_ctx)
        return 0;

    memset(out_ctx, 0, sizeof(usb_device_context_t));

    parent_hub = usb_hub_find_by_slot(parent_hub_slot);
    if (!parent_hub)
    {
        console_write_debug("[USB_HUB] Parent hub not found for slot ");
        console_print_dec_debug(parent_hub_slot);
        console_write_debug("\n");
        return 0;
    }

    out_ctx->route_string = usb_hub_calc_route_string(parent_hub->route_string, port_on_hub);

    out_ctx->root_port = parent_hub->root_port;

    out_ctx->hub_depth = parent_hub->hub_depth + 1;

    if (out_ctx->hub_depth > USB_HUB_MAX_DEPTH)
    {
        console_write_debug("[USB_HUB] Maximum hub depth exceeded!\n");
        return 0;
    }

    out_ctx->parent_hub_slot = parent_hub_slot;
    out_ctx->port_on_parent = port_on_hub;
    out_ctx->speed = device_speed;

    if (device_speed <= USB_SPEED_FULL && !parent_hub->is_usb3)
    {

        out_ctx->tt_hub_slot_id = parent_hub_slot;
        out_ctx->tt_port_num = port_on_hub;

        console_write_debug("[USB_HUB] Device needs TT: hub_slot=");
        console_print_dec_debug(out_ctx->tt_hub_slot_id);
        console_write_debug(" port=");
        console_print_dec_debug(out_ctx->tt_port_num);
        console_write_debug("\n");
    }

    console_write_debug("[USB_HUB] Device context: route=0x");
    console_print_hex_debug(out_ctx->route_string);
    console_write_debug(" root_port=");
    console_print_dec_debug(out_ctx->root_port);
    console_write_debug(" depth=");
    console_print_dec_debug(out_ctx->hub_depth);
    console_write_debug("\n");

    return 1;
}

int usb_hub_enumerate(
    uint8_t slot_id,
    uint8_t root_port,
    uint32_t route_string,
    uint8_t hub_depth,
    uint8_t parent_hub_slot,
    uint8_t port_on_parent,
    int is_usb3)
{
    int hub_idx;
    uint8_t num_ports;
    uint16_t conf_len;
    void *conf_buf;

    uint8_t config_value = 0;
    uint8_t interface_num = 0;
    uint8_t parent_hub_idx = 0xFF;
    uint8_t is_usb3_flag = is_usb3 ? 1 : 0;

    usb_hub_descriptor_t hub_desc;
    usb3_hub_descriptor_t hub3_desc;

    console_set_color_debug(CONSOLE_COLOR_YELLOW, CONSOLE_COLOR_BLACK);
    console_write_debug("\n[USB_HUB] === Enumerating Hub ===\n");
    console_write_debug("[USB_HUB] Slot: ");
    console_print_dec_debug(slot_id);
    console_write_debug(" Root Port: ");
    console_print_dec_debug(root_port);
    console_write_debug(" Route: 0x");
    console_print_hex_debug(route_string);
    console_write_debug(" Depth: ");
    console_print_dec_debug(hub_depth);
    console_write_debug("\n");
    console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

    if (hub_depth > USB_HUB_MAX_DEPTH)
    {
        console_write_debug("[USB_HUB] ERROR: Maximum hub depth exceeded!\n");
        return -1;
    }

    {
        irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);

        hub_idx = usb_hub_alloc_slot();
        if (hub_idx < 0)
        {
            spin_unlock_irqrestore(&g_usb_hub_lock, flags);
            console_write_debug("[USB_HUB] ERROR: No free hub slots!\n");
            return -1;
        }

        g_usb_hub_initializing[hub_idx] = 1;

        if (parent_hub_slot != 0)
        {
            for (int i = 0; i < USB_HUB_MAX_HUBS; i++)
            {
                if (usb_hub_list[i].valid && usb_hub_list[i].slot_id == parent_hub_slot)
                {
                    parent_hub_idx = (uint8_t)i;
                    break;
                }
            }
        }

        spin_unlock_irqrestore(&g_usb_hub_lock, flags);
    }

    conf_buf = xhci_get_config_descriptor(slot_id, &conf_len);
    if (conf_buf && conf_len >= 9)
    {
        usb_config_descriptor_t *conf = (usb_config_descriptor_t *)conf_buf;
        config_value = conf->bConfigurationValue;

        uint8_t *ptr = (uint8_t *)conf_buf;
        uint16_t offset = conf->bLength;

        while (offset < conf_len)
        {
            uint8_t len = ptr[offset];
            uint8_t type = ptr[offset + 1];

            if (len == 0)
                break;

            if (type == USB_DESC_INTERFACE && offset + 9 <= conf_len)
            {
                usb_interface_descriptor_t *iface = (usb_interface_descriptor_t *)(ptr + offset);
                if (iface->bInterfaceClass == USB_CLASS_HUB)
                {
                    interface_num = iface->bInterfaceNumber;
                    break;
                }
            }

            offset += len;
        }
    }

    if (config_value == 0)
        config_value = 1;

    console_write_debug("[USB_HUB] Setting configuration ");
    console_print_dec_debug(config_value);
    console_write_debug("...\n");

    if (!xhci_set_configuration(slot_id, config_value))
    {
        console_write_debug("[USB_HUB] Failed to set configuration!\n");

        irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
        g_usb_hub_initializing[hub_idx] = 0;
        spin_unlock_irqrestore(&g_usb_hub_lock, flags);

        return -1;
    }

    if (is_usb3_flag)
    {
        if (!usb_hub_set_depth(slot_id, hub_depth))
        {
            console_write_debug("[USB_HUB] Warning: Failed to set hub depth\n");
        }
    }

    if (is_usb3_flag)
    {
        if (usb_hub_get_descriptor(slot_id, 1, &hub3_desc))
            num_ports = hub3_desc.bNbrPorts;
        else
        {
            console_write_debug("[USB_HUB] Failed to get USB3 hub descriptor\n");
            num_ports = 4;
        }
    }
    else
    {
        if (usb_hub_get_descriptor(slot_id, 0, &hub_desc))
            num_ports = hub_desc.bNbrPorts;
        else
        {
            console_write_debug("[USB_HUB] Failed to get USB2 hub descriptor\n");
            num_ports = 4;
        }
    }

    if (num_ports > USB_HUB_MAX_PORTS)
        num_ports = USB_HUB_MAX_PORTS;

    console_write_debug("[USB_HUB] Hub has ");
    console_print_dec_debug(num_ports);
    console_write_debug(" ports\n");

    console_write_debug("[USB_HUB] Powering on ports...\n");
    usb_hub_power_on_ports(slot_id, num_ports);

    for (uint8_t port = 1; port <= num_ports; port++)
    {
        usb_hub_clear_port_feature(slot_id, port, HUB_C_PORT_CONNECTION);
    }

    {
        irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);

        usb_hub_info_t *hub = &usb_hub_list[hub_idx];
        memset(hub, 0, sizeof(usb_hub_info_t));

        hub->valid = 1;
        hub->slot_id = slot_id;
        hub->hub_depth = hub_depth;
        hub->num_ports = num_ports;

        hub->root_port = root_port;
        hub->route_string = route_string;

        hub->parent_hub_idx = parent_hub_idx;
        hub->port_on_parent = port_on_parent;

        hub->is_usb3 = is_usb3_flag;
        hub->interface_num = interface_num;
        hub->config_value = config_value;

        usb_hub_count++;

        g_usb_hub_initializing[hub_idx] = 0;

        spin_unlock_irqrestore(&g_usb_hub_lock, flags);
    }

    console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
    console_write_debug("[USB_HUB] Hub initialized successfully (index ");
    console_print_dec_debug(hub_idx);
    console_write_debug(")\n");
    console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

    usb_hub_probe_ports(hub_idx);

    return hub_idx;
}

int usb_hub_probe_ports(int hub_idx)
{
    usb_hub_info_t *hub;
    usb_hub_port_status_t status;
    int devices_found = 0;

    if (hub_idx < 0 || hub_idx >= USB_HUB_MAX_HUBS)
        return 0;

    usb_hub_info_t hub_snap;

    {
        irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);

        hub = &usb_hub_list[hub_idx];
        if (!hub->valid)
        {
            spin_unlock_irqrestore(&g_usb_hub_lock, flags);
            return 0;
        }

        memcpy(&hub_snap, hub, sizeof(usb_hub_info_t));
        spin_unlock_irqrestore(&g_usb_hub_lock, flags);
    }

    console_write_debug("\n[USB_HUB] Probing ");
    console_print_dec_debug(hub_snap.num_ports);
    console_write_debug(" ports on hub slot ");
    console_print_dec_debug(hub_snap.slot_id);
    console_write_debug("...\n");

    for (uint8_t port = 1; port <= hub_snap.num_ports; port++)
    {
        console_write_debug("[USB_HUB] Checking port ");
        console_print_dec_debug(port);
        console_write_debug("...");

        if (!usb_hub_get_port_status(hub_snap.slot_id, port, &status))
        {
            console_write_debug(" (failed to get status)\n");
            continue;
        }

        if (!(status.wPortStatus & HUB_PORT_STATUS_CONNECTION))
        {
            console_write_debug(" (no device)\n");
            continue;
        }

        console_write_debug(" CONNECTED!\n");

        if (!usb_hub_reset_port(hub_snap.slot_id, port))
        {
            console_write_debug("[USB_HUB] Port reset failed, skipping\n");
            continue;
        }

        if (!usb_hub_get_port_status(hub_snap.slot_id, port, &status))
        {
            continue;
        }

        if (!(status.wPortStatus & HUB_PORT_STATUS_ENABLE))
        {
            console_write_debug("[USB_HUB] Port not enabled after reset\n");
            continue;
        }

        uint8_t speed = usb_hub_get_port_speed(status.wPortStatus, hub_snap.is_usb3);

        console_write_debug("[USB_HUB] Device speed: ");
        switch (speed)
        {
        case USB_SPEED_LOW:
            console_write_debug("Low");
            break;
        case USB_SPEED_FULL:
            console_write_debug("Full");
            break;
        case USB_SPEED_HIGH:
            console_write_debug("High");
            break;
        case USB_SPEED_SUPER:
            console_write_debug("Super");
            break;
        default:
            console_write_debug("Unknown");
            break;
        }
        console_write_debug("\n");

        usb_device_context_t dev_ctx;
        dev_ctx.route_string = usb_hub_calc_route_string(hub_snap.route_string, port);
        dev_ctx.root_port = hub_snap.root_port;
        dev_ctx.hub_depth = hub_snap.hub_depth + 1;
        dev_ctx.parent_hub_slot = hub_snap.slot_id;
        dev_ctx.port_on_parent = port;
        dev_ctx.speed = speed;
        dev_ctx.slot_id = 0;

        if ((speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) && !hub_snap.is_usb3)
        {
            dev_ctx.tt_hub_slot_id = hub_snap.slot_id;
            dev_ctx.tt_port_num = port;

            console_write_debug("[USB_HUB] TT needed: hub_slot=");
            console_print_dec_debug(hub_snap.slot_id);
            console_write_debug(" tt_port=");
            console_print_dec_debug(port);
            console_write_debug("\n");
        }
        else
        {
            dev_ctx.tt_hub_slot_id = 0;
            dev_ctx.tt_port_num = 0;
        }

        console_write_debug("[USB_HUB] Configuring device: route=0x");
        console_print_hex_debug(dev_ctx.route_string);
        console_write_debug(" root=");
        console_print_dec_debug(dev_ctx.root_port);
        console_write_debug("\n");

        int slot_id = xhci_configure_device_with_context(
            hub_snap.root_port - 1,
            speed,
            &dev_ctx);

        if (slot_id > 0)
        {
            {
                irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
                if (usb_hub_list[hub_idx].valid)
                    usb_hub_list[hub_idx].child_slot[port] = (uint8_t)slot_id;
                spin_unlock_irqrestore(&g_usb_hub_lock, flags);
            }

            devices_found++;

            console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
            console_write_debug("[USB_HUB] Device configured at slot ");
            console_print_dec_debug(slot_id);
            console_write_debug("\n");
            console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
        }
        else
        {
            console_write_debug("[USB_HUB] Failed to configure device\n");
        }
    }

    console_write_debug("[USB_HUB] Found ");
    console_print_dec_debug(devices_found);
    console_write_debug(" devices on hub\n\n");

    return devices_found;
}

void usb_hub_print_tree(void)
{
    console_write_debug("\n=== USB Hub Tree ===\n");

    for (int i = 0; i < USB_HUB_MAX_HUBS; i++)
    {
        usb_hub_info_t hub_snap;

        {
            irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);

            if (!usb_hub_list[i].valid)
            {
                spin_unlock_irqrestore(&g_usb_hub_lock, flags);
                continue;
            }

            memcpy(&hub_snap, &usb_hub_list[i], sizeof(usb_hub_info_t));
            spin_unlock_irqrestore(&g_usb_hub_lock, flags);
        }

        for (int d = 0; d < hub_snap.hub_depth; d++)
            console_write_debug("  ");

        console_write_debug("Hub[");
        console_print_dec_debug(i);
        console_write_debug("] slot=");
        console_print_dec_debug(hub_snap.slot_id);
        console_write_debug(" ports=");
        console_print_dec_debug(hub_snap.num_ports);
        console_write_debug(" route=0x");
        console_print_hex_debug(hub_snap.route_string);
        console_write_debug(hub_snap.is_usb3 ? " (USB3)" : " (USB2)");
        console_write_debug("\n");

        for (int p = 1; p <= hub_snap.num_ports; p++)
        {
            if (hub_snap.child_slot[p] != 0)
            {
                for (int d = 0; d <= hub_snap.hub_depth; d++)
                    console_write_debug("  ");

                console_write_debug("Port ");
                console_print_dec_debug(p);
                console_write_debug(": slot ");
                console_print_dec_debug(hub_snap.child_slot[p]);
                console_write_debug("\n");
            }
        }
    }

    console_write_debug("====================\n\n");
}