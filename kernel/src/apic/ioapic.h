#ifndef IOAPIC_H
#define IOAPIC_H

#include "../acpi/madt.h"
#include "../core/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

#define IOREGSEL 0x00u
#define IOWIN 0x10u

#define IOAPICID 0x00u
#define IOAPICVER 0x01u
#define IOREDTBL 0x10u

#define IOAPIC_REDIR_POLARITY_LOW (1u << 13)
#define IOAPIC_REDIR_TRIGGER_LEVEL (1u << 15)
#define IOAPIC_REDIR_MASKED (1u << 16)

typedef union
{
    struct
    {
        uint64_t vector : 8;
        uint64_t delv_mode : 3;
        uint64_t dest_mode : 1;
        uint64_t delv_status : 1;
        uint64_t pin_polarity : 1;
        uint64_t remote_irr : 1;
        uint64_t trigger_mode : 1;
        uint64_t mask : 1;
        uint64_t reserved : 39;
        uint64_t destination : 8;
    } __attribute__((packed));
    struct
    {
        uint32_t lower;
        uint32_t upper;
    };
} IoApicRedirEntry;

typedef struct
{
    uint8_t id;
    uint64_t mmio_base;
    uint32_t gsi_base;
    uint32_t redirection_count;
    spinlock_t lock;
    uint8_t mapped;
    uint8_t quiescent;
} ioapic_controller_t;

typedef enum
{
    INTERRUPT_ROUTE_OWNER_UNKNOWN = 0,
    INTERRUPT_ROUTE_OWNER_KEYBOARD,
    INTERRUPT_ROUTE_OWNER_HPET
} interrupt_route_owner_t;

typedef struct
{
    uint32_t gsi;
    uint32_t controller_index;
    uint32_t pin;
    uint32_t destination_apic_id;
    uint32_t lower_value;
    uint32_t upper_value;
    uint8_t vector;
    irq_polarity_t polarity;
    irq_trigger_t trigger;
    interrupt_route_owner_t owner;
    uint8_t prepared;
    uint8_t masked;
} interrupt_route_t;

typedef struct
{
    uint32_t controllers;
    uint32_t total_entries;
    uint32_t masked_entries;
    uint32_t prepared_routes;
    uint32_t enabled_routes;
    uint8_t initialized;
    uint8_t quiescent;
} ioapic_registry_snapshot_t;

bool ioapic_init_all(void);
uint32_t ioapic_controller_count(void);
uint32_t ioapic_total_entries(void);
bool ioapic_find_for_gsi(uint32_t gsi,
                         const ioapic_controller_t **out_controller,
                         uint32_t *out_pin);
bool ioapic_mask_all(void);
bool ioapic_snapshot(ioapic_controller_t *out, uint32_t capacity,
                     uint32_t *out_count);
bool ioapic_controller_at(uint32_t index, ioapic_controller_t *out);
bool ioapic_registry_snapshot(ioapic_registry_snapshot_t *out);

bool ioapic_route_prepare(const interrupt_route_t *route);
bool ioapic_route_enable(uint32_t gsi);
bool ioapic_route_disable(uint32_t gsi);
bool ioapic_route_snapshot(uint32_t gsi, interrupt_route_t *out);
uint32_t ioapic_route_count(void);
bool ioapic_route_at(uint32_t index, interrupt_route_t *out);
bool ioapic_disable_vector(uint8_t vector);
bool ioapic_unknown_routes_masked(uint32_t *out_masked,
                                  uint32_t *out_unknown);

bool ioapic_encode_redirection(uint8_t vector, uint32_t destination_apic_id,
                               irq_polarity_t polarity,
                               irq_trigger_t trigger, bool masked,
                               uint32_t *out_lower, uint32_t *out_upper);
const char *ioapic_route_owner_name(interrupt_route_owner_t owner);
bool ioapic_model_selftest(void);

#endif
