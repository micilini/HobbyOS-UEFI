#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/drivers/usb/xhci/xhci.h"
#include "../kernel/src/drivers/usb/xhci/usb_hub.h"
static unsigned wire_length;
void *xhci_transfer_allocate(size_t bytes) { return calloc(1,bytes); }
bool xhci_transfer_release(void *cpu) {free(cpu);return true;}
int xhci_control_transfer(uint8_t slot, usb_setup_packet_t *setup, void *data)
{assert(slot==1);wire_length=setup->Length;memset(data,0xab,setup->Length);return 1;}
void console_write_debug(const char *s){(void)s;}
int main(void)
{
 struct {usb_hub_descriptor_t descriptor;uint8_t guard[8];} v;
 memset(&v,0xcd,sizeof(v));
 assert(usb_hub_get_descriptor(1,0,&v.descriptor));assert(wire_length==9);
 for(unsigned i=0;i<sizeof(v.guard);++i)assert(v.guard[i]==0xcd);
 usb3_hub_descriptor_t ss;assert(usb_hub_get_descriptor(1,1,&ss));assert(wire_length==12);
 puts("USB_HUB_BUFFER_HOST wire_sizes=9,12 bounded_output=PASS");
}
