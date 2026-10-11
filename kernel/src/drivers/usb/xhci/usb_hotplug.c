#include "usb_hotplug.h"
#include "xhci.h"
#include "../../../core/spinlock.h"
#include "../../../core/dpc.h"
#include "../../../core/timers.h"
#include "../../../graphics/console.h"
#include "../../../core/clock.h"

#define MAX_ROOT_PORTS 16
#define HP_ENUM_COOLDOWN_MS 2000

static spinlock_t g_hp_lock;
static hp_port_context_t g_port_ctx[MAX_ROOT_PORTS + 1];
static uint8_t g_hp_initialized = 0;
static volatile uint32_t g_hp_active_enumerations;
static volatile uint64_t g_hp_activity_generation;
static volatile uint64_t g_hp_last_activity_ns;
static uint64_t g_ticket, g_poll_ticket;
static bool g_poll_failed;
static uint64_t g_poll_completions;

static void hp_readiness_activity(void)
{
    __atomic_add_fetch(&g_hp_activity_generation, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hp_last_activity_ns, clock_monotonic_ns(),
                     __ATOMIC_RELEASE);
}

static void hp_readiness_begin(hp_port_context_t *port)
{
    if (!port || __atomic_exchange_n(&port->readiness_active, 1,
                                     __ATOMIC_ACQ_REL))
        return;
    __atomic_add_fetch(&g_hp_active_enumerations, 1, __ATOMIC_ACQ_REL);
    hp_readiness_activity();
}

