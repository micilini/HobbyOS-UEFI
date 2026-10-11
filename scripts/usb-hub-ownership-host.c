/* Exercise the production parent/route/translator and child publication paths. */
#include <assert.h>
#include <stdio.h>
#include "../kernel/src/drivers/usb/xhci/usb_hub.c"
static unsigned checks;
static uint64_t root_generation=19;
static bool attached=true;
#define C(x) do{++checks;assert(x);}while(0)
irq_flags_t spin_lock_irqsave(spinlock_t *lock){C(!lock->locked);lock->locked=1;return 512;}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t flags){C(lock->locked&&flags==512);lock->locked=0;}
bool usb_hotplug_root_current(uint8_t port,uint64_t generation)
{return attached&&port==16&&generation==root_generation;}
int main(void)
{
 usb_hub_info_t *parent=&usb_hub_list[0];
 *parent=(usb_hub_info_t){.valid=1,.slot_id=1,.num_ports=15,.root_port=16,
   .root_generation=19,.slot_generation=7,.speed=USB_SPEED_FULL};
 usb_device_context_t out={.slot_id=0xa5};
 C(usb_hub_build_device_context(1,15,USB_SPEED_FULL,&out));
 C(out.root_port==16&&out.parent_hub_slot==1&&out.port_on_parent==15&&out.route_string==15);
 C(!out.tt_hub_slot_id&&!out.tt_port_num&&out.root_generation==19&&out.parent_generation==7);
 C(!publish_child(0,&out,2));g_usb_hub_probing[0]=1;
 C(publish_child(0,&out,2));C(parent->slot_id==1&&parent->child_slot[15]==2);
 C(!publish_child(0,&out,3)&&parent->child_slot[15]==2);
 parent->child_slot[15]=0;attached=false;
 C(!publish_child(0,&out,2)&&!parent->child_slot[15]);
 C(!usb_hub_build_device_context(1,1,USB_SPEED_FULL,&out));attached=true;
 ++root_generation;C(!usb_hub_context_current(&out));--root_generation;
 ++parent->slot_generation;C(!usb_hub_context_current(&out));--parent->slot_generation;
 parent->speed=USB_SPEED_HIGH;
 C(usb_hub_build_device_context(1,3,USB_SPEED_LOW,&out));
 C(out.tt_hub_slot_id==1&&out.tt_port_num==3);
 C(usb_hub_build_device_context(1,3,USB_SPEED_HIGH,&out));C(!out.tt_hub_slot_id&&!out.tt_port_num);
 parent->speed=USB_SPEED_FULL;parent->route_string=3;parent->hub_depth=1;
 parent->upstream_tt_slot=9;parent->upstream_tt_port=4;
 C(usb_hub_build_device_context(1,2,USB_SPEED_LOW,&out));
 C(out.route_string==0x23&&out.hub_depth==2&&out.tt_hub_slot_id==9&&out.tt_port_num==4);
 C(!usb_hub_build_device_context(0,1,USB_SPEED_FULL,&out));
 C(!usb_hub_build_device_context(1,0,USB_SPEED_FULL,&out));
 C(!usb_hub_build_device_context(1,16,USB_SPEED_FULL,&out));
 C(!usb_hub_build_device_context(1,1,USB_SPEED_INVALID,&out));
 C(!usb_hub_build_device_context(1,1,USB_SPEED_FULL,NULL));
 parent->hub_depth=USB_HUB_MAX_DEPTH;C(!usb_hub_build_device_context(1,1,USB_SPEED_FULL,&out));
 C(usb_hub_calc_route_string(0x4321,5)==0x54321);
 C(usb_hub_calc_route_string(0x54321,6)==UINT32_MAX);
 C(usb_hub_calc_route_string(0x301,2)==UINT32_MAX);
 C(usb_hub_calc_route_string(0x100000,1)==UINT32_MAX);
 C(usb_hub_calc_route_string(0,0)==UINT32_MAX);
 C(usb_hub_calc_route_string(0,16)==UINT32_MAX);
 printf("USB_HUB_OWNERSHIP_HOST checks=%u status=PASS\n",checks);
}
