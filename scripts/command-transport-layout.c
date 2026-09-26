#include <stddef.h>
#include <stdio.h>

#include "../kernel/src/drivers/usb/xhci/xhci.h"

#define ARRAY_COUNT(value) (sizeof(value) / sizeof((value)[0]))

int main(void)
{
    xhci_controller_t *controller = NULL;

    printf("{\"schema\":1,\"controller_size\":%zu,"
           "\"slot_count\":%zu,\"key_count\":%zu,"
           "\"prev_keys_offset\":%zu,\"prev_keys_size\":%zu,"
           "\"prev_mods_offset\":%zu,\"prev_mods_size\":%zu,"
           "\"repeat_key_offset\":%zu,\"repeat_key_size\":%zu,"
           "\"repeat_mods_offset\":%zu,\"repeat_mods_size\":%zu,"
           "\"repeat_active_offset\":%zu,\"repeat_active_size\":%zu}\n",
           sizeof(*controller),
           ARRAY_COUNT(controller->slot_kbd_prev_keys),
           ARRAY_COUNT(controller->slot_kbd_prev_keys[0]),
           offsetof(xhci_controller_t, slot_kbd_prev_keys),
           sizeof(controller->slot_kbd_prev_keys),
           offsetof(xhci_controller_t, slot_kbd_prev_mods),
           sizeof(controller->slot_kbd_prev_mods),
           offsetof(xhci_controller_t, slot_kbd_repeat_key),
           sizeof(controller->slot_kbd_repeat_key),
           offsetof(xhci_controller_t, slot_kbd_repeat_mods),
           sizeof(controller->slot_kbd_repeat_mods),
           offsetof(xhci_controller_t, slot_kbd_repeat_active),
           sizeof(controller->slot_kbd_repeat_active));
    return 0;
}
