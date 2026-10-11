#include "lapic_delivery.h"
#include "../core/interrupt_vectors.h"

lapic_delivery_result_t lapic_delivery_encode(bool x2, uint32_t destination,
    uint32_t vector, lapic_message_t message, uint64_t *out)
{
    if (!out) return LAPIC_DELIVERY_INVALID;
    if (destination == UINT32_MAX || (!x2 && destination >= 255u))
        return LAPIC_DELIVERY_DESTINATION;
    uint32_t low;
    switch (message) {
    case LAPIC_MESSAGE_FIXED:
        if (vector != INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE &&
            vector != INT_VECTOR_CPU_REQUEST && vector != INT_VECTOR_LEGACY_HALT)
            return LAPIC_DELIVERY_INVALID;
        low = 0x4000u | vector;
        break;
    case LAPIC_MESSAGE_INIT_ASSERT:
        if (vector) return LAPIC_DELIVERY_INVALID;
        low = 0xC500u;
        break;
    case LAPIC_MESSAGE_INIT_DEASSERT:
        if (vector || x2) return LAPIC_DELIVERY_INVALID;
        low = 0x8500u;
        break;
    case LAPIC_MESSAGE_STARTUP:
        if (!vector || vector > 255u) return LAPIC_DELIVERY_INVALID;
        low = 0x4600u | vector;
        break;
    default: return LAPIC_DELIVERY_INVALID;
    }
    *out = ((uint64_t)destination << 32) | low;
    return LAPIC_DELIVERY_SUBMITTED;
}

static bool wait_idle(const lapic_delivery_ops_t *ops, void *context)
{
    for (uint32_t n = 0; n < LAPIC_ICR_POLL_LIMIT; ++n) {
        if (!ops->pending(context)) return true;
        ops->relax(context);
    }
    return false;
}

lapic_delivery_result_t lapic_delivery_send(const lapic_delivery_ops_t *ops,
    void *context, bool available, bool x2, uint32_t destination,
    uint32_t vector, lapic_message_t message)
{
    uint64_t value;
    lapic_delivery_result_t result =
        lapic_delivery_encode(x2, destination, vector, message, &value);
    if (result != LAPIC_DELIVERY_SUBMITTED) return result;
    if (!available) return LAPIC_DELIVERY_UNAVAILABLE;
    if (!ops || !ops->irq_save || !ops->irq_restore || !ops->pending ||
        !ops->write32 || !ops->write64 || !ops->publish || !ops->relax)
        return LAPIC_DELIVERY_INVALID;
    uint64_t flags = ops->irq_save(context);
    if (!x2 && !wait_idle(ops, context)) {
        result = LAPIC_DELIVERY_PRE_TIMEOUT;
        goto out;
    }
    ops->publish(context);
    if (x2) {
        if (!ops->write64(context, 0x830u, value))
            result = LAPIC_DELIVERY_WRITE_FAILED;
    } else if (!ops->write32(context, 0x310u, destination << 24)) {
        result = LAPIC_DELIVERY_WRITE_FAILED;
    } else if (!ops->write32(context, 0x300u, (uint32_t)value) ||
               !wait_idle(ops, context)) {
        /* After the triggering write, absence of delivery cannot be proved. */
        result = LAPIC_DELIVERY_INDETERMINATE;
    }
out:
    ops->irq_restore(context, flags);
    return result;
}