static void hp_readiness_end(hp_port_context_t *port)
{
    if (!port || !__atomic_exchange_n(&port->readiness_active, 0,
                                      __ATOMIC_ACQ_REL))
        return;
    uint32_t active = __atomic_load_n(&g_hp_active_enumerations,
                                      __ATOMIC_ACQUIRE);
    while (active && !__atomic_compare_exchange_n(
        &g_hp_active_enumerations, &active, active - 1u, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
    hp_readiness_activity();
}

static void hp_fsm_run(void *ctx);
static void hp_timer_callback(void *ctx);

static void hp_hub_poll_dpc(void *ctx);
static void hp_hub_poll_timer_cb(void *ctx);
static void hp_poll_schedule(bool timer);

_Static_assert(offsetof(xhci_port_regs_t, PortSC) % sizeof(uint32_t) == 0,
               "xHCI PortSC must be naturally aligned");

static volatile uint32_t *hp_portsc_reg(uint8_t port_1based)
{
    size_t offset = (size_t)(port_1based - 1u) *
                        sizeof(xhci_port_regs_t) +
                    offsetof(xhci_port_regs_t, PortSC);
    return (volatile uint32_t *)((volatile uint8_t *)xhci_driver.port_regs +
                                 offset);
}

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
    volatile uint32_t *reg = hp_portsc_reg(port_1based);

    *reg = val;
}

void usb_hotplug_init(void)
{
    if (g_hp_initialized)
        return;

    spinlock_init(&g_hp_lock);
    __atomic_store_n(&g_hp_active_enumerations, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hp_activity_generation, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hp_last_activity_ns, clock_monotonic_ns(),
                     __ATOMIC_RELEASE);

    for (int i = 0; i <= MAX_ROOT_PORTS; i++)
    {
        g_port_ctx[i].state = HP_STATE_IDLE;
        g_port_ctx[i].root_port_1based = i;
        g_port_ctx[i].retries = 0;
        g_port_ctx[i].slot_id = 0;
        g_port_ctx[i].readiness_active = 0;
    }

    hp_poll_schedule(true);
    g_hp_initialized = 1;
    console_write_debug("[USB] Hotplug State Machine Initialized.\n");
}

/* Callback cookies contain immutable ticket/port values, never a pointer to
 * mutable lifecycle state. A ticket is consumed exactly once under the lock. */
static bool valid_port(uint8_t port)
{ return port && port <= MAX_ROOT_PORTS && port <= xhci_driver.max_ports; }

static void hp_fail(uint8_t port, uint64_t generation)
{
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[port];
    if (usb_root_matches(&p->owner, generation)) {
        p->owner.phase = USB_ROOT_RETAINED; p->state = HP_STATE_ERROR;
        p->pending_ticket = 0; hp_readiness_end(p);
    }
    spin_unlock_irqrestore(&g_hp_lock, flags);
}

static void hp_schedule(uint8_t port, uint64_t generation, uint32_t delay)
{
    uint64_t ticket = 0;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[port];
    if (usb_root_matches(&p->owner, generation) && !p->pending_ticket && g_ticket < (UINT64_MAX >> 8)) {
        ticket = ++g_ticket; p->pending_ticket = ticket; p->pending_generation = generation;
    }
    spin_unlock_irqrestore(&g_hp_lock, flags);
    if (!ticket) { hp_fail(port, generation); return; }
    void *cookie = (void *)(uintptr_t)((ticket << 8) | port);
    int result = delay ? timers_add(delay, hp_timer_callback, cookie) : dpc_enqueue(hp_fsm_run, cookie);
    if (result < 0) hp_fail(port, generation);
}
static bool hp_consume(void *ctx, uint8_t *port, uint64_t *generation)
{
    uint64_t cookie = (uintptr_t)ctx, ticket = cookie >> 8;
    *port = cookie & 255;
    if (!ticket || !valid_port(*port)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[*port];
    bool ok = p->pending_ticket == ticket && usb_root_matches(&p->owner, p->pending_generation);
    if (ok) { *generation = p->pending_generation; p->pending_ticket = 0; }
    spin_unlock_irqrestore(&g_hp_lock, flags);
    return ok;
}
static void hp_timer_callback(void *ctx)
{
    uint8_t port; uint64_t generation;
    if (hp_consume(ctx, &port, &generation)) hp_schedule(port, generation, 0);
}

bool usb_hotplug_begin_root(uint8_t port, uint64_t *generation)
{
    if (!g_hp_initialized) usb_hotplug_init();
    if (!valid_port(port) || !generation) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[port];
    bool ok = usb_root_claim(&p->owner, generation);
    if (ok) { p->state = HP_STATE_CONFIGURE_DEVICE; p->retries = 0; hp_readiness_begin(p); }
    spin_unlock_irqrestore(&g_hp_lock, flags);
    return ok;
}
bool usb_hotplug_root_current(uint8_t port, uint64_t generation)
{
    if (!valid_port(port)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    bool ok = usb_root_matches(&g_port_ctx[port].owner, generation);
    spin_unlock_irqrestore(&g_hp_lock, flags); return ok;
}
bool usb_hotplug_root_identity(uint8_t port, uint64_t *generation)
{
    if (!valid_port(port) || !generation) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[port];
    bool ok = p->owner.slot && usb_root_matches(&p->owner, p->owner.generation);
    if (ok) *generation = p->owner.generation;
    spin_unlock_irqrestore(&g_hp_lock, flags); return ok;
}
bool usb_hotplug_root_address(uint8_t port, uint64_t generation, uint8_t slot)
{
    if (!valid_port(port)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    bool ok = usb_root_address(&g_port_ctx[port].owner, generation, slot);
    if (ok) g_port_ctx[port].slot_id = slot;
    spin_unlock_irqrestore(&g_hp_lock, flags); return ok;
}
bool usb_hotplug_finish_root(uint8_t port, uint64_t generation, bool success)
{
    if (!valid_port(port)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[port];
    bool current = usb_root_matches(&p->owner, generation);
    bool ok = usb_root_finish(&p->owner, generation, success);
    if (current) { p->state = ok ? HP_STATE_DONE : HP_STATE_ERROR; hp_readiness_end(p); }
    spin_unlock_irqrestore(&g_hp_lock, flags); return ok;
}

static void hp_fsm_run(void *ctx)
{
    uint8_t port; uint64_t generation;
    if (!hp_consume(ctx, &port, &generation)) return;
    uint32_t sc = hp_read_portsc(port);
    if (!(sc & 1)) { usb_hotplug_handle_root_port_status(port, sc); return; }
    hp_state_t state;
    uint8_t retired;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[port];
    if (!usb_root_matches(&p->owner, generation)) { spin_unlock_irqrestore(&g_hp_lock, flags); return; }
    state = p->state; retired = p->retired_slot;
    spin_unlock_irqrestore(&g_hp_lock, flags);
    if (retired) {
        if (!xhci_disable_slot(retired)) { hp_fail(port, generation); return; }
        flags = spin_lock_irqsave(&g_hp_lock);
        if (p->retired_slot == retired) p->retired_slot = 0;
        spin_unlock_irqrestore(&g_hp_lock, flags);
    }
    if (!usb_hotplug_root_current(port, generation)) return;
    if (state == HP_STATE_WAIT_CONNECTION_STABLE) {
        flags = spin_lock_irqsave(&g_hp_lock);
        if (usb_root_matches(&p->owner, generation)) {
            hp_write_portsc(port, (sc & (1u << 9)) | (1u << 4));
            p->state = HP_STATE_WAIT_RESET;
        }
        spin_unlock_irqrestore(&g_hp_lock, flags);
        hp_schedule(port, generation, 50);
    } else if (state == HP_STATE_WAIT_RESET) {
        if (sc & (1u << 1)) {
            flags = spin_lock_irqsave(&g_hp_lock);
            if (usb_root_matches(&p->owner, generation)) p->state = HP_STATE_CONFIGURE_DEVICE;
            spin_unlock_irqrestore(&g_hp_lock, flags);
            hp_schedule(port, generation, 0);
        } else {
            flags = spin_lock_irqsave(&g_hp_lock);
            bool retry = usb_root_matches(&p->owner, generation) && p->retries++ < 5;
            spin_unlock_irqrestore(&g_hp_lock, flags);
            if (retry) hp_schedule(port, generation, 50); else hp_fail(port, generation);
        }
    } else if (state == HP_STATE_CONFIGURE_DEVICE) {
        bool ok = xhci_configure_owned_root(port - 1, (sc >> 10) & 15, generation) > 0;
        if (usb_hotplug_finish_root(port, generation, ok)) console_write_debug("[HP] Enumeration DONE.\n");
        else console_write_debug("[HP] Enumeration failed or attachment retired.\n");
    }
}

void usb_hotplug_handle_root_port_status(uint8_t port, uint32_t portsc)
{
    if (!g_hp_initialized) usb_hotplug_init();
    if (!valid_port(port)) return;
    uint64_t generation = 0; uint8_t detached = 0;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    hp_port_context_t *p = &g_port_ctx[port];
    if (portsc & 1) {
        if (usb_root_claim(&p->owner, &generation)) {
            p->state = HP_STATE_WAIT_CONNECTION_STABLE; p->retries = 0; hp_readiness_begin(p);
        }
    } else if (p->owner.phase != USB_ROOT_DETACHED) {
        detached = usb_root_detach(&p->owner);
        p->pending_ticket = 0; p->retired_slot = detached; p->slot_id = 0;
        p->state = HP_STATE_IDLE; hp_readiness_end(p);
    }
    spin_unlock_irqrestore(&g_hp_lock, flags);
    if (detached) usb_hub_invalidate_by_slot(detached);
    if (generation) hp_schedule(port, generation, 150);
}

static void hp_poll_schedule(bool timer)
{
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    uint64_t ticket = 0;
    if (!g_poll_failed && !g_poll_ticket && g_ticket < (UINT64_MAX >> 8))
        g_poll_ticket = ticket = ++g_ticket;
    else g_poll_failed = true;
    spin_unlock_irqrestore(&g_hp_lock, flags);
    int result = -1;
    if (ticket) result = timer ? timers_add(g_hp_initialized ? 1000 : 2000,
        hp_hub_poll_timer_cb, (void *)(uintptr_t)ticket) :
        dpc_enqueue(hp_hub_poll_dpc, (void *)(uintptr_t)ticket);
    if (result < 0) {
        flags = spin_lock_irqsave(&g_hp_lock);
        g_poll_failed = true; g_poll_ticket = 0;
        spin_unlock_irqrestore(&g_hp_lock, flags);
        console_write_debug("[USB] Hub poll scheduling failed; readiness unavailable.\n");
    }
}
static bool hp_poll_consume(void *ctx)
{
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    bool valid = !g_poll_failed && g_poll_ticket && g_poll_ticket == (uintptr_t)ctx;
    if (valid) g_poll_ticket = 0;
    spin_unlock_irqrestore(&g_hp_lock, flags); return valid;
}
static void hp_hub_poll_dpc(void *ctx)
{
    if (!hp_poll_consume(ctx)) return;
    if (usb_hub_get_count() > 0) usb_hub_poll_all();
    __atomic_add_fetch(&g_poll_completions,1,__ATOMIC_RELEASE);
    hp_poll_schedule(true);
}
static void hp_hub_poll_timer_cb(void *ctx)
{
    if (hp_poll_consume(ctx)) hp_poll_schedule(false);
}

void usb_hotplug_process_pending(void)
{
   
}

bool usb_hotplug_readiness_snapshot(usb_hotplug_readiness_snapshot_t *out)
{
    if (!out) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    out->failed = g_poll_failed;
    out->poll_completions = __atomic_load_n(&g_poll_completions,__ATOMIC_ACQUIRE);
    for (unsigned i=1;i<=MAX_ROOT_PORTS;++i)
        out->failed |= g_port_ctx[i].owner.phase == USB_ROOT_RETAINED;
    spin_unlock_irqrestore(&g_hp_lock, flags);
    out->initialized = __atomic_load_n(&g_hp_initialized, __ATOMIC_ACQUIRE);
    out->active_enumerations = __atomic_load_n(&g_hp_active_enumerations,
                                               __ATOMIC_ACQUIRE);
    out->activity_generation = __atomic_load_n(&g_hp_activity_generation,
                                               __ATOMIC_ACQUIRE);
    out->last_activity_ns = __atomic_load_n(&g_hp_last_activity_ns,
                                            __ATOMIC_ACQUIRE);
    return out->active_enumerations <= MAX_ROOT_PORTS;
}
#ifdef HOBBYOS_SELFTEST
bool usb_hotplug_owner_snapshot(uint8_t port, usb_root_owner_t *out)
{
    if (!valid_port(port) || !out) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hp_lock);
    *out=g_port_ctx[port].owner;
    spin_unlock_irqrestore(&g_hp_lock,flags);return true;
}
#endif
