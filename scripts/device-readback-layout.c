/* Host layout facts from the actual private definitions; no kernel code runs. */
#include <stddef.h>
#include <stdio.h>
#include "../kernel/src/memory/owned_mapping.c"
#include "../kernel/src/memory/dma_buffer.c"
#include "../kernel/src/drivers/usb/xhci/xhci.h"
#define OFFSET(type,field) printf("\"" #field "\":%zu,",offsetof(type,field))
int main(void)
{
 printf("{\"schema\":1,\"view\":{");
 OFFSET(owned_view_t,view);OFFSET(owned_view_t,range);OFFSET(owned_view_t,first_page);
 OFFSET(owned_view_t,flags);OFFSET(owned_view_t,source);OFFSET(owned_view_t,cache);
 OFFSET(owned_view_t,state);OFFSET(owned_view_t,origin);
 printf("\"size\":%zu},\"dma\":{",sizeof(owned_view_t));
 OFFSET(dma_slot_t,buffer);OFFSET(dma_slot_t,state);OFFSET(dma_slot_t,allocated);OFFSET(dma_slot_t,preparing);
 printf("\"size\":%zu},\"controller\":{",sizeof(dma_slot_t));
 OFFSET(xhci_controller_t,dma_capabilities);OFFSET(xhci_controller_t,dma_maximum);
 OFFSET(xhci_controller_t,dcbaa);OFFSET(xhci_controller_t,cmd_ring_base);OFFSET(xhci_controller_t,event_ring);
 OFFSET(xhci_controller_t,erst);OFFSET(xhci_controller_t,slot_ep0_rings);OFFSET(xhci_controller_t,slot_kbd_rings);
 OFFSET(xhci_controller_t,slot_lifecycle);OFFSET(xhci_controller_t,slot_retiring);
 printf("\"size\":%zu}}\n",sizeof(xhci_controller_t));
}
