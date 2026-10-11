/* Real EP0 publisher and DMA ownership with substituted IRQ/event delivery. */
#define HOBBYOS_USB_HOST 1
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/drivers/usb/xhci/xhci.c"
#include "../kernel/src/memory/physical_translation.h"
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);assert(x); } while(0)
static unsigned checks, events, waits, entered, simultaneous, active, mode, contended;
static _Thread_local bool enabled=true;
static _Thread_local unsigned caller_slot;
static _Alignas(4096) unsigned char ram[256*4096];
static unsigned next_frame;
static pthread_mutex_t allocation=PTHREAD_MUTEX_INITIALIZER;
static xhci_doorbell_regs_t bells[64];
static uint64_t last_td;
void spinlock_init(spinlock_t *p){memset(p,0,sizeof(*p));}
irq_flags_t spin_lock_irqsave(spinlock_t *p)
{irq_flags_t old=enabled?512:0;enabled=false;while(__atomic_exchange_n(&p->locked,1,__ATOMIC_ACQUIRE))sched_yield();return old;}
void spin_unlock_irqrestore(spinlock_t *p,irq_flags_t flags)
{__atomic_store_n(&p->locked,0,__ATOMIC_RELEASE);enabled=!!(flags&512);}
bool irq_is_enabled(void){return enabled;}
uint64_t physical_allocation_ceiling(void){return 0x100000000;}
bool pmm_alloc_constrained_frames(size_t count,size_t alignment,uint64_t limit,uint64_t *out)
{
 pthread_mutex_lock(&allocation);unsigned start=(next_frame+alignment-1)&~(alignment-1);
 bool ok=count<=256-start && 0x200000+(start+count)*4096<=limit;
 if(ok){*out=0x200000+start*4096;next_frame=start+count;}
 pthread_mutex_unlock(&allocation);return ok;
}
void pmm_free_frame(void *physical){CHECK((uintptr_t)physical>=0x200000&&(uintptr_t)physical<0x300000);}
bool physical_pointer_range(uint64_t physical,size_t size,physical_kind_t domain,void **out)
{CHECK(domain==PHYSICAL_RAM&&physical>=0x200000&&size<=0x300000-physical);*out=ram+physical-0x200000;return true;}
void console_write_debug(const char *s){(void)s;}
void console_print_dec_debug(uint64_t n){(void)n;}
__attribute__((noreturn)) void kpanic(const char *s){fprintf(stderr,"unexpected panic: %s\n",s);abort();}
void timer_sleep(uint64_t ms)
{
 CHECK(enabled&&ms==1);__atomic_add_fetch(&waits,1,__ATOMIC_RELAXED);
 if(mode==1) {
  unsigned concurrent=__atomic_load_n(&active,__ATOMIC_ACQUIRE);
  if(concurrent==2)__atomic_store_n(&simultaneous,1,__ATOMIC_RELEASE);
  irq_flags_t flags=spin_lock_irqsave(&g_ep0_lock);
  bool other=g_ep0_owner.phase!=USB_EP0_IDLE && g_ep0_owner.slot!=caller_slot;
  spin_unlock_irqrestore(&g_ep0_lock,flags);
  if(other)__atomic_store_n(&contended,1,__ATOMIC_RELEASE);
  /* Deliver no completion until the other real caller has retried admission. */
  unsigned spins=0;
  while(__atomic_load_n(&entered,__ATOMIC_ACQUIRE)<2 ||
        !__atomic_load_n(&contended,__ATOMIC_ACQUIRE)) {
   assert(++spins<10000000U);sched_yield();
  }
 }
 sched_yield();
}
void usb_host_control_events(void)
{
 irq_flags_t flags=spin_lock_irqsave(&g_ep0_lock);
 if(g_ep0_owner.phase==USB_EP0_ACTIVE && mode!=2) {
  usb_ep0_owner_t *o=&g_ep0_owner;
  uint64_t td=o->trb[o->count-1];uint8_t slot=o->slot;
  CHECK(!usb_ep0_event(o,slot,2,o->lifecycle,td,false,1));
  CHECK(!usb_ep0_event(o,slot==1?2:1,1,o->lifecycle,td,false,1));
  CHECK(!usb_ep0_event(o,slot,1,o->lifecycle+1,td,false,1));
  CHECK(!usb_ep0_event(o,slot,1,o->lifecycle,td+16,false,1));
  if(last_td && last_td!=td)CHECK(!usb_ep0_event(o,slot,1,o->lifecycle,last_td,false,1));
  CHECK(usb_ep0_event(o,slot,1,o->lifecycle,td,false,1));
  CHECK(o->phase==USB_EP0_OBSERVED&&!usb_ep0_retire(o));
  CHECK(!usb_ep0_event(o,slot,1,o->lifecycle,td,false,1));
  /* Substituted event consumer's ERDP retirement boundary. */
  usb_ep0_event_retired(o);CHECK(o->phase==USB_EP0_COMPLETE);
  last_td=td;++events;
 }
 spin_unlock_irqrestore(&g_ep0_lock,flags);
}
static void *caller(void *argument)
{
 unsigned slot=(uintptr_t)argument;
 caller_slot=slot;
 usb_setup_packet_t setup={.RequestType=0x80,.Request=6,.Length=8};
 void *data=xhci_transfer_allocate(8);CHECK(data);
 __atomic_add_fetch(&active,1,__ATOMIC_RELEASE);__atomic_add_fetch(&entered,1,__ATOMIC_RELEASE);
 CHECK(xhci_control_transfer(slot,&setup,data));CHECK(xhci_transfer_release(data));
 __atomic_sub_fetch(&active,1,__ATOMIC_RELEASE);return NULL;
}
int main(void)
{
 CHECK(xhci_dma_initialize(1));xhci_driver.db_regs=bells;
 for(unsigned slot=1;slot<=2;++slot) {
  xhci_driver.slot_ep0_rings[slot]=xhci_dma_allocate(4096,4096);CHECK(xhci_driver.slot_ep0_rings[slot]);
  xhci_driver.slot_lifecycle[slot]=slot;xhci_driver.slot_ep0_cycle[slot]=1;
 }
 usb_setup_packet_t setup={.RequestType=0x80,.Request=6,.Length=8};unsigned char unowned[8]={0};
 CHECK(!xhci_control_transfer(1,&setup,unowned));CHECK(!xhci_driver.slot_ep0_enqueue[1]&&!bells[1].Target);
 CHECK(!xhci_control_transfer(0,&setup,unowned));CHECK(!xhci_control_transfer(64,&setup,unowned));
 enabled=false;CHECK(!xhci_control_transfer(1,&setup,unowned));enabled=true;
 mode=1;pthread_t a,b;CHECK(!pthread_create(&a,NULL,caller,(void *)1));CHECK(!pthread_create(&b,NULL,caller,(void *)2));
 CHECK(!pthread_join(a,NULL));CHECK(!pthread_join(b,NULL));CHECK(simultaneous&&contended&&events==2);
 mode=0;setup.Length=0;
 for(unsigned i=0;i<300;++i)CHECK(xhci_control_transfer(1,&setup,NULL));
 CHECK(events==302&&g_ep0_owner.phase==USB_EP0_IDLE&&xhci_driver.slot_ep0_enqueue[1]==(3+600)%255);
 void *data=xhci_transfer_allocate(8);CHECK(data);setup.Length=8;mode=2;
 unsigned old_waits=waits;CHECK(!xhci_control_transfer(1,&setup,data));CHECK(waits-old_waits==3000);
 CHECK(g_ep0_owner.phase==USB_EP0_QUARANTINED&&!xhci_transfer_release(data));
 uint16_t old_index=xhci_driver.slot_ep0_enqueue[2];old_waits=waits;
 CHECK(!xhci_control_transfer(2,&setup,data));CHECK(old_index==xhci_driver.slot_ep0_enqueue[2]&&waits==old_waits);
 CHECK(!usb_ep0_event(&g_ep0_owner,1,1,1,g_ep0_owner.trb[2],false,1));
 CHECK(!xhci_transfer_allocate(8));
 printf("USB_CONTROL_HOST checks=%u terminal_events=%u concurrent_callers=2 contention_observed=%u wraps=2 retained_timeout=1 cpu_bus_separate=1 status=PASS\n",checks,events,contended);
}
