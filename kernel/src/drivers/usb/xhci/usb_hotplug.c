#include "usb_hotplug.h"
#include "xhci.h"
#include "../../../core/spinlock.h"
#include "../../../core/dpc.h"
#include "../../../core/timers.h"
#include "../../../graphics/console.h"

#define MAX_ROOT_PORTS 16
#define HP_ENUM_COOLDOWN_MS 2000

static spinlock_t g_hp_lock;
static hp_port_context_t g_port_ctx[MAX_ROOT_PORTS + 1];
static uint8_t g_hp_initialized = 0;

static void hp_fsm_run(void *ctx);
static void hp_timer_callback(void *ctx);

static uint32_t hp_read_portsc(uint8_t port_1based)
{
    if (port_1based == 0 || port_1based > xhci_driver.max_ports)
        return 0;
    return xhci_driver.port_regs[port_1based - 1].PortSC;
}

static void hp_write_portsc(uint8_t port_1based, uint32_t val)
{
    if (port_1based == 0 || port_1based > xhci_driver.max_ports)
        return;
    volatile uint32_t *reg = &xhci_driver.port_regs[port_1based - 1].PortSC;

    *reg = val;
}

void usb_hotplug_init(void)
{
    if (g_hp_initialized)
        return;

    spinlock_init(&g_hp_lock);

    for (int i = 0; i <= MAX_ROOT_PORTS; i++)
    {
        g_port_ctx[i].state = HP_STATE_IDLE;
        g_port_ctx[i].root_port_1based = i;
        g_port_ctx[i].retries = 0;
        g_port_ctx[i].slot_id = 0;
    }

    g_hp_initialized = 1;
    console_write_debug("[USB] Hotplug State Machine Initialized.\n");
}

static void hp_timer_callback(void *ctx)
{
    hp_port_context_t *port_ctx = (hp_port_context_t *)ctx;

    dpc_enqueue(hp_fsm_run, port_ctx);
}

static void hp_fsm_run(void *ctx)
{
    hp_port_context_t *p = (hp_port_context_t *)ctx;
    uint8_t port = p->root_port_1based;
    uint32_t sc = hp_read_portsc(port);

    if (!(sc & (1 << 0)) && p->state != HP_STATE_IDLE)
    {
        console_write_debug("[HP] Device disconnected during enum. Resetting state.\n");
        p->state = HP_STATE_IDLE;
        return;
    }

    switch (p->state)
    {
    case HP_STATE_IDLE:

        break;

    case HP_STATE_WAIT_CONNECTION_STABLE:

        console_write_debug("[HP] Connection stable. Starting Reset.\n");

        hp_write_portsc(port, (sc & (1 << 9)) | (1 << 4));

        p->state = HP_STATE_WAIT_RESET;

        timers_add(50, hp_timer_callback, p);
        break;

    case HP_STATE_WAIT_RESET:

        if (sc & (1 << 1))
        {
            console_write_debug("[HP] Port Enabled! Configuring device...\n");
            p->state = HP_STATE_CONFIGURE_DEVICE;

            dpc_enqueue(hp_fsm_run, p);
        }
        else
        {

            if (p->retries++ < 5)
            {
                console_write_debug("[HP] Reset wait retry...\n");
                timers_add(50, hp_timer_callback, p);
            }
            else
            {
                console_write_debug("[HP] Reset TIMEOUT. Aborting.\n");
                p->state = HP_STATE_ERROR;
            }
        }
        break;

    case HP_STATE_CONFIGURE_DEVICE:
        if (p->slot_id != 0)
        {
            xhci_disable_slot(p->slot_id);
            p->slot_id = 0;
        }

        uint8_t speed = (sc >> 10) & 0xF;

        xhci_configure_device(port - 1, speed);

        p->state = HP_STATE_DONE;
        console_write_debug("[HP] Enumeration DONE.\n");

        p->state = HP_STATE_IDLE;
        break;

    case HP_STATE_ERROR:
    case HP_STATE_DONE:
        p->state = HP_STATE_IDLE;
        break;
    }
}

void usb_hotplug_handle_root_port_status(uint8_t root_port_1based, uint32_t portsc)
{
    if (!g_hp_initialized)
        usb_hotplug_init();
    if (root_port_1based >= MAX_ROOT_PORTS)
        return;

    hp_port_context_t *p = &g_port_ctx[root_port_1based];
    uint8_t ccs = (portsc & 1);

    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);

    if (ccs)
    {

        if (p->state == HP_STATE_IDLE)
        {
            console_write_debug("[HP] Connect detected. Waiting debounce (150ms)...\n");

            p->state = HP_STATE_WAIT_CONNECTION_STABLE;
            p->retries = 0;

            timers_add(150, hp_timer_callback, p);
        }
    }
    else
    {

        console_write_debug("[HP] Disconnect detected.\n");

        p->state = HP_STATE_IDLE;
    }

    spin_unlock_irqrestore(&g_hp_lock, flags);
}

void usb_hotplug_process_pending(void)
{
}

void usb_hotplug_notify_root_device_configured(uint8_t root_port_1based, uint8_t slot_id)
{
    if (root_port_1based == 0 || root_port_1based > MAX_ROOT_PORTS)
        return;

    hp_port_context_t *p = &g_port_ctx[root_port_1based];
    p->slot_id = slot_id;
}