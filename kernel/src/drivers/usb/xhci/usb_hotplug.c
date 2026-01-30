#include "usb_hotplug.h"

extern void console_write_debug(const char *str);
extern void console_print_dec_debug(uint32_t val);

/*
 * Root port indexing:
 *  - xHCI Port ID in the Port Status Change Event TRB is 1-based
 *  - We keep the arrays 1-based for simplicity: index 1..255
 */
static uint8_t g_hp_last_ccs[256];  /* last known CCS (0/1) */
static uint8_t g_hp_port_slot[256]; /* Slot ID for device directly attached to the root port (0 = unknown) */
static uint8_t g_hp_initialized = 0;

static void hp_log_connect(uint8_t root_port, uint8_t slot_id)
{
    console_write_debug("[USB][HOTPLUG][0] connect on root port ");
    console_print_dec_debug(root_port);
    console_write_debug(" (Slot ");
    console_print_dec_debug(slot_id);
    console_write_debug(")\n");
}

static void hp_log_disconnect(uint8_t root_port, uint8_t slot_id)
{
    console_write_debug("[USB][HOTPLUG][0] disconnect on root port ");
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
        /* Slot may still be 0 here (no re-enumeration in this milestone). */
        hp_log_connect(root_port_1based, slot);
    }
    else
    {
        hp_log_disconnect(root_port_1based, slot);

        /* Clear mapping; device is gone. */
        g_hp_port_slot[root_port_1based] = 0;
    }
}

void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id)
{
    if (!g_hp_initialized)
        usb_hotplug_init();

    if (root_port_1based == 0)
        return;

    /* Only remember for logs; no extra output here (to avoid noise). */
    g_hp_port_slot[root_port_1based] = slot_id;
    g_hp_last_ccs[root_port_1based] = 1;
}

void keyboard_on_usb_hid_key(uint8_t modifiers, uint8_t keycode)
{
    extern void keyboard_push_usb_event(uint8_t modifiers, uint8_t keycode);
    keyboard_push_usb_event(modifiers, keycode);
}
