#include "ioapic.h"

#include "../cpu/mmio.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../memory/heap.h"
#include "../memory/paging.h"

typedef struct
{
    ioapic_controller_t *controllers;
    interrupt_route_t *routes;
    uint32_t controller_count;
    uint32_t route_capacity;
    uint32_t route_count;
    uint32_t total_entries;
    uint32_t masked_entries;
    uint8_t initialized;
    uint8_t quiescent;
} ioapic_registry_t;

static ioapic_registry_t g_ioapic_registry;

static uint32_t ioapic_read_locked(const ioapic_controller_t *controller,
                                   uint32_t reg)
{
    mmio_write32((void *)(controller->mmio_base + IOREGSEL), reg);
    return mmio_read32((void *)(controller->mmio_base + IOWIN));
}

static void ioapic_write_locked(const ioapic_controller_t *controller,
                                uint32_t reg, uint32_t value)
{
    mmio_write32((void *)(controller->mmio_base + IOREGSEL), reg);
    mmio_write32((void *)(controller->mmio_base + IOWIN), value);
}

static bool ioapic_controller_for_gsi(uint32_t gsi, uint32_t *out_index,
                                      uint32_t *out_pin)
{
    for (uint32_t i = 0; i < g_ioapic_registry.controller_count; i++)
    {
        const ioapic_controller_t *controller =
            &g_ioapic_registry.controllers[i];
        uint64_t end = (uint64_t)controller->gsi_base +
                       controller->redirection_count;
        if (gsi >= controller->gsi_base && (uint64_t)gsi < end)
        {
            if (out_index) *out_index = i;
            if (out_pin) *out_pin = gsi - controller->gsi_base;
            return true;
        }
    }
    return false;
}

static bool ioapic_ranges_valid(void)
{
    for (uint32_t i = 0; i < g_ioapic_registry.controller_count; i++)
    {
        const ioapic_controller_t *a = &g_ioapic_registry.controllers[i];
        uint64_t a_end = (uint64_t)a->gsi_base + a->redirection_count;
        if (!a->mmio_base || !a->redirection_count || a_end > UINT32_MAX + 1ULL)
            return false;
        for (uint32_t j = i + 1; j < g_ioapic_registry.controller_count; j++)
        {
            const ioapic_controller_t *b =
                &g_ioapic_registry.controllers[j];
            uint64_t b_end = (uint64_t)b->gsi_base + b->redirection_count;
            if ((uint64_t)a->gsi_base < b_end &&
                (uint64_t)b->gsi_base < a_end)
                return false;
        }
    }
    return true;
}

static char *ioapic_append_text(char *out, const char *text)
{
    while (*text) *out++ = *text++;
    return out;
}

static char *ioapic_append_u32(char *out, uint32_t value)
{
    char digits[10];
    uint32_t count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; }
    while (value);
    while (count) *out++ = digits[--count];
    return out;
}

static void ioapic_log_quiescent(void)
{
    char line[160];
    char *p = ioapic_append_text(line,
        "[IRQ][IOAPIC] QUIESCENT controllers=");
    p = ioapic_append_u32(p, g_ioapic_registry.controller_count);
    p = ioapic_append_text(p, " entries=");
    p = ioapic_append_u32(p, g_ioapic_registry.total_entries);
    p = ioapic_append_text(p, " masked=");
    p = ioapic_append_u32(p, g_ioapic_registry.masked_entries);
    *p++ = '\n';
    *p = 0;
    serial_write_all(line);
}

