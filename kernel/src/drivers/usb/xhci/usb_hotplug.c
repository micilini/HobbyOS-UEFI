#include "usb_hotplug.h"
#include "../../../core/spinlock.h"

#define HP_DISABLE_QUEUE_SIZE 16
static uint8_t g_hp_pending_disable[HP_DISABLE_QUEUE_SIZE];
static spinlock_t g_hp_lock;

static uint8_t g_hp_disable_head = 0;
static uint8_t g_hp_disable_tail = 0;
static uint8_t g_hp_disable_count = 0;

static uint64_t g_hp_last_enum_time[256] = {0};
#define HP_ENUM_COOLDOWN_MS 2000

extern uint64_t timer_get_uptime_ms(void);

extern void console_write_debug(const char *str);
extern void console_print_dec_debug(uint32_t val);
extern void console_print_hex_debug(uint32_t val);

extern void xhci_disable_slot(uint8_t slot_id);

extern void xhci_hotplug_enumerate_port(uint8_t port_0based);

static uint8_t g_hp_last_ccs[256];
static uint8_t g_hp_port_slot[256];
static uint8_t g_hp_initialized = 0;

#define HP_PENDING_QUEUE_SIZE 16
static uint8_t g_hp_pending_ports[HP_PENDING_QUEUE_SIZE];
static uint8_t g_hp_pending_head = 0;
static uint8_t g_hp_pending_tail = 0;
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

    spinlock_init(&g_hp_lock);

    g_hp_initialized = 1;

    for (int i = 0; i < 256; i++)
    {
        g_hp_last_ccs[i] = 0;
        g_hp_port_slot[i] = 0;
        g_hp_last_enum_time[i] = 0;
    }

    g_hp_pending_head = 0;
    g_hp_pending_tail = 0;
    g_hp_pending_count = 0;

    g_hp_disable_head = 0;
    g_hp_disable_tail = 0;
    g_hp_disable_count = 0;
}

static int hp_schedule_enumeration_nolock(uint8_t root_port_1based)
{
    if (g_hp_pending_count >= HP_PENDING_QUEUE_SIZE)
    {
        return -1;
    }

    uint64_t now = timer_get_uptime_ms();
    uint64_t last = g_hp_last_enum_time[root_port_1based];
    if (last != 0 && (now - last) < HP_ENUM_COOLDOWN_MS)
    {
        return 0;
    }

    for (uint8_t i = 0; i < g_hp_pending_count; i++)
    {
        uint8_t idx = (g_hp_pending_tail + i) % HP_PENDING_QUEUE_SIZE;
        if (g_hp_pending_ports[idx] == root_port_1based)
        {
            return 0;
        }
    }

    g_hp_last_enum_time[root_port_1based] = now;

    g_hp_pending_ports[g_hp_pending_head] = root_port_1based;
    g_hp_pending_head = (g_hp_pending_head + 1) % HP_PENDING_QUEUE_SIZE;
    g_hp_pending_count++;

    return 1;
}

static uint8_t hp_take_pending_nolock(void)
{
    if (g_hp_pending_count == 0)
        return 0;

    uint8_t port = g_hp_pending_ports[g_hp_pending_tail];
    g_hp_pending_tail = (g_hp_pending_tail + 1) % HP_PENDING_QUEUE_SIZE;
    g_hp_pending_count--;

    return port;
}

static int hp_schedule_disable_nolock(uint8_t slot_id)
{
    if (slot_id == 0)
        return 0;

    if (g_hp_disable_count >= HP_DISABLE_QUEUE_SIZE)
    {
        return -1;
    }

    for (uint8_t i = 0; i < g_hp_disable_count; i++)
    {
        uint8_t idx = (g_hp_disable_tail + i) % HP_DISABLE_QUEUE_SIZE;
        if (g_hp_pending_disable[idx] == slot_id)
        {
            return 0;
        }
    }

    g_hp_pending_disable[g_hp_disable_head] = slot_id;
    g_hp_disable_head = (g_hp_disable_head + 1) % HP_DISABLE_QUEUE_SIZE;
    g_hp_disable_count++;

    return 1;
}

static uint8_t hp_take_pending_disable_nolock(void)
{
    if (g_hp_disable_count == 0)
        return 0;

    uint8_t slot = g_hp_pending_disable[g_hp_disable_tail];
    g_hp_disable_tail = (g_hp_disable_tail + 1) % HP_DISABLE_QUEUE_SIZE;
    g_hp_disable_count--;

    return slot;
}

void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc)
{
    if (!g_hp_initialized)
        usb_hotplug_init();

    if (root_port_1based == 0)
        return;

    uint8_t ccs = (portsc & (1u << 0)) ? 1 : 0;

    uint8_t do_log_connect = 0;
    uint8_t do_log_disconnect = 0;
    uint8_t slot_for_log = 0;

    int enum_status = 0;
    int disable_status = 0;

    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);

    uint8_t prev = g_hp_last_ccs[root_port_1based];
    if (ccs == prev)
    {
        spin_unlock_irqrestore(&g_hp_lock, flags);
        return;
    }

    g_hp_last_ccs[root_port_1based] = ccs;

    uint8_t slot = g_hp_port_slot[root_port_1based];
    slot_for_log = slot;

    if (ccs)
    {
        do_log_connect = 1;
        enum_status = hp_schedule_enumeration_nolock(root_port_1based);
    }
    else
    {
        do_log_disconnect = 1;

        if (slot != 0)
        {
            disable_status = hp_schedule_disable_nolock(slot);
        }

        g_hp_port_slot[root_port_1based] = 0;
        g_hp_last_enum_time[root_port_1based] = 0;
    }

    spin_unlock_irqrestore(&g_hp_lock, flags);

    if (do_log_connect)
    {
        hp_log_connect(root_port_1based, slot_for_log);
        if (enum_status < 0)
            console_write_debug("[USB][HOTPLUG] WARNING: pending queue full!\n");
    }
    else if (do_log_disconnect)
    {
        hp_log_disconnect(root_port_1based, slot_for_log);
        if (disable_status < 0)
            console_write_debug("[USB][HOTPLUG] WARNING: disable queue full!\n");
    }
}

void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id)
{
    if (!g_hp_initialized)
        usb_hotplug_init();

    if (root_port_1based == 0)
        return;

    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    g_hp_port_slot[root_port_1based] = slot_id;
    g_hp_last_ccs[root_port_1based] = 1;
    spin_unlock_irqrestore(&g_hp_lock, flags);
}

int usb_hotplug_process_pending(void)
{
    int processed = 0;

    for (;;)
    {
        uint8_t slot_id = 0;

        irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
        slot_id = hp_take_pending_disable_nolock();
        spin_unlock_irqrestore(&g_hp_lock, flags);

        if (slot_id == 0)
            break;

        xhci_disable_slot(slot_id);
        processed++;
    }

    for (;;)
    {
        uint8_t port_1based = 0;

        irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
        port_1based = hp_take_pending_nolock();
        spin_unlock_irqrestore(&g_hp_lock, flags);

        if (port_1based == 0)
            break;

        console_write_debug("[USB][HOTPLUG] Enumerating device on port ");
        console_print_dec_debug(port_1based);
        console_write_debug("...\n");

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
