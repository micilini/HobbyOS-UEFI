#include "usb_ownership.h"
bool usb_root_claim(usb_root_owner_t *o, uint64_t *generation)
{
    if (!o || !generation || o->phase != USB_ROOT_DETACHED || o->generation == UINT64_MAX) return false;
    ++o->generation; o->phase = USB_ROOT_ENUMERATING; o->slot = 0;
    *generation = o->generation; return true;
}
bool usb_root_matches(const usb_root_owner_t *o, uint64_t generation)
{
    return o && generation && o->generation == generation &&
           (o->phase == USB_ROOT_PENDING || o->phase == USB_ROOT_ENUMERATING || o->phase == USB_ROOT_CONFIGURED);
}
bool usb_root_address(usb_root_owner_t *o, uint64_t generation, uint8_t slot)
{
    if (!usb_root_matches(o, generation) || o->phase != USB_ROOT_ENUMERATING ||
        !slot || slot >= 64 || o->slot) return false;
    o->slot = slot; return true;
}
bool usb_root_finish(usb_root_owner_t *o, uint64_t generation, bool success)
{
    if (!usb_root_matches(o, generation) || o->phase != USB_ROOT_ENUMERATING) return false;
    o->phase = success && o->slot ? USB_ROOT_CONFIGURED : USB_ROOT_RETAINED;
    return o->phase == USB_ROOT_CONFIGURED;
}
uint8_t usb_root_detach(usb_root_owner_t *o)
{
    if (!o) return 0;
    uint8_t slot = o->slot;
    /* Exhaustion fails closed: it cannot make an old lifecycle valid again. */
    if (o->generation == UINT64_MAX) o->phase = USB_ROOT_RETAINED;
    else { ++o->generation; o->phase = USB_ROOT_DETACHED; }
    o->slot = 0; return slot;
}
bool usb_ep0_claim(usb_ep0_owner_t *o, uint8_t slot, uint64_t lifecycle)
{
    if (!o || o->phase != USB_EP0_IDLE || !slot || slot >= 64 || !lifecycle || o->generation == UINT64_MAX) return false;
    ++o->generation; o->slot = slot; o->lifecycle = lifecycle;
    o->count = 0; o->result = 0; o->phase = USB_EP0_PREPARING; return true;
}
bool usb_ep0_publish(usb_ep0_owner_t *o, const uint64_t *trb, uint8_t count)
{
    if (!o || o->phase != USB_EP0_PREPARING || !trb || count < 2 || count > 3) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (!trb[i] || (trb[i] & 15)) return false;
        for (unsigned j = 0; j < i; ++j) if (trb[j] == trb[i]) return false;
    }
    for (unsigned i = 0; i < count; ++i) o->trb[i] = trb[i];
    o->count = count; o->phase = USB_EP0_ACTIVE; return true;
}
bool usb_ep0_event(usb_ep0_owner_t *o, uint8_t slot, uint8_t endpoint,
                   uint64_t lifecycle, uint64_t trb, bool event_data, uint8_t cc)
{
    if (!o || o->phase != USB_EP0_ACTIVE || slot != o->slot || endpoint != 1 ||
        lifecycle != o->lifecycle || event_data || (trb & 15) || !cc) return false;
    for (unsigned i = 0; i < o->count; ++i) if (trb == o->trb[i]) {
        if (cc == 1 && i + 1 != o->count) return false;
        /* A data-stage short packet is not retirement of the status stage. */
        if (cc == 13 && i + 1 != o->count) return false;
        o->result = cc;
        o->phase = cc == 1 ? USB_EP0_OBSERVED : USB_EP0_QUARANTINED;
        return true;
    }
    return false;
}
bool usb_ep0_retire(usb_ep0_owner_t *o)
{
    if (!o || o->phase != USB_EP0_COMPLETE) return false;
    o->phase = USB_EP0_IDLE; return true;
}
void usb_ep0_event_retired(usb_ep0_owner_t *o)
{
    /* The event consumer has advanced ERDP under its existing serialization.
     * A successful terminal TD and its consumed event may now release the lane. */
    if (o && o->phase == USB_EP0_OBSERVED) o->phase = USB_EP0_COMPLETE;
}
void usb_ep0_timeout(usb_ep0_owner_t *o)
{
    if (o && (o->phase == USB_EP0_ACTIVE || o->phase == USB_EP0_PREPARING ||
              o->phase == USB_EP0_OBSERVED)) o->phase = USB_EP0_QUARANTINED;
}
bool usb_translator(uint8_t speed, uint8_t parent_speed, uint8_t parent_slot,
                    uint8_t port, uint8_t upstream_slot, uint8_t upstream_port,
                    uint8_t *slot, uint8_t *tt_port)
{
    if (!slot || !tt_port || speed < 1 || speed > 5 || parent_speed < 1 || parent_speed > 5 ||
        !parent_slot || parent_slot >= 64 || !port || port > 15 ||
        upstream_slot >= 64 || (!!upstream_slot != !!upstream_port) || upstream_port > 15) return false;
    uint8_t selected = 0, selected_port = 0;
    if (speed == 1 || speed == 2) {
        if (parent_speed == 3) { selected = parent_slot; selected_port = port; }
        else if (parent_speed == 1 || parent_speed == 2) { selected = upstream_slot; selected_port = upstream_port; }
    }
    *slot = selected; *tt_port = selected_port; return true;
}