bool ioapic_init_all(void)
{
    if (g_ioapic_registry.initialized)
        return g_ioapic_registry.quiescent;
    g_ioapic_registry.initialized = 1;

    uint32_t count = madt_get_ioapic_count();
    if (!count)
        return false;
    g_ioapic_registry.controllers =
        kmalloc(sizeof(ioapic_controller_t) * count);
    if (!g_ioapic_registry.controllers)
        return false;
    memset(g_ioapic_registry.controllers, 0,
           sizeof(ioapic_controller_t) * count);
    g_ioapic_registry.controller_count = count;

    for (uint32_t i = 0; i < count; i++)
    {
        madt_ioapic_t source;
        if (!madt_ioapic_at(i, &source) || !source.mmio_base)
            return false;
        ioapic_controller_t *controller =
            &g_ioapic_registry.controllers[i];
        controller->id = source.id;
        controller->mmio_base = source.mmio_base;
        controller->gsi_base = source.gsi_base;
        spinlock_init(&controller->lock);

        uint64_t base_page = controller->mmio_base & ~0xFFFULL;
        paging_map(base_page, base_page,
                   PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        __asm__ volatile("invlpg (%0)" : : "r"(base_page) : "memory");
        controller->mapped = 1;

        irq_flags_t flags = spin_lock_irqsave(&controller->lock);
        uint32_t version = ioapic_read_locked(controller, IOAPICVER);
        spin_unlock_irqrestore(&controller->lock, flags);
        controller->redirection_count = ((version >> 16) & 0xFFu) + 1u;
        if (!controller->redirection_count ||
            UINT32_MAX - g_ioapic_registry.total_entries <
                controller->redirection_count)
            return false;
        g_ioapic_registry.total_entries += controller->redirection_count;
    }

    if (!ioapic_ranges_valid())
        return false;
    g_ioapic_registry.route_capacity = g_ioapic_registry.total_entries;
    g_ioapic_registry.routes =
        kmalloc(sizeof(interrupt_route_t) * g_ioapic_registry.route_capacity);
    if (!g_ioapic_registry.routes)
        return false;
    memset(g_ioapic_registry.routes, 0,
           sizeof(interrupt_route_t) * g_ioapic_registry.route_capacity);
    return ioapic_mask_all();
}

uint32_t ioapic_controller_count(void)
{
    return g_ioapic_registry.controller_count;
}

uint32_t ioapic_total_entries(void)
{
    return g_ioapic_registry.total_entries;
}

bool ioapic_find_for_gsi(uint32_t gsi,
                         const ioapic_controller_t **out_controller,
                         uint32_t *out_pin)
{
    if (!out_controller || !out_pin)
        return false;
    uint32_t index;
    if (!ioapic_controller_for_gsi(gsi, &index, out_pin))
        return false;
    *out_controller = &g_ioapic_registry.controllers[index];
    return true;
}

bool ioapic_mask_all(void)
{
    if (!g_ioapic_registry.controllers)
        return false;
    uint32_t masked = 0;
    bool all_ok = true;
    for (uint32_t i = 0; i < g_ioapic_registry.controller_count; i++)
    {
        ioapic_controller_t *controller =
            &g_ioapic_registry.controllers[i];
        bool controller_ok = true;
        irq_flags_t flags = spin_lock_irqsave(&controller->lock);
        for (uint32_t pin = 0; pin < controller->redirection_count; pin++)
        {
            uint32_t reg = IOREDTBL + pin * 2u;
            uint32_t lower = ioapic_read_locked(controller, reg);
            ioapic_write_locked(controller, reg, lower | IOAPIC_REDIR_MASKED);
            uint32_t readback = ioapic_read_locked(controller, reg);
            if (readback & IOAPIC_REDIR_MASKED)
                masked++;
            else
                controller_ok = false;
        }
#ifdef HOBBYOS_IRQ_NEGATIVE_IOAPIC_ROUTE_ACTIVE
        if (i == 0 && controller->redirection_count)
        {
            uint32_t lower = ioapic_read_locked(controller, IOREDTBL);
            ioapic_write_locked(controller, IOREDTBL,
                                lower & ~IOAPIC_REDIR_MASKED);
            controller_ok = false;
            if (masked) masked--;
        }
#endif
        spin_unlock_irqrestore(&controller->lock, flags);
        controller->quiescent = controller_ok ? 1u : 0u;
        all_ok = all_ok && controller_ok;
    }
    g_ioapic_registry.masked_entries = masked;
    g_ioapic_registry.quiescent =
        all_ok && masked == g_ioapic_registry.total_entries;
    if (g_ioapic_registry.quiescent)
        ioapic_log_quiescent();
    return g_ioapic_registry.quiescent;
}

bool ioapic_snapshot(ioapic_controller_t *out, uint32_t capacity,
                     uint32_t *out_count)
{
    if (!out_count)
        return false;
    *out_count = g_ioapic_registry.controller_count;
    if (!out || capacity < g_ioapic_registry.controller_count)
        return false;
    for (uint32_t i = 0; i < g_ioapic_registry.controller_count; i++)
        out[i] = g_ioapic_registry.controllers[i];
    return true;
}

bool ioapic_controller_at(uint32_t index, ioapic_controller_t *out)
{
    if (!out || index >= g_ioapic_registry.controller_count)
        return false;
    *out = g_ioapic_registry.controllers[index];
    return true;
}

bool ioapic_registry_snapshot(ioapic_registry_snapshot_t *out)
{
    if (!out)
        return false;
    *out = (ioapic_registry_snapshot_t){
        .controllers = g_ioapic_registry.controller_count,
        .total_entries = g_ioapic_registry.total_entries,
        .masked_entries = g_ioapic_registry.masked_entries,
        .prepared_routes = g_ioapic_registry.route_count,
        .initialized = g_ioapic_registry.initialized,
        .quiescent = g_ioapic_registry.quiescent};
    for (uint32_t i = 0; i < g_ioapic_registry.route_count; i++)
        if (!g_ioapic_registry.routes[i].masked)
            out->enabled_routes++;
    return true;
}

bool ioapic_encode_redirection(uint8_t vector, uint32_t destination_apic_id,
                               irq_polarity_t polarity,
                               irq_trigger_t trigger, bool masked,
                               uint32_t *out_lower, uint32_t *out_upper)
{
    if (!out_lower || !out_upper || vector < 32u ||
        destination_apic_id > 0xFFu ||
        (polarity != IRQ_POLARITY_HIGH && polarity != IRQ_POLARITY_LOW) ||
        (trigger != IRQ_TRIGGER_EDGE && trigger != IRQ_TRIGGER_LEVEL))
        return false;
    uint32_t lower = vector;
    if (polarity == IRQ_POLARITY_LOW) lower |= IOAPIC_REDIR_POLARITY_LOW;
    if (trigger == IRQ_TRIGGER_LEVEL) lower |= IOAPIC_REDIR_TRIGGER_LEVEL;
    if (masked) lower |= IOAPIC_REDIR_MASKED;
    *out_lower = lower;
    *out_upper = destination_apic_id << 24;
    return true;
}

static interrupt_route_t *ioapic_route_find(uint32_t gsi)
{
    for (uint32_t i = 0; i < g_ioapic_registry.route_count; i++)
        if (g_ioapic_registry.routes[i].gsi == gsi)
            return &g_ioapic_registry.routes[i];
    return NULL;
}

bool ioapic_route_prepare(const interrupt_route_t *route)
{
    if (!route || route->owner == INTERRUPT_ROUTE_OWNER_UNKNOWN ||
        !g_ioapic_registry.quiescent)
        return false;
    uint32_t controller_index;
    uint32_t pin;
    if (!ioapic_controller_for_gsi(route->gsi, &controller_index, &pin))
        return false;
    uint32_t lower;
    uint32_t upper;
    if (!ioapic_encode_redirection(route->vector,
                                   route->destination_apic_id,
                                   route->polarity, route->trigger, true,
                                   &lower, &upper))
        return false;

    interrupt_route_t *existing = ioapic_route_find(route->gsi);
    if (existing && existing->owner != route->owner)
        return false;
    if (!existing)
    {
        if (g_ioapic_registry.route_count >= g_ioapic_registry.route_capacity)
            return false;
        existing = &g_ioapic_registry.routes[g_ioapic_registry.route_count++];
    }

    ioapic_controller_t *controller =
        &g_ioapic_registry.controllers[controller_index];
    uint32_t reg = IOREDTBL + pin * 2u;
    irq_flags_t flags = spin_lock_irqsave(&controller->lock);
    ioapic_write_locked(controller, reg, lower | IOAPIC_REDIR_MASKED);
    ioapic_write_locked(controller, reg + 1u, upper);
    ioapic_write_locked(controller, reg, lower | IOAPIC_REDIR_MASKED);
    uint32_t lower_read = ioapic_read_locked(controller, reg);
    uint32_t upper_read = ioapic_read_locked(controller, reg + 1u);
    spin_unlock_irqrestore(&controller->lock, flags);
    if (lower_read != (lower | IOAPIC_REDIR_MASKED) || upper_read != upper)
        return false;

    *existing = *route;
    existing->controller_index = controller_index;
    existing->pin = pin;
    existing->lower_value = lower | IOAPIC_REDIR_MASKED;
    existing->upper_value = upper;
    existing->prepared = 1;
    existing->masked = 1;
    return true;
}

bool ioapic_route_enable(uint32_t gsi)
{
    interrupt_route_t *route = ioapic_route_find(gsi);
    if (!route || !route->prepared ||
        route->controller_index >= g_ioapic_registry.controller_count)
        return false;
    ioapic_controller_t *controller =
        &g_ioapic_registry.controllers[route->controller_index];
    uint32_t reg = IOREDTBL + route->pin * 2u;
    uint32_t final_lower = route->lower_value & ~IOAPIC_REDIR_MASKED;
    irq_flags_t flags = spin_lock_irqsave(&controller->lock);
    uint32_t before = ioapic_read_locked(controller, reg);
    uint32_t upper = ioapic_read_locked(controller, reg + 1u);
    bool prepared = (before & IOAPIC_REDIR_MASKED) &&
                    upper == route->upper_value;
    if (prepared)
        ioapic_write_locked(controller, reg, final_lower);
    uint32_t readback = ioapic_read_locked(controller, reg);
    spin_unlock_irqrestore(&controller->lock, flags);
    if (!prepared || readback != final_lower)
        return false;
    route->lower_value = final_lower;
    if (__atomic_load_n(&route->masked, __ATOMIC_ACQUIRE) &&
        g_ioapic_registry.masked_entries)
        g_ioapic_registry.masked_entries--;
    __atomic_store_n(&route->masked, 0, __ATOMIC_RELEASE);
    return true;
}

bool ioapic_route_disable(uint32_t gsi)
{
    interrupt_route_t *route = ioapic_route_find(gsi);
    if (!route || !route->prepared ||
        route->controller_index >= g_ioapic_registry.controller_count)
        return false;
    ioapic_controller_t *controller =
        &g_ioapic_registry.controllers[route->controller_index];
    uint32_t reg = IOREDTBL + route->pin * 2u;
    irq_flags_t flags = spin_lock_irqsave(&controller->lock);
    uint32_t lower = ioapic_read_locked(controller, reg) |
                     IOAPIC_REDIR_MASKED;
    ioapic_write_locked(controller, reg, lower);
    uint32_t readback = ioapic_read_locked(controller, reg);
    spin_unlock_irqrestore(&controller->lock, flags);
    if (!(readback & IOAPIC_REDIR_MASKED))
        return false;
    route->lower_value = readback;
    if (!__atomic_load_n(&route->masked, __ATOMIC_ACQUIRE))
        g_ioapic_registry.masked_entries++;
    __atomic_store_n(&route->masked, 1, __ATOMIC_RELEASE);
    return true;
}

bool ioapic_route_snapshot(uint32_t gsi, interrupt_route_t *out)
{
    if (!out)
        return false;
    interrupt_route_t *route = ioapic_route_find(gsi);
    if (!route)
        return false;
    *out = *route;
    out->masked = __atomic_load_n(&route->masked, __ATOMIC_ACQUIRE);
    return true;
}

uint32_t ioapic_route_count(void)
{
    return g_ioapic_registry.route_count;
}

bool ioapic_route_at(uint32_t index, interrupt_route_t *out)
{
    if (!out || index >= g_ioapic_registry.route_count)
        return false;
    return ioapic_route_snapshot(g_ioapic_registry.routes[index].gsi, out);
}

bool ioapic_disable_vector(uint8_t vector)
{
    bool found = false;
    bool ok = true;
    for (uint32_t i = 0; i < g_ioapic_registry.route_count; i++)
    {
        if (g_ioapic_registry.routes[i].vector != vector)
            continue;
        found = true;
        ok = ioapic_route_disable(g_ioapic_registry.routes[i].gsi) && ok;
    }
    return found && ok;
}

bool ioapic_unknown_routes_masked(uint32_t *out_masked,
                                  uint32_t *out_unknown)
{
    uint32_t masked = 0;
    uint32_t unknown = 0;
    bool ok = true;
    for (uint32_t i = 0; i < g_ioapic_registry.controller_count; i++)
    {
        ioapic_controller_t *controller =
            &g_ioapic_registry.controllers[i];
        irq_flags_t flags = spin_lock_irqsave(&controller->lock);
        for (uint32_t pin = 0; pin < controller->redirection_count; pin++)
        {
            uint32_t gsi = controller->gsi_base + pin;
            if (ioapic_route_find(gsi))
                continue;
            unknown++;
            uint32_t lower = ioapic_read_locked(
                controller, IOREDTBL + pin * 2u);
            if (lower & IOAPIC_REDIR_MASKED) masked++;
            else ok = false;
        }
        spin_unlock_irqrestore(&controller->lock, flags);
    }
    if (out_masked) *out_masked = masked;
    if (out_unknown) *out_unknown = unknown;
    return ok && masked == unknown;
}

const char *ioapic_route_owner_name(interrupt_route_owner_t owner)
{
    switch (owner)
    {
    case INTERRUPT_ROUTE_OWNER_KEYBOARD: return "keyboard";
    case INTERRUPT_ROUTE_OWNER_HPET: return "hpet";
    default: return "unknown";
    }
}

bool ioapic_model_selftest(void)
{
    uint32_t lower;
    uint32_t upper;
    bool ok = ioapic_encode_redirection(33, 7, IRQ_POLARITY_HIGH,
                                        IRQ_TRIGGER_EDGE, true,
                                        &lower, &upper);
    ok = ok && lower == (33u | IOAPIC_REDIR_MASKED) &&
         upper == (7u << 24);
    ok = ok && ioapic_encode_redirection(32, 255, IRQ_POLARITY_LOW,
                                         IRQ_TRIGGER_LEVEL, false,
                                         &lower, &upper);
    ok = ok && (lower & IOAPIC_REDIR_POLARITY_LOW) &&
         (lower & IOAPIC_REDIR_TRIGGER_LEVEL) &&
         !(lower & IOAPIC_REDIR_MASKED);
    ok = ok && !ioapic_encode_redirection(31, 0, IRQ_POLARITY_HIGH,
                                          IRQ_TRIGGER_EDGE, true,
                                          &lower, &upper);
    ok = ok && !ioapic_encode_redirection(33, 256, IRQ_POLARITY_HIGH,
                                          IRQ_TRIGGER_EDGE, true,
                                          &lower, &upper);
    uint32_t safe_order[3] = {
        33u | IOAPIC_REDIR_MASKED,
        7u << 24,
        33u | IOAPIC_REDIR_MASKED};
    ok = ok && (safe_order[0] & IOAPIC_REDIR_MASKED) &&
         (safe_order[2] & IOAPIC_REDIR_MASKED) &&
         safe_order[1] == (7u << 24);
    return ok;
}
