#include "usb_hotplug.h"

extern void console_write_debug(const char *str);
extern void console_print_dec_debug(uint32_t val);
extern void console_print_hex_debug(uint32_t val);

/* External functions from xhci.c */
extern void xhci_hotplug_enumerate_port(uint8_t port_0based);

/*
 * Root port indexing:
 *  - xHCI Port ID in the Port Status Change Event TRB is 1-based
 *  - We keep the arrays 1-based for simplicity: index 1..255
 */
static uint8_t g_hp_last_ccs[256];  /* last known CCS (0/1) */
static uint8_t g_hp_port_slot[256]; /* Slot ID for device directly attached to the root port (0 = unknown) */
static uint8_t g_hp_initialized = 0;

/*
 * Pending enumeration queue (simple circular buffer)
 * We use a queue because multiple devices could be connected rapidly
 */
#define HP_PENDING_QUEUE_SIZE 16
static uint8_t g_hp_pending_ports[HP_PENDING_QUEUE_SIZE];
static uint8_t g_hp_pending_head = 0;  /* next slot to write */
static uint8_t g_hp_pending_tail = 0;  /* next slot to read */
static uint8_t g_hp_pending_count = 0;

static void hp_log_connect(uint8_t root_port, uint8_t slot_id)
{
    console_write_debug("[USB][HOTPLUG] connect on root port ");
    console_print_dec_debug(root_port);
    console_write_debug(" (Slot ");
    console_print_dec_debug(slot_id);
    console_write_debug(")\n");
}

static void hp_log_disconnect(uint8_t root_port, uint8_t slot_id)
{
    console_write_debug("[USB][HOTPLUG] disconnect on root port ");
    console_print_dec_debug(root_port);
    console_write_debug(" (Slot ");
    console_print_dec_debug(slot_id);
    console_write_debug(")\n");
}

void usb_hotplug_init(void)
{
    if (g_hp_initialized)
        return;

    g_hp_initialized = 1;

    for (int i = 0; i < 256; i++)
    {
        g_hp_last_ccs[i] = 0;
        g_hp_port_slot[i] = 0;
    }

    g_hp_pending_head = 0;
    g_hp_pending_tail = 0;
    g_hp_pending_count = 0;
}

/*
 * Schedule a port for enumeration (called from event handler)
 */
static void hp_schedule_enumeration(uint8_t root_port_1based)
{
    if (g_hp_pending_count >= HP_PENDING_QUEUE_SIZE)
    {
        console_write_debug("[USB][HOTPLUG] WARNING: pending queue full!\n");
        return;
    }

    /* Check if already in queue */
    for (uint8_t i = 0; i < g_hp_pending_count; i++)
    {
        uint8_t idx = (g_hp_pending_tail + i) % HP_PENDING_QUEUE_SIZE;
        if (g_hp_pending_ports[idx] == root_port_1based)
        {
            return; /* Already queued */
        }
    }

    g_hp_pending_ports[g_hp_pending_head] = root_port_1based;
    g_hp_pending_head = (g_hp_pending_head + 1) % HP_PENDING_QUEUE_SIZE;
    g_hp_pending_count++;
}

/*
 * Take next port from enumeration queue
 * Returns 0 if queue is empty, otherwise returns port_1based
 */
static uint8_t hp_take_pending(void)
{
    if (g_hp_pending_count == 0)
        return 0;

    uint8_t port = g_hp_pending_ports[g_hp_pending_tail];
    g_hp_pending_tail = (g_hp_pending_tail + 1) % HP_PENDING_QUEUE_SIZE;
    g_hp_pending_count--;

    return port;
}

/*
 * portsc bit we care about right now:
 *  - CCS (Current Connect Status) bit 0
 */
void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc)
{
    if (!g_hp_initialized)
        usb_hotplug_init();

    if (root_port_1based == 0)
        return;

    uint8_t ccs = (portsc & (1u << 0)) ? 1 : 0;
    uint8_t prev = g_hp_last_ccs[root_port_1based];

    if (ccs == prev)
        return;

    g_hp_last_ccs[root_port_1based] = ccs;

    uint8_t slot = g_hp_port_slot[root_port_1based];

    if (ccs)
    {
        /* Device connected - log and schedule enumeration */
        hp_log_connect(root_port_1based, slot);

        /* Schedule this port for enumeration (will be processed in bottom_half) */
        hp_schedule_enumeration(root_port_1based);
    }
    else
    {
        /* Device disconnected */
        hp_log_disconnect(root_port_1based, slot);

        /* Clear mapping; device is gone */
        g_hp_port_slot[root_port_1based] = 0;
    }
}

void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id)
{
    if (!g_hp_initialized)
        usb_hotplug_init();

    if (root_port_1based == 0)
        return;

    /* Remember slot for this port */
    g_hp_port_slot[root_port_1based] = slot_id;
    g_hp_last_ccs[root_port_1based] = 1;
}

/*
 * Process pending hotplug enumerations.
 * Called from xhci_bottom_half() - OUTSIDE the event processing loop.
 */
int usb_hotplug_process_pending(void)
{
    int processed = 0;

    while (g_hp_pending_count > 0)
    {
        uint8_t port_1based = hp_take_pending();
        if (port_1based == 0)
            break;

        console_write_debug("[USB][HOTPLUG] Enumerating device on port ");
        console_print_dec_debug(port_1based);
        console_write_debug("...\n");

        /* Call xhci to do the actual enumeration */
        /* port_0based = port_1based - 1 */
        xhci_hotplug_enumerate_port(port_1based - 1);

        processed++;
    }

    return processed;
}

void keyboard_on_usb_hid_key(uint8_t modifiers, uint8_t keycode)
{
    extern void keyboard_push_usb_event(uint8_t modifiers, uint8_t keycode);
    keyboard_push_usb_event(modifiers, keycode);
}
