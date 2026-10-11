/* Exercise the actual root event and callback paths with host scheduling. */
#include <assert.h>
#include <stdio.h>
#include "../kernel/src/drivers/usb/xhci/usb_hotplug.c"
static unsigned checks, disables, configurations, scheduled;
static bool fail_schedule, fail_disable;
static void *last_cookie;
static xhci_port_regs_t ports[16];
xhci_controller_t xhci_driver;
#define C(x) do{++checks;assert(x);}while(0)
void spinlock_init(spinlock_t *p){p->locked=0;}
irq_flags_t spin_lock_irqsave(spinlock_t *p){C(!p->locked);p->locked=1;return 512;}
void spin_unlock_irqrestore(spinlock_t *p,irq_flags_t f){C(p->locked&&f==512);p->locked=0;}
uint64_t clock_monotonic_ns(void){return 1;}
int timers_add(uint64_t ms,timer_callback_t cb,void *ctx)
{(void)ms;(void)cb;C(!g_hp_lock.locked);if(fail_schedule)return -1;++scheduled;last_cookie=ctx;return 0;}
int dpc_enqueue(dpc_callback_t cb,void *ctx)
{(void)cb;C(!g_hp_lock.locked);if(fail_schedule)return -1;++scheduled;last_cookie=ctx;return 0;}
void console_write_debug(const char *s){(void)s;}
bool xhci_disable_slot(uint8_t slot){C(slot&& !g_hp_lock.locked);++disables;return !fail_disable;}
int xhci_configure_owned_root(int port,int speed,uint64_t generation)
{(void)speed;C(!g_hp_lock.locked);++configurations;return usb_hotplug_root_address(port+1,generation,1)?1:0;}
void usb_hub_invalidate_by_slot(uint8_t slot){C(slot&&!g_hp_lock.locked);}
int usb_hub_get_count(void){return 0;}
void usb_hub_poll_all(void){}
int main(void)
{
 xhci_driver.max_ports=16;xhci_driver.port_regs=ports;usb_hotplug_init();
 uint64_t generation;C(usb_hotplug_begin_root(1,&generation));
 unsigned before=scheduled;usb_hotplug_handle_root_port_status(1,3);C(scheduled==before);
 C(usb_hotplug_root_address(1,generation,1));C(!usb_hotplug_root_address(1,generation,2));
 C(usb_hotplug_finish_root(1,generation,true));
 for(unsigned i=0;i<100;++i)usb_hotplug_handle_root_port_status(1,3|(1u<<21));
 C(scheduled==before&&!disables&&!configurations&&g_port_ctx[1].owner.slot==1);
 usb_hotplug_handle_root_port_status(1,0);C(!usb_hotplug_root_current(1,generation));
 ports[0].PortSC=3;usb_hotplug_handle_root_port_status(1,3);void *old=last_cookie;
 hp_timer_callback(old);C(last_cookie!=old);void *work=last_cookie;
 hp_timer_callback(old);C(last_cookie==work);hp_fsm_run(work);C(disables==1);
 old=last_cookie;usb_hotplug_handle_root_port_status(1,0);usb_hotplug_handle_root_port_status(1,3);
 work=last_cookie;before=scheduled;hp_timer_callback(old);C(scheduled==before&&last_cookie==work);
 fail_schedule=true;hp_timer_callback(work);C(g_port_ctx[1].owner.phase==USB_ROOT_RETAINED);
 C(!g_port_ctx[1].readiness_active&&g_hp_active_enumerations==0);
 fail_schedule=false;ports[15].PortSC=3;usb_hotplug_handle_root_port_status(16,3);
 C(g_port_ctx[16].readiness_active);hp_timer_callback(last_cookie);hp_fsm_run(last_cookie);
 ports[15].PortSC=3;hp_timer_callback(last_cookie);hp_fsm_run(last_cookie);hp_fsm_run(last_cookie);
 C(configurations==1&&g_port_ctx[16].owner.phase==USB_ROOT_CONFIGURED);
 before=scheduled;usb_hotplug_handle_root_port_status(0,3);usb_hotplug_handle_root_port_status(17,3);C(scheduled==before);
 usb_hotplug_handle_root_port_status(16,0);usb_hotplug_handle_root_port_status(16,3);
 fail_disable=true;hp_timer_callback(last_cookie);hp_fsm_run(last_cookie);
 C(g_port_ctx[16].owner.phase==USB_ROOT_RETAINED&&g_port_ctx[16].retired_slot==1);
 C(!g_port_ctx[16].readiness_active&&g_hp_active_enumerations==0);
 unsigned configured=configurations;before=scheduled;
 usb_hotplug_handle_root_port_status(16,3);C(scheduled==before&&configurations==configured);
 void *poll=(void *)(uintptr_t)g_poll_ticket;C(poll&&!g_poll_failed);
 hp_hub_poll_timer_cb(poll);work=last_cookie;before=scheduled;
 hp_hub_poll_timer_cb(poll);C(scheduled==before&&last_cookie==work);
 hp_hub_poll_dpc(work);C(g_poll_completions==1);poll=last_cookie;before=scheduled;
 hp_hub_poll_dpc(work);C(g_poll_completions==1&&scheduled==before&&last_cookie==poll);
 fail_schedule=true;hp_hub_poll_timer_cb(poll);
 C(g_poll_failed&&!g_poll_ticket);before=scheduled;hp_hub_poll_dpc(work);C(scheduled==before);
 usb_hotplug_readiness_snapshot_t readiness;C(usb_hotplug_readiness_snapshot(&readiness));
 C(readiness.failed&&readiness.active_enumerations==0&&readiness.poll_completions==1);
 printf("USB_HOTPLUG_HOST checks=%u status=PASS\n",checks);
}
