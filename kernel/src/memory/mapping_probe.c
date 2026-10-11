#include "mapping_probe.h"
#include "paging_private.h"
#include "heap.h"
#include "paging_inspect.h"
#include "boot_address.h"
#include "physical_translation.h"
#include "../core/scheduler.h"
#include "../core/clock.h"
#include "../core/io.h"
#include "../core/spinlock.h"
#include "../shell/commands/cmd_tasktest.h"
#include "../drivers/usb/xhci/xhci.h"
#include "../drivers/usb/xhci/usb_hotplug.h"
#include "../graphics/graphics.h"
#include "../graphics/console.h"
#include "../drivers/serial.h"
#include "../libc/string.h"
#include <stdarg.h>

extern uint64_t mapping_probe_read(const void *address);
extern char mapping_probe_fault[], mapping_probe_recovery[];
static mapping_probe_state_t g_probe;
static uint32_t g_fixture_owner;
static uint64_t g_fixture_generation;
static bool g_fixture_retained;
static uint64_t g_owned_allocated, g_owned_released;
static unsigned g_owned_buffers, g_owned_views;

static bool record(const char *format, ...) __attribute__((format(printf,1,2)));
static bool record(const char *format, ...)
{
    char line[768]; va_list args; va_start(args,format);
    int n=kvsnprintf(line,sizeof(line),format,args);va_end(args);
    if(n<1 || (size_t)n>=sizeof(line))return false;
    serial_write_all(line);return true;
}
static uint64_t g_trace_bytes,g_trace_lines,g_trace_hash;
static bool trace(const char *format,...) __attribute__((format(printf,1,2)));
static bool trace(const char *format,...)
{
    char line[768];va_list args;va_start(args,format);
    int n=kvsnprintf(line,sizeof(line),format,args);va_end(args);
    if(n<1 || (size_t)n>=sizeof(line)-32 || line[n-1]!='\n')return false;
    uint32_t crc=tasktest_transport_crc32(line,n-1);
    int tail=ksnprintf(line+n-1,sizeof(line)-(size_t)n+1," crc=%08x\n",crc);
    if(tail<0 || (size_t)tail>=sizeof(line)-(size_t)n+1)return false;
    irq_flags_t saved=irq_save();
    for(const unsigned char *p=(const unsigned char *)line;*p;++p) {
        outb(0x402,*p);g_trace_hash=(g_trace_hash^*p)*UINT64_C(1099511628211);++g_trace_bytes;
    }
    ++g_trace_lines;irq_restore(saved);return true;
}
static bool usb_inventory(void)
{
    usb_hotplug_readiness_snapshot_t ready;
    if(!usb_hotplug_readiness_snapshot(&ready) || ready.failed || ready.active_enumerations)return false;
    bool ok=trace("[MAPPING][USB] capabilities=0x%x maximum=0x%llx polls=%llu active=%u failed=%u\n",
        xhci_driver.dma_capabilities,(unsigned long long)xhci_driver.dma_maximum,
        (unsigned long long)ready.poll_completions,ready.active_enumerations,ready.failed);
    for(unsigned port=1;ok && port<=xhci_driver.max_ports;++port) {
        usb_root_owner_t owner;
        if(!usb_hotplug_owner_snapshot(port,&owner))return false;
        if(owner.phase==USB_ROOT_DETACHED)continue;
        ok=trace("[MAPPING][ROOT] port=%u slot=%u generation=%llu phase=%u\n",port,owner.slot,
            (unsigned long long)owner.generation,owner.phase);
        if(owner.phase!=USB_ROOT_CONFIGURED)return false;
    }
    for(unsigned index=0;ok && index<USB_HUB_MAX_HUBS;++index) {
        usb_hub_info_t hub;
        if(!usb_hub_snapshot(index,&hub))continue;
        ok=trace("[MAPPING][HUB] index=%u slot=%u root=%u generation=%llu lifecycle=%llu speed=%u route=0x%x ports=%u tt_slot=%u tt_port=%u\n",
            index,hub.slot_id,hub.root_port,(unsigned long long)hub.root_generation,
            (unsigned long long)hub.slot_generation,hub.speed,hub.route_string,hub.num_ports,
            hub.upstream_tt_slot,hub.upstream_tt_port);
        for(unsigned port=1;ok && port<=hub.num_ports;++port)if(hub.child_slot[port]) {
            unsigned slot=hub.child_slot[port];
            ok=trace("[MAPPING][CHILD] parent=%u port=%u slot=%u speed=%u route=0x%x lifecycle=%llu tt_slot=%u tt_port=%u retiring=%u\n",
                hub.slot_id,port,slot,xhci_driver.slot_speed[slot],usb_hub_calc_route_string(hub.route_string,port),
                (unsigned long long)xhci_driver.slot_lifecycle[slot],xhci_driver.slot_tt_hub[slot],
                xhci_driver.slot_tt_port[slot],xhci_driver.slot_retiring[slot]);
        }
    }
    return ok;
}

