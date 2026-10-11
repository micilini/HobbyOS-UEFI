#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/drivers/usb/xhci/usb_ownership.h"
static unsigned checks;
#define C(x) do {++checks; assert(x);} while(0)
int main(void)
{
    usb_root_owner_t root = {0}; uint64_t gen = 0, unchanged = 77;
    C(usb_root_claim(&root, &gen) && gen == 1);
    C(!usb_root_claim(&root, &unchanged) && unchanged == 77);
    C(usb_root_address(&root, gen, 1));
    C(!usb_root_address(&root, gen, 2) && root.slot == 1);
    C(usb_root_finish(&root, gen, true));
    for (unsigned i=0;i<256;++i) C(!usb_root_claim(&root,&unchanged) && root.slot==1);
    C(usb_root_detach(&root)==1 && !usb_root_matches(&root,gen));
    uint64_t next; C(usb_root_claim(&root,&next) && next>gen);
    C(!usb_root_address(&root,gen,2)); C(!usb_root_finish(&root,gen,true));
    C(usb_root_address(&root,next,2)); C(usb_root_finish(&root,next,true));
    C(root.slot==2); C(usb_root_detach(&root)==2);
    root.generation=UINT64_MAX;C(!usb_root_claim(&root,&unchanged));
    for (unsigned slot=1;slot<64;++slot) {
        usb_ep0_owner_t lane={0}; uint64_t trb[]={0x123456780,0x123456790,0x1234567a0};
        C(usb_ep0_claim(&lane,slot,1)); C(!usb_ep0_claim(&lane,slot,1));
        C(usb_ep0_publish(&lane,trb,3));
        C(!usb_ep0_event(&lane,slot,1,2,trb[2],false,1));
        C(!usb_ep0_event(&lane,slot,2,1,trb[2],false,1));
        C(!usb_ep0_event(&lane,(slot%63)+1,1,1,trb[2],false,1));
        C(!usb_ep0_event(&lane,slot,1,1,trb[2],true,1));
        C(!usb_ep0_event(&lane,slot,1,1,trb[0],false,1));
        C(!usb_ep0_event(&lane,slot,1,1,trb[1],false,13));
        C(!usb_ep0_event(&lane,slot,1,1,trb[2]+16,false,1));
        C(usb_ep0_event(&lane,slot,1,1,trb[2],false,1));
        C(!usb_ep0_event(&lane,slot,1,1,trb[2],false,1));
        C(!usb_ep0_retire(&lane));
        usb_ep0_event_retired(&lane);
        C(usb_ep0_retire(&lane)); C(!usb_ep0_retire(&lane));
        C(usb_ep0_claim(&lane,slot,2)); trb[0]+=4096;trb[1]+=4096;trb[2]+=4096;
        C(usb_ep0_publish(&lane,trb,3));
        C(!usb_ep0_event(&lane,slot,1,2,trb[2]-4096,false,1));
        usb_ep0_timeout(&lane);C(lane.phase==USB_EP0_QUARANTINED);
        C(!usb_ep0_event(&lane,slot,1,2,trb[2],false,1));
        C(!usb_ep0_retire(&lane));C(!usb_ep0_claim(&lane,slot,3));
    }
    for (unsigned speed=1;speed<=5;++speed) for (unsigned parent=1;parent<=5;++parent) {
        uint8_t slot=55, port=55;
        C(usb_translator(speed,parent,2,3,0,0,&slot,&port));
        bool tt=(speed==1||speed==2)&&parent==3;
        C(slot==(tt?2:0)&&port==(tt?3:0));
        C(usb_translator(speed,parent,2,3,4,5,&slot,&port));
        bool inherited=(speed==1||speed==2)&&(parent==1||parent==2);
        C(slot==(tt?2:inherited?4:0)&&port==(tt?3:inherited?5:0));
    }
    uint8_t slot=55,port=55;C(!usb_translator(1,1,0,1,0,0,&slot,&port)&&slot==55&&port==55);
    C(!usb_translator(1,1,2,0,0,0,&slot,&port));
    printf("USB_OWNERSHIP_HOST checks=%u status=PASS\n",checks);
}
