

#include "usb_hub.h"
#include "xhci.h"
#include "xhci_dma.h"
#include "usb_hotplug.h"
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
extern void memset(void *dst, int val, size_t n);
extern void memcpy(void *dst, const void *src, size_t n);

extern uint64_t timer_get_uptime_ms(void);
extern bool xhci_disable_slot(uint8_t slot_id);

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
static uint8_t g_usb_hub_probing[USB_HUB_MAX_HUBS];
#ifdef HOBBYOS_SELFTEST
bool usb_hub_snapshot(unsigned index, usb_hub_info_t *out)
{
    if (!out || index>=USB_HUB_MAX_HUBS)return false;
    irq_flags_t flags=spin_lock_irqsave(&g_usb_hub_lock);
    bool valid=usb_hub_list[index].valid;
    if(valid)*out=usb_hub_list[index];
    spin_unlock_irqrestore(&g_usb_hub_lock,flags);return valid;
}
#endif

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
        if (!usb_hub_list[i].valid && !g_usb_hub_initializing[i] && !g_usb_hub_probing[i])
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

void usb_hub_invalidate_by_slot(uint8_t slot_id)
{
    irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);

    for (int i = 0; i < USB_HUB_MAX_HUBS; i++)
    {
        if (usb_hub_list[i].valid && usb_hub_list[i].slot_id == slot_id)
        {
           
            for (int p = 1; p <= usb_hub_list[i].num_ports; p++)
            {
                uint8_t child = usb_hub_list[i].child_slot[p];
                if (child != 0)
                {
                   
                    for (int j = 0; j < USB_HUB_MAX_HUBS; j++)
                    {
                        if (usb_hub_list[j].valid && usb_hub_list[j].slot_id == child)
                        {
                            usb_hub_list[j].valid = 0;
                            usb_hub_count--;
                        }
                    }
                    usb_hub_list[i].child_slot[p] = 0;
                   
                }
            }

            usb_hub_list[i].valid = 0;
            usb_hub_count--;

            console_write_debug("[USB_HUB] Hub invalidated (slot ");
            console_print_dec_debug(slot_id);
            console_write_debug(")\n");
            break;
        }
    }

    spin_unlock_irqrestore(&g_usb_hub_lock, flags);
}

uint32_t usb_hub_calc_route_string(uint32_t parent_route, uint8_t port)
{
    if (!port || port > 15 || parent_route > 0xfffff) return UINT32_MAX;
    /* The tier nearest the root occupies the least-significant nibble. */
    for (unsigned shift = 0; shift < 20; shift += 4) {
        if (!(parent_route & (15u << shift))) {
            if (parent_route >> shift) return UINT32_MAX;
            return parent_route | ((uint32_t)port << shift);
        }
    }
    return UINT32_MAX;
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

    if ((device_speed == USB_SPEED_FULL || device_speed == USB_SPEED_LOW) && parent_hub_speed == USB_SPEED_HIGH)
    {
        return 1;
    }
    return 0;
}

int usb_hub_get_descriptor(uint8_t slot_id, int is_usb3, void *out_desc)
{
    usb_setup_packet_t setup;
    uint8_t *buffer = xhci_transfer_allocate(16);
    if (!buffer) return 0;

    setup.RequestType = 0xA0;
    setup.Request = HUB_REQUEST_GET_DESCRIPTOR;
    setup.Value = (is_usb3 ? USB_HUB3_DESCRIPTOR_TYPE : USB_HUB_DESCRIPTOR_TYPE) << 8;
    setup.Index = 0;
    setup.Length = is_usb3 ? 12 : 9;

    if (!xhci_control_transfer(slot_id, &setup, buffer))
    {
        console_write_debug("[USB_HUB] Failed to get hub descriptor\n");
        (void)xhci_transfer_release(buffer);
        return 0;
    }

    if (out_desc)
    {
        /* The USB2 wire reply includes removable/power masks beyond the
         * fixed parsed header. Those bytes do not belong to the caller. */
        memcpy(out_desc, buffer, is_usb3 ? sizeof(usb3_hub_descriptor_t) : sizeof(usb_hub_descriptor_t));
    }

    (void)xhci_transfer_release(buffer);
    return 1;
}

