#ifndef LAPIC_DELIVERY_H
#define LAPIC_DELIVERY_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LAPIC_DELIVERY_SUBMITTED = 0,
    LAPIC_DELIVERY_INVALID,
    LAPIC_DELIVERY_DESTINATION,
    LAPIC_DELIVERY_UNAVAILABLE,
    LAPIC_DELIVERY_PRE_TIMEOUT,
    LAPIC_DELIVERY_WRITE_FAILED,
    LAPIC_DELIVERY_INDETERMINATE
} lapic_delivery_result_t;
typedef enum {
    LAPIC_MESSAGE_FIXED, LAPIC_MESSAGE_INIT_ASSERT,
    LAPIC_MESSAGE_INIT_DEASSERT, LAPIC_MESSAGE_STARTUP
} lapic_message_t;

/* SUBMITTED reports only the local controller transaction, never remote work.
 * An indeterminate result may still deliver. Its payload must remain alive. */
#define LAPIC_ICR_POLL_LIMIT 100000u
typedef struct {
    uint64_t (*irq_save)(void *);
    void (*irq_restore)(void *, uint64_t);
    bool (*pending)(void *);
    bool (*write32)(void *, uint32_t, uint32_t);
    bool (*write64)(void *, uint32_t, uint64_t);
    void (*publish)(void *);
    void (*relax)(void *);
} lapic_delivery_ops_t;
lapic_delivery_result_t lapic_delivery_encode(bool x2, uint32_t destination,
    uint32_t vector, lapic_message_t message, uint64_t *out);
lapic_delivery_result_t lapic_delivery_send(const lapic_delivery_ops_t *ops,
    void *context, bool available, bool x2, uint32_t destination,
    uint32_t vector, lapic_message_t message);
#endif
