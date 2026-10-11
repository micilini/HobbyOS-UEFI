/* Actual enumeration/parser functions with explicit device/command substitutes. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/drivers/usb/xhci/xhci.h"
#include "../kernel/src/drivers/usb/xhci/usb_hotplug.h"
#include "../kernel/src/core/spinlock.h"
#include "../kernel/src/graphics/console.h"

xhci_controller_t xhci_driver;
static spinlock_t g_ep0_lock;
static _Alignas(4096) unsigned char dma[8][8192];
static xhci_cap_regs_t cap;
static uint32_t op[sizeof(xhci_op_regs_t)/sizeof(uint32_t)];
static uint64_t dcbaa[64];
static unsigned allocations, enables, configured, queued, released, cases;
static unsigned char descriptor[64];
static uint16_t descriptor_length;
static void *descriptor_copy;
static bool attached, stale_after_descriptor, descriptor_failure, endpoint_failure;
#define XHCI_OP_REG(member) (&op[offsetof(xhci_op_regs_t,member)/sizeof(uint32_t)])
uint32_t xhci_read32(volatile uint32_t *p) { return *p; }
void xhci_process_events(void) {}
static int xhci_recover_command_ring(void) { return 0; }
void console_write_debug(const char *s) { (void)s; }
void console_print_dec_debug(uint64_t n) { (void)n; }
void console_print_hex_debug(uint64_t n) { (void)n; }
void console_set_color_debug(uint32_t f,uint32_t b) { (void)f;(void)b; }
void timer_sleep(uint64_t ms) { assert(ms<=20); }
void *xhci_dma_allocate(size_t bytes,size_t alignment)
{ assert(bytes<=8192 && alignment==4096 && allocations<8);return dma[allocations++]; }
uint64_t xhci_dma_address(const void *p,size_t bytes)
{ assert(p && bytes<=8192);return (uintptr_t)p; }
void kfree(void *p) { assert(p==descriptor_copy);free(p);descriptor_copy=NULL;++released; }
irq_flags_t spin_lock_irqsave(spinlock_t *p) { assert(!p->locked);p->locked=1;return 512; }
void spin_unlock_irqrestore(spinlock_t *p,irq_flags_t f) { assert(p->locked&&f==512);p->locked=0; }
bool usb_hotplug_root_current(uint8_t port,uint64_t generation)
{ return attached && port==1 && generation==19; }
bool usb_hotplug_root_address(uint8_t port,uint64_t generation,uint8_t slot)
{ return slot==3 && usb_hotplug_root_current(port,generation); }
bool usb_hub_context_current(const usb_device_context_t *ctx)
{ return ctx && ctx->parent_hub_slot==1 && usb_hotplug_root_current(ctx->root_port,ctx->root_generation); }
int xhci_configure_device(int port,int speed) { (void)port;(void)speed;assert(0);return 0; }
uint8_t xhci_send_command_wait(uint32_t type,uint64_t param,uint32_t control)
{
 (void)param;(void)control;assert(!g_ep0_lock.locked);
 if(type==TRB_TYPE_NOOP)return 1;
 if(type==TRB_TYPE_ENABLE_SLOT){++enables;return 3;}
 assert(type==TRB_TYPE_ADDRESS_DEVICE);return 3;
}
int xhci_evaluate_context(uint8_t slot,uint16_t mps,int port,int speed)
{ (void)slot;(void)mps;(void)port;(void)speed;return 1; }
int xhci_get_descriptor_device(uint8_t slot,usb_device_descriptor_t *out,uint16_t length)
{ assert(slot==3&&length==18);*out=(usb_device_descriptor_t){.Length=18,.DescriptorType=1,.DeviceClass=0xef};return !descriptor_failure; }
void *xhci_get_config_descriptor(uint8_t slot,uint16_t *length)
{
 assert(slot==3&&!descriptor_copy);*length=descriptor_length;
 descriptor_copy=malloc(descriptor_length);assert(descriptor_copy);
 memcpy(descriptor_copy,descriptor,descriptor_length);
 if(stale_after_descriptor)attached=false;
 return descriptor_copy;
}
int xhci_set_configuration(uint8_t slot,uint8_t value)
{ assert(slot==3&&value==1);++configured;return 1; }
int xhci_set_protocol(uint8_t slot,uint8_t iface,uint8_t protocol)
{ assert(slot==3&&iface==0&&protocol==0);return 1; }
int xhci_set_idle(uint8_t slot,uint8_t iface,uint8_t duration,uint8_t report)
{ assert(slot==3&&iface==0&&duration==0&&report==0);return 1; }
int xhci_configure_endpoint_irq(uint8_t slot,int port,int speed,uint8_t ep,uint16_t mps,uint8_t interval)
{ assert(slot==3&&port==0&&speed==3&&ep==0x81&&mps==8&&interval==10);return endpoint_failure?1:0; }
static void xhci_queue_kbd_request_buf(uint8_t slot,uint8_t *p)
{ (void)p;assert(slot==3);++queued; }
int usb_hub_enumerate(uint8_t s,uint8_t r,uint32_t route,uint8_t depth,uint8_t parent,uint8_t port,int ss)
{ (void)s;(void)r;(void)route;(void)depth;(void)parent;(void)port;(void)ss;assert(0);return -1; }

#include USB_ENUMERATION_ACTUAL_SOURCE

static void reset(bool keyboard)
{
 free(descriptor_copy);descriptor_copy=NULL;
 memset(&xhci_driver,0,sizeof(xhci_driver));memset(&op,0,sizeof(op));
 xhci_driver.cap_regs=&cap;xhci_driver.op_regs=(xhci_op_regs_t *)op;xhci_driver.dcbaa=dcbaa;xhci_driver.max_ports=16;
 allocations=enables=configured=queued=released=0;
 attached=true;stale_after_descriptor=descriptor_failure=endpoint_failure=false;
 const unsigned char data[]={9,2,25,0,1,1,0,0x80,50,9,4,0,0,1,14,0,0,0,7,5,0x81,3,8,0,10};
 memcpy(descriptor,data,sizeof(data));descriptor_length=sizeof(data);
 if(keyboard){descriptor[14]=3;descriptor[15]=descriptor[16]=1;}
}
static int enumerate(bool hub)
{
 ++cases;
 usb_device_context_t ctx={.root_port=1,.root_generation=19,.parent_hub_slot=1,.speed=3};
 return hub?xhci_configure_device_with_context(0,3,&ctx):xhci_configure_owned_root(0,3,19);
}
int main(void)
{
 for(unsigned hub=0;hub<2;++hub){
  reset(false);
#ifdef EXPECT_LEGACY_ENUMERATION
  assert(enumerate(hub)==0 && enables==1 && !configured && !queued);
#else
  assert(enumerate(hub)==3 && enables==1 && !configured && !queued && released==1);
#endif
  reset(true);assert(enumerate(hub)==3 && configured==1 && queued==XHCI_KBD_PIPE_DEPTH);
  reset(true);endpoint_failure=true;assert(enumerate(hub)==0 && configured==1 && !queued);
  reset(false);stale_after_descriptor=true;assert(enumerate(hub)==0 && !configured && !queued);
  reset(false);descriptor_failure=true;assert(enumerate(hub)==0 && !configured && !queued);
#ifndef EXPECT_LEGACY_ENUMERATION
  reset(true);descriptor_length=18;descriptor[2]=18;descriptor[13]=0;
  assert(enumerate(hub)==0 && !configured && !queued);
  for(unsigned kind=0;kind<8;++kind){
   reset(false);
   switch(kind){
    case 0:descriptor[0]=8;break;
    case 1:descriptor[1]=1;break;
    case 2:descriptor[2]=26;break;
    case 3:descriptor[5]=0;break;
    case 4:descriptor[9]=0;break;
    case 5:descriptor[9]=8;break;
    case 6:descriptor[18]=6;break;
    case 7:descriptor[18]=8;break;
   }
   assert(enumerate(hub)==0 && !configured && !queued && released==1);
  }
#endif
 }
#ifndef EXPECT_LEGACY_ENUMERATION
 usb_device_info_t info;
 for(unsigned n=0;n<9;++n){
  void *b=malloc(n?n:1);assert(b);memset(b,0,n);xhci_parse_config(b,n,&info);
  assert(!info.found&&!info.descriptor_valid);free(b);++cases;
 }
 xhci_parse_config(NULL,9,&info);assert(!info.descriptor_valid);
 xhci_parse_config(NULL,0,NULL);
#endif
 free(descriptor_copy);
 printf("USB_ENUMERATION_HOST cases=%u status=PASS authority=actual_functions_substituted_device\n",cases);
 return 0;
}