int usb_hub_get_port_status(uint8_t slot_id, uint8_t port, usb_hub_port_status_t *out_status)
{
    usb_setup_packet_t setup;
    uint8_t *buffer = xhci_transfer_allocate(4);
    if (!buffer) return 0;

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
        (void)xhci_transfer_release(buffer);
        return 0;
    }

    if (out_status)
    {
        out_status->wPortStatus = (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
        out_status->wPortChange = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
    }

    (void)xhci_transfer_release(buffer);
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

bool usb_hub_context_current(const usb_device_context_t *ctx)
{
    if (!ctx || !ctx->parent_hub_slot || !ctx->port_on_parent || ctx->route_string > 0xfffff ||
        !usb_hotplug_root_current(ctx->root_port, ctx->root_generation)) return false;
    bool valid = false;
    irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
    for (unsigned i = 0; i < USB_HUB_MAX_HUBS; ++i) {
        const usb_hub_info_t *hub = &usb_hub_list[i];
        if (hub->valid && hub->slot_id == ctx->parent_hub_slot &&
            hub->slot_generation == ctx->parent_generation &&
            hub->root_generation == ctx->root_generation && hub->root_port == ctx->root_port &&
            ctx->port_on_parent <= hub->num_ports &&
            ctx->route_string == usb_hub_calc_route_string(hub->route_string, ctx->port_on_parent)) valid = true;
    }
    spin_unlock_irqrestore(&g_usb_hub_lock, flags);
    return valid;
}
int usb_hub_build_device_context(uint8_t parent_slot, uint8_t port, uint8_t speed,
                                 usb_device_context_t *out)
{
    if (!out || !port || port > 15) return 0;
    usb_hub_info_t parent = {0};
    irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
    for (unsigned i = 0; i < USB_HUB_MAX_HUBS; ++i)
        if (usb_hub_list[i].valid && usb_hub_list[i].slot_id == parent_slot) parent = usb_hub_list[i];
    spin_unlock_irqrestore(&g_usb_hub_lock, flags);
    if (!parent.valid || port > parent.num_ports || parent.hub_depth >= USB_HUB_MAX_DEPTH) return 0;
    uint32_t route = usb_hub_calc_route_string(parent.route_string, port);
    if (route == UINT32_MAX) return 0;
    usb_device_context_t ctx = { .root_port=parent.root_port, .root_generation=parent.root_generation,
        .parent_generation=parent.slot_generation, .parent_hub_slot=parent_slot, .port_on_parent=port,
        .hub_depth=parent.hub_depth+1, .speed=speed,
        .route_string=route };
    if (!usb_translator(speed, parent.speed, parent_slot, port, parent.upstream_tt_slot,
                        parent.upstream_tt_port, &ctx.tt_hub_slot_id, &ctx.tt_port_num) ||
        !usb_hub_context_current(&ctx)) return 0;
    *out = ctx; return 1;
}

static bool publish_child(unsigned index, const usb_device_context_t *ctx, uint8_t slot)
{
    if (index >= USB_HUB_MAX_HUBS || !slot || slot >= 64) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
    usb_hub_info_t *hub = &usb_hub_list[index];
    bool ok = hub->valid && g_usb_hub_probing[index] &&
        hub->slot_id == ctx->parent_hub_slot && hub->slot_generation == ctx->parent_generation &&
        ctx->port_on_parent && ctx->port_on_parent <= hub->num_ports &&
        !hub->child_slot[ctx->port_on_parent] &&
        usb_hotplug_root_current(ctx->root_port, ctx->root_generation);
    if (ok) hub->child_slot[ctx->port_on_parent] = slot;
    spin_unlock_irqrestore(&g_usb_hub_lock, flags); return ok;
}
static void end_probe(unsigned index)
{
    irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
    g_usb_hub_probing[index] = 0;
    spin_unlock_irqrestore(&g_usb_hub_lock, flags);
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
    uint64_t root_generation;
    if (!slot_id || slot_id >= 64 || !usb_hotplug_root_identity(root_port, &root_generation)) return -1;
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

        hub->root_generation = root_generation;
        hub->slot_generation = xhci_driver.slot_lifecycle[slot_id];
        hub->speed = xhci_driver.slot_speed[slot_id];
        hub->upstream_tt_slot = xhci_driver.slot_tt_hub[slot_id];
        hub->upstream_tt_port = xhci_driver.slot_tt_port[slot_id];
        hub->is_usb3 = is_usb3_flag;
        hub->interface_num = interface_num;
        hub->config_value = config_value;

        usb_hub_count++;

        spin_unlock_irqrestore(&g_usb_hub_lock, flags);
    }

    console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
    console_write_debug("[USB_HUB] Hub initialized successfully (index ");
    console_print_dec_debug(hub_idx);
    console_write_debug(")\n");
    console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

    usb_hub_probe_ports(hub_idx);
    irq_flags_t done_flags = spin_lock_irqsave(&g_usb_hub_lock);
    g_usb_hub_initializing[hub_idx] = 0;
    spin_unlock_irqrestore(&g_usb_hub_lock, done_flags);
    if (!usb_hotplug_root_current(root_port, root_generation)) { usb_hub_invalidate_by_slot(slot_id); return -1; }
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
        if (!hub->valid || g_usb_hub_probing[hub_idx])
        {
            spin_unlock_irqrestore(&g_usb_hub_lock, flags);
            return 0;
        }

        g_usb_hub_probing[hub_idx] = 1;
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
        if (!usb_hotplug_root_current(hub_snap.root_port, hub_snap.root_generation)) break;
        if (hub_snap.child_slot[port]) continue;
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
        if (!usb_hub_build_device_context(hub_snap.slot_id, port, speed, &dev_ctx)) continue;

        console_write_debug("[USB_HUB] Configuring device: route=0x");
        console_print_hex_debug(dev_ctx.route_string);
        console_write_debug(" root=");
        console_print_dec_debug(dev_ctx.root_port);
        console_write_debug("\n");

        int slot_id = xhci_configure_device_with_context(
            hub_snap.root_port - 1,
            speed,
            &dev_ctx);

        if (slot_id > 0 && publish_child(hub_idx, &dev_ctx, slot_id))
        {
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

    end_probe(hub_idx);
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

            if (!usb_hub_list[i].valid || g_usb_hub_initializing[i] || g_usb_hub_probing[i])
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

void usb_hub_poll_all(void)
{
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++)
    {
        usb_hub_info_t hub_snap;

        {
            irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
            if (!usb_hub_list[i].valid || g_usb_hub_initializing[i] || g_usb_hub_probing[i])
            {
                spin_unlock_irqrestore(&g_usb_hub_lock, flags);
                continue;
            }
            g_usb_hub_probing[i] = 1;
            memcpy(&hub_snap, &usb_hub_list[i], sizeof(usb_hub_info_t));
            spin_unlock_irqrestore(&g_usb_hub_lock, flags);
        }

       
        if (xhci_driver.slot_ep0_rings[hub_snap.slot_id] == NULL)
        {
            console_write_debug("[HUB-HP] Hub slot ");
            console_print_dec_debug(hub_snap.slot_id);
            console_write_debug(" is dead, removing from poll list\n");
            usb_hub_invalidate_by_slot(hub_snap.slot_id);
            end_probe(i);
            continue;
        }

       
        {
            usb_hub_port_status_t probe;
            if (!usb_hub_get_port_status(hub_snap.slot_id, 1, &probe))
            {
                console_write_debug("[HUB-HP] Hub slot ");
                console_print_dec_debug(hub_snap.slot_id);
                console_write_debug(" unreachable, invalidating\n");
                usb_hub_invalidate_by_slot(hub_snap.slot_id);
                end_probe(i);
                continue;
            }
        }

        for (uint8_t port = 1; port <= hub_snap.num_ports; port++)
        {
            if (!usb_hotplug_root_current(hub_snap.root_port, hub_snap.root_generation)) break;
            usb_hub_port_status_t status;
            if (!usb_hub_get_port_status(hub_snap.slot_id, port, &status))
                continue;

            uint8_t connected = (status.wPortStatus & HUB_PORT_STATUS_CONNECTION) ? 1 : 0;
            uint8_t had_device = (hub_snap.child_slot[port] != 0) ? 1 : 0;
            uint8_t change = (status.wPortChange & 1) ? 1 : 0;

           
            if (!change && (connected == had_device))
                continue;

           
            if (change)
                usb_hub_clear_port_feature(hub_snap.slot_id, port, HUB_C_PORT_CONNECTION);

            if (connected && !had_device)
            {
               
                console_set_color_debug(CONSOLE_COLOR_CYAN, CONSOLE_COLOR_BLACK);
                console_write_debug("[HUB-HP] Connect on hub slot=");
                console_print_dec_debug(hub_snap.slot_id);
                console_write_debug(" port=");
                console_print_dec_debug(port);
                console_write_debug("\n");
                console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

                timer_sleep(100);

               
                if (!usb_hub_get_port_status(hub_snap.slot_id, port, &status))
                    continue;
                if (!(status.wPortStatus & HUB_PORT_STATUS_CONNECTION))
                    continue;

                if (!usb_hub_reset_port(hub_snap.slot_id, port))
                {
                    console_write_debug("[HUB-HP] Port reset failed\n");
                    continue;
                }

                if (!usb_hub_get_port_status(hub_snap.slot_id, port, &status))
                    continue;
                if (!(status.wPortStatus & HUB_PORT_STATUS_ENABLE))
                {
                    console_write_debug("[HUB-HP] Port not enabled after reset\n");
                    continue;
                }

                uint8_t speed = usb_hub_get_port_speed(status.wPortStatus, hub_snap.is_usb3);

                console_write_debug("[HUB-HP] Device speed: ");
                console_print_dec_debug(speed);
                console_write_debug("\n");

                usb_device_context_t dev_ctx;
                if (!usb_hub_build_device_context(hub_snap.slot_id, port, speed, &dev_ctx)) continue;

                int slot_id = xhci_configure_device_with_context(
                    hub_snap.root_port - 1,
                    speed,
                    &dev_ctx);

                if (slot_id > 0 && publish_child(i, &dev_ctx, slot_id))
                {
                    console_set_color_debug(CONSOLE_COLOR_GREEN, CONSOLE_COLOR_BLACK);
                    console_write_debug("[HUB-HP] Device configured at slot ");
                    console_print_dec_debug(slot_id);
                    console_write_debug("\n");
                    console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);
                }
            }
            else if (!connected && had_device)
            {
               
                uint8_t old_slot = hub_snap.child_slot[port];

                console_set_color_debug(CONSOLE_COLOR_ORANGE, CONSOLE_COLOR_BLACK);
                console_write_debug("[HUB-HP] Disconnect on hub slot=");
                console_print_dec_debug(hub_snap.slot_id);
                console_write_debug(" port=");
                console_print_dec_debug(port);
                console_write_debug(" (slot ");
                console_print_dec_debug(old_slot);
                console_write_debug(")\n");
                console_set_color_debug(CONSOLE_COLOR_WHITE, CONSOLE_COLOR_BLACK);

                {
                    irq_flags_t flags = spin_lock_irqsave(&g_usb_hub_lock);
                    if (usb_hub_list[i].valid && usb_hub_list[i].slot_generation == hub_snap.slot_generation)
                        usb_hub_list[i].child_slot[port] = 0;
                    spin_unlock_irqrestore(&g_usb_hub_lock, flags);
                }

               
                usb_hub_invalidate_by_slot(old_slot);

                xhci_disable_slot(old_slot);
            }
        }
        end_probe(i);
    }
}
