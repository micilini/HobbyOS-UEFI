/* Actual PMM and DMA ownership with substituted host lock/physical backing.
 * Numeric device addresses are deliberately unrelated to CPU buffer pointers. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../kernel/src/memory/pmem.c"
#include "../kernel/src/memory/dma_buffer.h"
#include "../kernel/src/drivers/usb/xhci/xhci_dma.h"

static uint8_t host_backing[4096 * 512];
static irq_flags_t host_flags;
static uint64_t ceiling;
static unsigned checks;
#define CHECK(x) do {++checks; assert(x);} while (0)
uint64_t physical_allocation_ceiling(void) { return ceiling; }
bool physical_pointer_range(uint64_t physical, size_t bytes, physical_kind_t kind, void **out)
{
    CHECK(kind == PHYSICAL_RAM && physical != (uintptr_t)host_backing);
    if (!out || physical >= sizeof(host_backing) || bytes > sizeof(host_backing)-physical) return false;
    *out = host_backing + physical; return true;
}
void serial_write_all(const char *line) { CHECK(line); }
int ksnprintf(char *out, size_t bytes, const char *format, ...)
{ va_list args; va_start(args, format); int n = vsnprintf(out, bytes, format, args); va_end(args); return n; }
void kpanic(const char *message) { fprintf(stderr, "Unexpected panic: %s\n", message); abort(); }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    CHECK(!lock->locked); lock->locked=1;
    irq_flags_t saved=host_flags; host_flags &= ~UINT64_C(512); return saved;
}
void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{ CHECK(lock->locked && !(host_flags & 512)); lock->locked=0; host_flags=flags; }
static void setup(size_t bits, size_t limit)
{
    g_bitmap = (Bitmap){malloc((bits+7)/8),(bits+7)/8,bits};
    g_reserved_bitmap = (Bitmap){calloc((bits+7)/8,1),(bits+7)/8,bits};
    CHECK(g_bitmap.buffer && g_reserved_bitmap.buffer);
    memset(g_bitmap.buffer,255,g_bitmap.size); memset(host_backing,0xa5,sizeof(host_backing));
    g_total_frames=bits;g_bitmap_size=g_bitmap.size;g_free_frames=0;
    g_min_alloc_frame=1;g_alloc_limit_frames=limit;ceiling=limit*4096;
}
static void available(size_t frame)
{ bitmap_set(&g_bitmap,frame,false); ++g_free_frames; }
static void done(void)
{ CHECK(pmm_validate_integrity()); free(g_bitmap.buffer);free(g_reserved_bitmap.buffer); }
static void constrained(void)
{
    static const size_t limits[]={0,1,7,8,9,63,64,65,127,128,129,511};
    for (unsigned enable=0;enable<2;++enable) for(unsigned n=0;n<sizeof(limits)/sizeof(limits[0]);++n) {
        size_t limit=limits[n];setup(513,limit);host_flags=0x45|(enable?512:0);
        available(limit);available(512);uint64_t result=0xfeed, before=g_free_frames;
        CHECK(!pmm_alloc_constrained_frames(1,1,limit*4096,&result));
        CHECK(result==0xfeed && g_free_frames==before && host_flags==(0x45|(enable?512:0)));
        if (limit>1) {
            available(limit-1);CHECK(pmm_alloc_constrained_frames(1,1,limit*4096,&result));
            CHECK(result==(limit-1)*4096);pmm_free_frame((void *)(uintptr_t)result);
        }
        done();
    }
    setup(513,129);for(unsigned i=3;i<20;++i)available(i);available(500);
    uint64_t result=0;
    CHECK(pmm_alloc_constrained_frames(4,8,16*4096,&result) && result==8*4096);
    CHECK(!pmm_alloc_constrained_frames(9,8,16*4096,&result) && result==8*4096);
    CHECK(!pmm_alloc_constrained_frames(1,3,16*4096,&result));done();
    setup(268435456,256);available(256);available(268435455);
    uint64_t before=g_free_frames;result=0xfeed;
    CHECK(!pmm_alloc_constrained_frames(1,1,UINT64_C(0x100000000),&result));
    CHECK(result==0xfeed && g_free_frames==before && !bitmap_get(&g_bitmap,268435455));
    CHECK(pmm_validate_integrity());bitmap_set(&g_reserved_bitmap,268435455,true);
    CHECK(!pmm_validate_integrity());bitmap_set(&g_reserved_bitmap,268435455,false);done();
    /* A verified RAM ceiling above four GiB does not raise a device's limit. */
    setup(268435456,0x140000);host_flags=0x245;
    for(unsigned i=0;i<3;++i)available(0x100000+i);
    available(268435455);before=g_free_frames;result=0xfeed;
    CHECK(!pmm_alloc_constrained_frames(3,1,UINT64_C(0x100000000),&result));
    CHECK(result==0xfeed && g_free_frames==before && host_flags==0x245);
    dma_buffer_t denied,sentinel;memset(&denied,0xa5,sizeof(denied));sentinel=denied;
    CHECK(dma_buffer_allocate(8192,4096,UINT32_MAX,&denied)==MAPPING_NO_MEMORY);
    CHECK(!memcmp(&denied,&sentinel,sizeof(denied)) && g_free_frames==before);
    CHECK(pmm_alloc_constrained_frames(3,1,UINT64_C(0x140000000),&result));
    CHECK(result==UINT64_C(0x100000000) && !bitmap_get(&g_bitmap,268435455));
    for(unsigned i=0;i<3;++i)pmm_free_frame((void *)(uintptr_t)(result+i*4096));
    CHECK(g_free_frames==before && host_flags==0x245);done();
}
static void descriptors(void)
{
    setup(513,512);for(unsigned i=1;i<512;++i)available(i);
    dma_buffer_t buffer, sentinel;memset(&buffer,0xa5,sizeof(buffer));sentinel=buffer;
    CHECK(dma_buffer_allocate(4097,4096,8191,&buffer)==MAPPING_NO_MEMORY);
    CHECK(!memcmp(&buffer,&sentinel,sizeof(buffer)));
    CHECK(dma_buffer_allocate(4097,16384,UINT32_MAX,&buffer)==MAPPING_OK);
    CHECK(buffer.device_address==16384 && buffer.backing_bytes==8192 && buffer.alignment==16384);
    CHECK(buffer.cpu==host_backing+16384 && (uintptr_t)buffer.cpu!=buffer.device_address);
    for(size_t i=0;i<buffer.backing_bytes;i++)CHECK(((uint8_t *)buffer.cpu)[i]==0);
    uint64_t address=0;dma_buffer_t copy;
    CHECK(dma_buffer_lookup((uint8_t *)buffer.cpu+4096,1,&copy,&address) && address==20480);
    CHECK(!dma_buffer_lookup((uint8_t *)buffer.cpu+4097,1,&copy,&address));
    CHECK(dma_buffer_address(&buffer,4096,1,&address) && address==20480);
    CHECK(!dma_buffer_address(&buffer,4096,2,&address));
    void *cpu=NULL;CHECK(dma_buffer_device_view(20480,1,UINT32_MAX,&cpu) && cpu==(uint8_t *)buffer.cpu+4096);
    CHECK(!dma_buffer_device_view(20480,1,UINT64_MAX,&cpu));
    CHECK(dma_buffer_state(buffer.handle,DMA_IN_FLIGHT));
    CHECK(dma_buffer_release(buffer.handle)==MAPPING_QUARANTINED);
    CHECK(dma_buffer_state(buffer.handle,DMA_COMPLETED));
    CHECK(dma_buffer_release(buffer.handle)==MAPPING_OK);
    CHECK(dma_buffer_release(buffer.handle)==MAPPING_STALE);
    CHECK(xhci_dma_capability_limit(0)==UINT32_MAX && xhci_dma_capability_limit(1)==UINT64_MAX);
    CHECK(xhci_dma_capability_limit(0xfffffffe)==UINT32_MAX);
    CHECK(xhci_dma_initialize(0));CHECK(!xhci_dma_initialize(1));
    uint8_t stack[8]={0};CHECK(!xhci_transfer_begin(stack,sizeof(stack),&copy,&address));
    void *transfer=xhci_transfer_allocate(64);CHECK(transfer);
    CHECK(xhci_transfer_begin(transfer,64,&copy,&address));
    CHECK(address==copy.device_address && address<=UINT32_MAX);
    CHECK(!xhci_transfer_begin(transfer,64,&copy,&address));
    xhci_transfer_finish(&copy,true);CHECK(xhci_transfer_release(transfer));
    transfer=xhci_transfer_allocate(64);CHECK(transfer);
    CHECK(xhci_transfer_begin(transfer,64,&copy,&address));
    xhci_transfer_finish(&copy,false);
    CHECK(!xhci_transfer_release(transfer));CHECK(!xhci_transfer_allocate(64));
    CHECK(dma_buffer_release(copy.handle)==MAPPING_QUARANTINED);
    done();
}
int main(void)
{
    constrained();descriptors();
    printf("DEVICE_DMA_HOST checks=%u logical_frames=268435456 privileged_operations=SUBSTITUTED status=PASS\n",checks);
    return 0;
}