static bool inventory(void)
{
    unsigned views=0,dmas=0;uint64_t pages_total=0;
    bool ok=trace("[MAPPING][BEGIN] generation=%llu root=0x%llx window=0x%llx window_bytes=%llu tables=514\n",
        (unsigned long long)g_fixture_generation,(unsigned long long)paging_kernel_root_physical(),
        (unsigned long long)DEVICE_WINDOW_BASE,(unsigned long long)DEVICE_WINDOW_BYTES);
    for(unsigned slot=0;ok && slot<OWNED_MAPPING_CAPACITY;++slot) {
        mapping_view_sample_t sample;
        if(!owned_mapping_sample(slot,&sample))continue;
        const mapping_view_t *v=&sample.view;
        uint64_t offset=v->physical&4095,pages=(offset+v->bytes+4095)/4096;
        uint64_t first=(uintptr_t)v->cpu-offset, physical=v->physical-offset;
        if(sample.state!=2 || v->handle.slot!=slot)return false;
        ok=trace("[MAPPING][VIEW] slot=%u generation=%llu va=0x%llx physical=0x%llx bytes=%llu pages=%llu flags=0x%llx origin=%u source=0x%llx state=%u\n",
            slot,(unsigned long long)v->handle.generation,(unsigned long long)(uintptr_t)v->cpu,
            (unsigned long long)v->physical,(unsigned long long)v->bytes,(unsigned long long)pages,
            (unsigned long long)sample.flags,sample.origin,(unsigned long long)sample.source,sample.state);
        for(uint64_t at=0;ok && at<pages;at+=8) {
            char entries[320];size_t used=0;unsigned count=pages-at<8?pages-at:8;
            for(unsigned i=0;i<count;++i) {
                uint64_t leaf,identity;
                if(!paging_owned_leaf_pair(first+(at+i)*4096,physical+(at+i)*4096,&leaf,&identity) ||
                   (leaf&~UINT64_C(0x60))!=((physical+(at+i)*4096)|sample.flags) ||
                   (sample.origin && identity) || (!sample.origin && identity &&
                    ((identity&UINT64_C(0x98)) || (identity&PAGE_ADDR_MASK)!=physical+(at+i)*4096)))return false;
                int n=ksnprintf(entries+used,sizeof(entries)-used,"%s%llx:%llx",i?",":"",
                    (unsigned long long)leaf,(unsigned long long)identity);
                if(n<0 || (size_t)n>=sizeof(entries)-used)return false;
                used+=n;
            }
            ok=trace("[MAPPING][LEAVES] slot=%u first=%llu count=%u entries=%s\n",slot,(unsigned long long)at,count,entries);
        }
        ++views;pages_total+=pages;
    }
    for(unsigned slot=0;ok && slot<2048;++slot) {
        dma_buffer_t buffer;dma_lifetime_t state;
        if(!dma_buffer_sample(slot,&buffer,&state))continue;
        uint64_t physical;
        if(!buffer.backing_bytes || buffer.device_address>buffer.maximum_address ||
           buffer.backing_bytes-1>buffer.maximum_address-buffer.device_address ||
           buffer.device_address>=physical_allocation_ceiling() ||
           buffer.backing_bytes>physical_allocation_ceiling()-buffer.device_address ||
           !physical_address_range(buffer.cpu,buffer.backing_bytes,&physical) || physical!=buffer.device_address ||
           state==DMA_UNCERTAIN)return false;
        ok=trace("[MAPPING][DMA] slot=%u generation=%llu cpu=0x%llx device=0x%llx requested=%llu backing=%llu alignment=%llu maximum=0x%llx ceiling=0x%llx state=%u\n",
            slot,(unsigned long long)buffer.handle.generation,(unsigned long long)(uintptr_t)buffer.cpu,
            (unsigned long long)buffer.device_address,(unsigned long long)buffer.requested_bytes,
            (unsigned long long)buffer.backing_bytes,(unsigned long long)buffer.alignment,
            (unsigned long long)buffer.maximum_address,(unsigned long long)physical_allocation_ceiling(),state);++dmas;
    }
    return ok && usb_inventory() && trace("[MAPPING][INVENTORY] generation=%llu views=%u pages=%llu dma_objects=%u\n",
        (unsigned long long)g_fixture_generation,views,(unsigned long long)pages_total,dmas);
}

