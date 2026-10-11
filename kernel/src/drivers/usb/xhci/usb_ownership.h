#ifndef USB_OWNERSHIP_H
#define USB_OWNERSHIP_H
#include <stdbool.h>
#include <stdint.h>

/* Called under the owner's short metadata lock. A disconnected generation is
 * never reused, including by a queued timer carrying an older work ticket. */
typedef enum { USB_ROOT_DETACHED, USB_ROOT_PENDING, USB_ROOT_ENUMERATING,
               USB_ROOT_CONFIGURED, USB_ROOT_RETAINED } usb_root_phase_t;
typedef struct {
    uint64_t generation;
    usb_root_phase_t phase;
    uint8_t slot;
} usb_root_owner_t;
bool usb_root_claim(usb_root_owner_t *owner, uint64_t *generation);
bool usb_root_address(usb_root_owner_t *owner, uint64_t generation, uint8_t slot);
bool usb_root_finish(usb_root_owner_t *owner, uint64_t generation, bool success);
uint8_t usb_root_detach(usb_root_owner_t *owner);
bool usb_root_matches(const usb_root_owner_t *owner, uint64_t generation);

/* The lane is controller-owned and remains quarantined after an uncertain TD.
 * Hardware completion carries a TRB address, not the software generation.
 * Only a terminal status-stage success permits reuse of a completed lane. */
typedef enum { USB_EP0_IDLE, USB_EP0_PREPARING, USB_EP0_ACTIVE, USB_EP0_OBSERVED,
               USB_EP0_COMPLETE, USB_EP0_QUARANTINED } usb_ep0_phase_t;
typedef struct {
    usb_ep0_phase_t phase;
    uint64_t generation, lifecycle, trb[3];
    uint8_t slot, count, result;
} usb_ep0_owner_t;
bool usb_ep0_claim(usb_ep0_owner_t *owner, uint8_t slot, uint64_t lifecycle);
bool usb_ep0_publish(usb_ep0_owner_t *owner, const uint64_t *trb, uint8_t count);
bool usb_ep0_event(usb_ep0_owner_t *owner, uint8_t slot, uint8_t endpoint,
                   uint64_t lifecycle, uint64_t trb, bool event_data, uint8_t cc);
bool usb_ep0_retire(usb_ep0_owner_t *owner);
void usb_ep0_event_retired(usb_ep0_owner_t *owner);
void usb_ep0_timeout(usb_ep0_owner_t *owner);
/* xHCI speed IDs: full=1, low=2, high=3. Preserve inherited HS translator. */
bool usb_translator(uint8_t speed, uint8_t parent_speed, uint8_t parent_slot,
                    uint8_t port, uint8_t upstream_slot, uint8_t upstream_port,
                    uint8_t *slot, uint8_t *tt_port);
#endif