bool mapping_probe_fixup(InterruptFrame *frame, uint64_t address, uint64_t error)
{
    cpu_slot_t slot;
    if(!frame || !smp_current_cpu_slot(&slot) ||
       !mapping_probe_match(&g_probe,frame->rip,address,error,frame->cs,slot,g_fixture_generation)) return false;
    g_probe.seen=true;g_probe.armed=false;frame->rip=g_probe.recovery;return true;
}
static bool unmap_cycle(unsigned cycle)
{
    dma_buffer_t buffer;
    mapping_view_t view;
    size_t bytes=(cycle%4+1)*4096,alignment=64u<<cycle;
    if(dma_buffer_allocate(bytes,alignment,UINT32_MAX,&buffer)!=MAPPING_OK)return false;
    g_owned_allocated+=buffer.backing_bytes;++g_owned_buffers;
    size_t zeroed=0;
    for(size_t i=0;i<buffer.backing_bytes;++i)zeroed+=((volatile uint8_t *)buffer.cpu)[i]==0;
    if(zeroed!=buffer.backing_bytes)return false;
    uint64_t marker=UINT64_C(0x7341520000000000)|cycle;
    *(volatile uint64_t *)buffer.cpu=marker;
    if(owned_mapping_test_buffer(&buffer,&view)!=MAPPING_OK) {
        if(dma_buffer_release(buffer.handle)==MAPPING_OK) {
            g_owned_released+=buffer.backing_bytes;--g_owned_buffers;
        }
        return false;
    }
    ++g_owned_views;
    uint64_t before=mapping_probe_read(view.cpu),actual=0;
    if(before!=marker || !paging_query_range((uintptr_t)view.cpu,8,&actual) || actual!=buffer.device_address)return false;
    mapping_status_t removed=owned_mapping_release(view.handle);
    if(removed!=MAPPING_OK || paging_query_range((uintptr_t)view.cpu,8,&actual))return false;
    scheduler_cpu_pin_t pin={0};
    if(!scheduler_cpu_pin(&pin))return false;
    g_probe=(mapping_probe_state_t){.address=(uintptr_t)view.cpu,.generation=g_fixture_generation,
        .site=(uintptr_t)mapping_probe_fault,.recovery=(uintptr_t)mapping_probe_recovery,.slot=pin.slot,.armed=true};
    __asm__ volatile("" ::: "memory");
    uint64_t after=mapping_probe_read(view.cpu);
    __asm__ volatile("" ::: "memory");
    bool recovered=g_probe.seen && !g_probe.armed && after==0;
    cpu_slot_t probe_slot=pin.slot;
    g_probe.armed=false;
    scheduler_cpu_unpin(&pin);
    bool finished=recovered && owned_mapping_test_finish(view.handle);
    if(finished)--g_owned_views;
    bool freed=finished && dma_buffer_release(buffer.handle)==MAPPING_OK;
    if(freed){g_owned_released+=buffer.backing_bytes;--g_owned_buffers;}
    bool stale=finished && owned_mapping_release(view.handle)==MAPPING_STALE;
    bool dma_stale=freed && dma_buffer_release(buffer.handle)==MAPPING_STALE;
    return record("\n[MAPPING][UNMAP] generation=%llu cycle=%u va=0x%llx physical=0x%llx before=0x%llx marker=0x%llx removed=%u cpu=%u recovered=%u after=0x%llx error=0 site=0x%llx recovery=0x%llx freed=%u stale=%u backing=%llu alignment=%llu maximum=0xffffffff zeroed=%llu dma_stale=%u dma_slot=%u dma_generation=%llu view_slot=%u view_generation=%llu\n",
        (unsigned long long)g_fixture_generation,cycle,(unsigned long long)(uintptr_t)view.cpu,
        (unsigned long long)buffer.device_address,(unsigned long long)before,(unsigned long long)marker,
        (unsigned)removed,probe_slot,recovered,(unsigned long long)after,(unsigned long long)(uintptr_t)mapping_probe_fault,
        (unsigned long long)(uintptr_t)mapping_probe_recovery,freed,stale,(unsigned long long)buffer.backing_bytes,
        (unsigned long long)buffer.alignment,(unsigned long long)zeroed,dma_stale,buffer.handle.slot,
        (unsigned long long)buffer.handle.generation,view.handle.slot,
        (unsigned long long)view.handle.generation)&&freed&&stale&&dma_stale;
}
static bool framebuffer_check(void)
{
    const boot_physical_objects_t *boot=physical_boot_objects();Framebuffer *fb=graphics_framebuffer();
    if(!boot || !fb || fb->BufferSize!=boot->framebuffer_bytes)return false;
    uint64_t first=(uintptr_t)fb->BaseAddress&~UINT64_C(4095),physical=boot->framebuffer&~UINT64_C(4095);
    uint64_t pages=((boot->framebuffer&4095)+boot->framebuffer_bytes+4095)/4096;
    uint64_t digest=UINT64_C(14695981039346656037);
#ifdef HOBBYOS_FRAMEBUFFER_REFERENCE
    const unsigned expected_slot=4, expected_type=6;
#else
    const unsigned expected_slot=5, expected_type=1;
#endif
    for(uint64_t i=0;i<pages;++i) {
        cpu_control_sample_t cpu;pt_result_t result,identity;
        paging_inspect(first+i*4096,&cpu,&result);
        if(result.status!=PT_MAPPED || result.physical!=physical+i*4096 ||
            result.pat_index!=expected_slot || result.pat_type!=expected_type)return false;
        paging_inspect(physical+i*4096,&cpu,&identity);
        if(identity.status!=PT_NOT_PRESENT)return false;
        digest=(digest^result.entries[result.count-1].raw)*UINT64_C(1099511628211);
    }
    return record("\n[MAPPING][FRAMEBUFFER] generation=%llu va=0x%llx physical=0x%llx bytes=%llu pages=%llu pat_slot=%u pat_type=%u identity_absent=%llu leaf_digest=0x%llx effective_type=NOT_COMPUTED\n",
        (unsigned long long)g_fixture_generation,(unsigned long long)(uintptr_t)fb->BaseAddress,
        (unsigned long long)boot->framebuffer,(unsigned long long)boot->framebuffer_bytes,
        (unsigned long long)pages,expected_slot,expected_type,(unsigned long long)pages,(unsigned long long)digest);
}
int mapping_fixture_run(void)
{
    uint32_t empty=0;
    if(!__atomic_compare_exchange_n(&g_fixture_owner,&empty,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    if(g_fixture_retained || g_fixture_generation==UINT64_MAX) {__atomic_store_n(&g_fixture_owner,0,__ATOMIC_RELEASE);return 1;}
    ++g_fixture_generation;uint64_t before=pmm_get_free_memory();
    g_owned_allocated=g_owned_released=0;g_owned_buffers=g_owned_views=0;
    g_trace_bytes=g_trace_lines=0;g_trace_hash=UINT64_C(14695981039346656037);
    bool ok=inventory() && framebuffer_check();
    for(unsigned cycle=0;ok && cycle<8;++cycle)ok=unmap_cycle(cycle);
    /* Hub polling and the reaper may allocate concurrently. Global free memory
     * remains an observation; only this fixture's generation-bound resources
     * determine whether its retirement completed. */
    uint64_t after=pmm_get_free_memory();
    ok=ok && g_owned_allocated==20*4096 && g_owned_released==g_owned_allocated &&
        !g_owned_buffers && !g_owned_views;
    if(!ok)g_fixture_retained=true;
    (void)record("\n[MAPPING][END] schema=2 generation=%llu status=%s cycles=8 free_before=%llu free_after=%llu retained=%u allocated_bytes=%llu released_bytes=%llu live_buffers=%u live_views=%u\n",
        (unsigned long long)g_fixture_generation,ok?"PASS":"FAIL",(unsigned long long)before,(unsigned long long)after,g_fixture_retained,
        (unsigned long long)g_owned_allocated,(unsigned long long)g_owned_released,g_owned_buffers,g_owned_views);
    ok=record("\n[MAPPING_TRACE][COMPLETE] generation=%llu channel=debugcon port=0x402 bytes=%llu records=%llu fnv64=0x%llx status=%s\n",
        (unsigned long long)g_fixture_generation,(unsigned long long)g_trace_bytes,
        (unsigned long long)g_trace_lines,(unsigned long long)g_trace_hash,ok?"PASS":"FAIL")&&ok;
    __atomic_store_n(&g_fixture_owner,0,__ATOMIC_RELEASE);return ok?0:1;
}
int framebuffer_copy_benchmark(void)
{
    Framebuffer *fb=graphics_framebuffer();void *back=get_draw_buffer();
    if(!fb || !fb->Height || !fb->PixelsPerScanLine || !clock_monotonic_is_ready())return 1;
    uint64_t bytes=(uint64_t)fb->Height*fb->PixelsPerScanLine*4;
    if(bytes>fb->BufferSize || bytes>16u*1024u*1024u)return 1;
    if(!back) {
        /* graphics_bind_framebuffer retains this CPU-only test buffer for the
         * boot. Repeated benchmarks reuse it; it is never freed underneath graphics. */
        back=kmalloc(bytes);
        if(!back || !graphics_bind_framebuffer(fb,back))return 1;
    }
    /* The real copy path and a completion fence are timed; validation is outside.
     * Console presentation is suspended while the benchmark owns the buffer. */
    uint8_t suspended=console_is_render_suspended();
    console_set_render_suspended(1);
    for(uint64_t i=0;i<bytes/4;++i)((uint32_t *)back)[i]=0x112244u^(uint32_t)i;
    graphics_enable_buffering(true);
    for(unsigned i=0;i<3;++i){swap_buffers();__asm__ volatile("sfence" ::: "memory");}
    uint64_t samples[8];bool ok=true;
    for(unsigned i=0;i<8;++i) {
        uint64_t start=clock_monotonic_ns();swap_buffers();__asm__ volatile("sfence" ::: "memory");
        uint64_t end=clock_monotonic_ns();samples[i]=end>=start?end-start:UINT64_MAX;ok &= samples[i]!=UINT64_MAX;
    }
    uint64_t hash=UINT64_C(14695981039346656037);
    for(uint64_t i=0;i<bytes/4;++i) {
        uint32_t value=((volatile uint32_t *)fb->BaseAddress)[i];
        if(value!=(0x112244u^(uint32_t)i))ok=false;
        hash=(hash^value)*UINT64_C(1099511628211);
    }
    graphics_enable_buffering(false);console_set_render_suspended(suspended);
#ifdef HOBBYOS_FRAMEBUFFER_REFERENCE
    const char *selection="WB_REFERENCE";
#else
    const char *selection="WC";
#endif
    for(unsigned i=0;i<8;++i)ok=record("\n[FRAMEBUFFER_COPY][SAMPLE] selection=%s warmup=3 samples=8 sample=%u bytes=%llu width=%u height=%u stride=%u ns=%llu checksum=0x%llx valid=%u timer=monotonic_hpet scope=swap_buffers_plus_sfence\n",
        selection,i,(unsigned long long)bytes,fb->Width,fb->Height,fb->PixelsPerScanLine,(unsigned long long)samples[i],(unsigned long long)hash,ok)&&ok;
    return ok?0:1;
}
