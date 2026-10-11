#include "tlb_fixture.h"
#include "tlb_proof.h"
#include "heap.h"
#include "owned_mapping.h"
#include "image_walk.h"
#include "kernel_tlb.h"
#include "kernel_stack.h"
#include "paging_private.h"
#include "physical_translation.h"
#include "../core/clock.h"
#include "../core/irq_latency.h"
#include "../core/idt.h"
#include "../core/io.h"
#include "../core/spinlock.h"
#include "../core/scheduler.h"
#include "../core/panic.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "../apic/lapic.h"
#include "../cpu/cpu_control_snapshot.h"
#include "../shell/commands/cmd_tasktest.h"
#include <stdarg.h>

typedef struct {
    cpu_request_result_t result;
    cpu_request_sample_t cpus[HOBBYOS_MAX_CPUS];
} fixture_operation_t;
typedef struct {
    uint64_t frame_a, frame_b;
    volatile uint64_t *pointer_a, *pointer_b;
    fixture_operation_t operations[4];
    uint32_t live, finished, failed, rounds;
    uint64_t contentions, started_ns, ended_ns;
} fixture_window_t;
static fixture_window_t g_windows[TLB_FIXTURE_WINDOWS];
static task_handle_t g_worker_tasks[TLB_FIXTURE_WINDOWS];
static stack_handle_t g_worker_stacks[TLB_FIXTURE_WINDOWS];
static uint32_t g_owner, g_readers[TLB_FIXTURE_WINDOWS], g_barrier, g_release;
static uint64_t g_invocation;
static bool g_quarantined;
static const unsigned g_pages[] = {1, 2, 63, 64, 65};
static spinlock_t g_trace_lock;
static uint64_t g_trace_bytes, g_trace_records, g_trace_digest;
static tlb_proof_t g_proof;
static bool g_proof_deferred;
static unsigned g_proof_invocations, g_proof_completed;
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
#define PROOF_METHOD "immutable-tlb-failure-proof-v1"
static unsigned g_proof_ended, g_workers_created;
static bool g_writers_retired;
static const char *g_failure_stage;
static struct { const char *stage; unsigned round, phase; } g_cycle_failure[TLB_FIXTURE_WINDOWS];
static bool fixture_check(bool ok, const char *stage)
{
    if (!ok && !g_failure_stage) g_failure_stage=stage;
    return ok;
}
typedef struct { uint64_t last; unsigned stalled; } fixture_wait_clock_t;
static bool fixture_wait_before(uint64_t deadline, fixture_wait_clock_t *state)
{
    uint64_t now;
    if (!clock_monotonic_read_ns(&now) || now<state->last || now>=deadline) return false;
    if (now==state->last) {
        if (++state->stalled>=1024u) return false;
    } else state->stalled=0;
    state->last=now;
    return true;
}
static bool cycle_failure(unsigned window, unsigned round, unsigned phase, const char *stage)
{
    g_cycle_failure[window].stage=stage;
    g_cycle_failure[window].round=round;
    g_cycle_failure[window].phase=phase;
    return false;
}
#define CYCLE_REQUIRE(condition, stage, phase) do { if (!(condition)) return cycle_failure(window,round,phase,stage); } while (0)
#else
#define PROOF_METHOD "immutable-tlb-proof-v2"
#define fixture_check(ok, stage) (ok)
#define CYCLE_REQUIRE(condition, stage, phase) do { if (!(condition)) return false; } while (0)
#endif
static uint64_t g_proof_rights_limit, g_proof_rights, g_proof_port_bytes;
#define TLB_TRACE_PORT 0x402u
#define TLB_TRACE_FNV_OFFSET UINT64_C(14695981039346656037)
#define TLB_TRACE_FNV_PRIME UINT64_C(1099511628211)

static bool emit_line(const char *line)
{
    if (strncmp(line+1,"[TLB]",5)) {
        serial_write_all(line);
        return true;
    }
    IRQ_LATENCY_SCOPE(IRQ_PATH_SHOOTDOWN);
    size_t bytes=strlen(line+1);
    irq_flags_t flags=spin_lock_irqsave(&g_trace_lock);
    if (g_proof.state!=TLB_PROOF_UNUSED && !tlb_proof_append(&g_proof,line+1,bytes)) {
        spin_unlock_irqrestore(&g_trace_lock,flags);
        return false;
    }
    for(const unsigned char *p=(const unsigned char *)line+1;*p;++p) {
        if (!g_proof_deferred) {
            outb(TLB_TRACE_PORT,*p);
            ++g_proof_port_bytes;
        }
        g_trace_digest=(g_trace_digest^*p)*TLB_TRACE_FNV_PRIME;
        ++g_trace_bytes;
    }
    ++g_trace_records;
    spin_unlock_irqrestore(&g_trace_lock,flags);
    return true;
}

static bool record(const char *format, ...) __attribute__((format(printf,1,2)));
static bool record(const char *format, ...)
{
    /* An unrelated legacy multipart message may have left the UART mid-line. */
    char line[768];line[0]='\n';va_list args;va_start(args,format);
    int n=kvsnprintf(line+1,sizeof(line)-1,format,args);va_end(args);
    if(n<1 || (size_t)n>=sizeof(line)-1 || line[n]!='\n') return false;
    if(!strncmp(format,"[TLB]",5)) {
        uint32_t crc=tasktest_transport_crc32(line+1,(uint32_t)n-1);
        int tail=ksnprintf(line+n,sizeof(line)-(size_t)n," crc=%08x\n",crc);
        if(tail<0 || (size_t)tail>=sizeof(line)-(size_t)n)return false;
    }
    return emit_line(line);
}

static bool proof_count_leaf(void *context, const image_leaf_run_t *leaf)
{
    (void)leaf;
    ++*(uint64_t *)context;
    return true;
}

bool tlb_fixture_proof_configured(void)
{ return g_proof.state!=TLB_PROOF_UNUSED; }

bool tlb_fixture_proof_prepare(bool deferred, uint64_t session, unsigned invocations)
{
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    if (!deferred) return false;
#endif
    if (!irq_is_enabled() || !cpu_request_context_allowed() || !session ||
        !invocations || invocations>3 || g_invocation || g_owner || g_proof.state!=TLB_PROOF_UNUSED ||
        !g_cpu_count || g_cpu_count>HOBBYOS_MAX_CPUS) return false;
    image_walk_result_t scan;
    uint64_t baseline_runs=0;
    if (!paging_image_snapshot(&scan,proof_count_leaf,&baseline_runs)) return false;
    /* Image/direct runs are fixed. Each contiguous stack/view adds at most
     * one run; allow two per view for identity withdrawals, and every scratch
     * page separately. Each scan enforces this admitted bound again. */
    uint64_t rights=baseline_runs+STACK_OWNER_COUNT+2u*OWNED_MAPPING_CAPACITY+
        TLB_FIXTURE_WINDOWS*TLB_FIXTURE_PAGES;
    uint64_t windows=g_cpu_count>1 ? 2 : 1;
    uint64_t cycles=TLB_FIXTURE_ROUNDS+1+(g_cpu_count>1 ? 2*TLB_FIXTURE_CONCURRENT_ROUNDS : 0);
    uint64_t records=invocations*(cycles*(5+8*g_cpu_count)+windows*(2+2*g_cpu_count)+
        (g_cpu_count>1 ? 3 : 2)*(rights+STACK_OWNER_COUNT+1)+12)+CPU_REQUEST_AUDIT_CAPACITY;
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    records+=3u*invocations; /* Outcome plus at most two failed initiators. */
#endif
    if (records>SIZE_MAX/TLB_PROOF_LINE_BYTES) return false;
    size_t capacity=(size_t)records*TLB_PROOF_LINE_BYTES;
    unsigned char *data=kmalloc(capacity);
    if (!data) return false;
    /* Touch support pages before activation in both comparison modes. */
    memset(data,0,capacity);
    if (!tlb_proof_open(&g_proof,data,capacity,session)) { kfree(data); return false; }
    g_proof_deferred=deferred;g_proof_invocations=invocations;g_proof_rights_limit=rights;
    char line[512];
    int n=ksnprintf(line,sizeof(line),"\n[TLB_PROOF][PREPARED] method=" PROOF_METHOD " session=0x%llx policy=%s cpus=%u invocations=%u capacity=%llu record_bound=%llu line_bound=%u baseline_runs=%llu rights_bound=%llu owned_until=VM_END\n",
        (unsigned long long)session,deferred?"deferred":"immediate",g_cpu_count,invocations,
        (unsigned long long)capacity,(unsigned long long)records,TLB_PROOF_LINE_BYTES,
        (unsigned long long)baseline_runs,(unsigned long long)rights);
    if (n<0 || (size_t)n>=sizeof(line)) {g_proof.failed=true;return false;}
    serial_write_all(line);return true;
}

static bool proof_export_line(const char *line)
{
    if (!irq_is_enabled() || !line || line[0]!='\n') return false;
    size_t bytes=0;
    while (bytes<383 && line[bytes+1]) ++bytes;
    if (!bytes || bytes==383 || line[bytes]!='\n') return false;
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    return serial_proof_write(g_proof.session,line+1,bytes);
#else
    for (size_t i=1;i<=bytes;++i) outb(TLB_TRACE_PORT,(unsigned char)line[i]);
    return true;
#endif
}

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
static bool proof_export_serial_body(uint64_t service_generation)
#else
bool tlb_fixture_proof_export(uint64_t service_generation)
#endif
{
    if (!irq_is_enabled() || !cpu_request_context_allowed()) return false;
    uint64_t admitted,completed;
    if (!cpu_request_audit_snapshot(&admitted,&completed) || admitted!=completed ||
        completed!=service_generation) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_trace_lock);
    bool idle=!__atomic_load_n(&g_owner,__ATOMIC_ACQUIRE);
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    idle=idle && g_writers_retired && g_proof_ended==g_proof_invocations;
#else
    idle=idle && !g_quarantined && g_proof_completed==g_proof_invocations;
#endif
    bool ok=tlb_proof_seal(&g_proof,g_proof.session,idle,completed);
    spin_unlock_irqrestore(&g_trace_lock,flags);
    if (!ok) return false;
    char line[384];
    int n=ksnprintf(line,sizeof(line),"\n[TLB_PROOF][SEALED] method=" PROOF_METHOD " session=0x%llx policy=%s invocations=%u bytes=%llu records=%llu fnv1a=0x%llx service=%llu capture_port_bytes=%llu\n",
        (unsigned long long)g_proof.session,g_proof_deferred?"deferred":"immediate",g_proof_invocations,
        (unsigned long long)g_proof.bytes,(unsigned long long)g_proof.records,(unsigned long long)g_proof.digest,
        (unsigned long long)completed,(unsigned long long)g_proof_port_bytes);
    if (n<0 || (size_t)n>=sizeof(line)) return false;
    if (!proof_export_line(line)) return false;
    flags=spin_lock_irqsave(&g_trace_lock);
    ok=tlb_proof_claim(&g_proof,g_proof.session);
    spin_unlock_irqrestore(&g_trace_lock,flags);
    if (!ok || !irq_is_enabled()) return false;
    if (g_proof_deferred) {
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
        size_t offset=0;
        while (offset<g_proof.bytes) {
            size_t length=0;
            while (offset+length<g_proof.bytes && length<TLB_PROOF_LINE_BYTES &&
                   g_proof.data[offset+length]!='\n') ++length;
            if (offset+length>=g_proof.bytes || length>=TLB_PROOF_LINE_BYTES ||
                !serial_proof_write(g_proof.session,g_proof.data+offset,length+1u) ||
                !serial_proof_flush(g_proof.session)) return false;
            offset+=length+1u;
        }
#else
        for (size_t i=0;i<g_proof.bytes;++i) outb(TLB_TRACE_PORT,g_proof.data[i]);
#endif
    }
    if (!cpu_request_audit_snapshot(&admitted,&completed) || admitted!=completed ||
        completed!=service_generation) return false;
    flags=spin_lock_irqsave(&g_trace_lock);
    ok=tlb_proof_finish(&g_proof,g_proof.session,g_proof.bytes);
    spin_unlock_irqrestore(&g_trace_lock,flags);
    if (!ok) return false;
    n=ksnprintf(line,sizeof(line),"\n[TLB_PROOF][EXPORTED] method=" PROOF_METHOD " session=0x%llx bytes=%llu records=%llu fnv1a=0x%llx service=%llu export_port_bytes=%llu status=COMPLETE\n",
        (unsigned long long)g_proof.session,(unsigned long long)g_proof.bytes,
        (unsigned long long)g_proof.records,(unsigned long long)g_proof.digest,
        (unsigned long long)completed,(unsigned long long)(g_proof_deferred?g_proof.bytes:0));
    if (n<0 || (size_t)n>=sizeof(line)) return false;
    return proof_export_line(line);
}

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
bool tlb_fixture_proof_export(uint64_t service_generation)
{
    if (!irq_is_enabled() || !cpu_request_context_allowed() || !g_proof_deferred ||
        !serial_proof_begin(g_proof.session)) return false;
    const char transport[]="\n[SERIAL_PROOF][BEGIN] version=2 transport=uart-115200-8n1 units=bytes newline=LF role=PHYSICAL_DIAGNOSTIC auxiliary=hex-fnv1a-v1\n";
    bool ok=serial_proof_write(g_proof.session,transport,sizeof(transport)-1u) &&
        proof_export_serial_body(service_generation);
    bool closed=serial_proof_end(g_proof.session);
    return ok && closed;
}
#endif

bool tlb_fixture_load(const tlb_interval_t *interval, cpu_request_sample_t *sample)
{
    if (!interval || !sample || !tlb_interval_valid(interval) || !interval->pages ||
        interval->pages > TLB_FIXTURE_PAGES) return false;
    unsigned window;
    for (window=0;window<TLB_FIXTURE_WINDOWS;++window)
        if(interval->first==TLB_FIXTURE_BASE+window*TLB_FIXTURE_STRIDE)break;
    if(window==TLB_FIXTURE_WINDOWS ||
       !__atomic_load_n(&g_windows[window].live,__ATOMIC_ACQUIRE))return false;
    __atomic_add_fetch(&g_readers[window],1,__ATOMIC_ACQUIRE);
    bool equal=true;
    for(uint64_t page=0;page<interval->pages;++page) {
        uint64_t value,address=interval->first+page*4096;
        __asm__ volatile("movq (%1), %0" : "=r"(value) : "r"(address) : "memory");
        if(!page)sample->value=value;
        else if(value!=sample->value)equal=false;
        ++sample->loads;
    }
    __atomic_sub_fetch(&g_readers[window],1,__ATOMIC_RELEASE);
    return equal;
}

static bool operation(unsigned window,unsigned phase,const tlb_interval_t *interval)
{
    fixture_operation_t *op=&g_windows[window].operations[phase];
    op->result=cpu_request_run_observed(phase==1 || phase==3 ? CPU_REQUEST_SAMPLE : CPU_REQUEST_INVALIDATE,
        interval,op->cpus,HOBBYOS_MAX_CPUS);
    g_windows[window].contentions+=op->result.admission_spins;
    return op->result.status==CPU_REQUEST_OK && !__atomic_load_n(&g_readers[window],__ATOMIC_ACQUIRE);
}

static bool emit_operation(unsigned window,unsigned round,unsigned phase,const tlb_interval_t *interval)
{
    const fixture_operation_t *op=&g_windows[window].operations[phase];
    const cpu_request_result_t *r=&op->result;
    bool ok=record("[TLB][R] data=%llu:%u:%u:%u:%llu:%llx:%llx:%llu:%u:%x:%x:%x:%x:%x:%x:%u:%llu:%llu:%u:%u\n",
        (unsigned long long)g_invocation,window,round,phase,(unsigned long long)r->generation,
        (unsigned long long)interval->first,(unsigned long long)interval->last,
        (unsigned long long)interval->pages,(unsigned)interval->strategy,r->requested,r->acknowledged,
        r->missing,r->failed,r->sent,r->send_failed,r->notifications,(unsigned long long)r->elapsed_ns,
        (unsigned long long)r->timing_schema,r->admission_spins,(unsigned)r->status);
    for(unsigned slot=0;slot<HOBBYOS_MAX_CPUS;++slot) {
        if(!(r->requested&(1u<<slot)))continue;
        const cpu_request_sample_t *s=&op->cpus[slot];
        ok=record("[TLB][C] generation=%llu entries=%u:%u:%llx:%llx:%u:%u\n",
            (unsigned long long)r->generation,s->slot,s->apic,
            (unsigned long long)s->value,(unsigned long long)s->cr3,s->loads,s->success)&&ok;
        char timing[512];
        ok=cpu_request_timing_format(timing,sizeof(timing),r->generation,paging_kernel_root_physical(),slot,&s->timing) &&
            record("[TLB][LOCAL] data=%s\n",timing) && ok;
    }
    return ok;
}

static bool cycle(unsigned window,unsigned round)
{
    fixture_window_t *w=&g_windows[window];
    unsigned pages=g_pages[round%5];
    uint64_t a=UINT64_C(0xa000000000000000)|(g_invocation<<24)|((uint64_t)window<<20)|round;
    uint64_t b=UINT64_C(0xb000000000000000)|(g_invocation<<24)|((uint64_t)window<<20)|round;
    *w->pointer_a=a;*w->pointer_b=b;
    tlb_interval_t interval;
    CYCLE_REQUIRE(tlb_interval_normalize(TLB_FIXTURE_BASE+window*TLB_FIXTURE_STRIDE,pages*4096u,&interval),"normalize",4);
    uint64_t ignored_old,ignored_new,old_entry,new_entry;
    CYCLE_REQUIRE(paging_tlb_fixture_replace(window,pages,0,w->frame_a,&ignored_old,&ignored_new),"map_a",4);
    CYCLE_REQUIRE(operation(window,0,&interval),"invalidate_a",0);
    CYCLE_REQUIRE(operation(window,1,&interval),"sample_a",1);
    for(unsigned slot=0;slot<HOBBYOS_MAX_CPUS;++slot)
        CYCLE_REQUIRE(!(w->operations[1].result.requested&(1u<<slot)) ||
           (w->operations[1].cpus[slot].value==a && w->operations[1].cpus[slot].loads==pages),"loads_a",1);
    CYCLE_REQUIRE(paging_tlb_fixture_replace(window,pages,w->frame_a,w->frame_b,&old_entry,&new_entry),"map_b",4);
    tlb_completion_t handle={0};
    tlb_deferred_status_t queued=TLB_DEFERRED_INVALID,initial=TLB_DEFERRED_INVALID,status=TLB_DEFERRED_INVALID;
    bool released=false;
    if(round==320) {
        irq_flags_t flags=irq_save();
        queued=kernel_tlb_defer(interval.first,pages*4096u,&handle);
        cpu_request_result_t pending={0};
        initial=queued==TLB_DEFERRED_QUEUED ? kernel_tlb_poll(handle,&pending) : TLB_DEFERRED_INVALID;
        irq_restore(flags);
        CYCLE_REQUIRE(queued==TLB_DEFERRED_QUEUED && initial==TLB_DEFERRED_PENDING,"deferred_admission",2);
        uint64_t deadline=clock_monotonic_ns()+UINT64_C(2000000000);
        do {
            status=kernel_tlb_poll(handle,&w->operations[2].result);
            if(status!=TLB_DEFERRED_PENDING)break;
            timer_sleep(1);
        } while(clock_monotonic_ns()<deadline);
        CYCLE_REQUIRE(status==TLB_DEFERRED_COMPLETE &&
           kernel_tlb_deferred_samples(handle,w->operations[2].cpus,HOBBYOS_MAX_CPUS) &&
           *w->pointer_a==a,"deferred_completion",2);
        released=kernel_tlb_release(handle);
        CYCLE_REQUIRE(released,"deferred_release",2);
    } else CYCLE_REQUIRE(operation(window,2,&interval),"invalidate_b",2);
    CYCLE_REQUIRE(operation(window,3,&interval),"sample_b",3);
    for(unsigned slot=0;slot<HOBBYOS_MAX_CPUS;++slot)
        CYCLE_REQUIRE(!(w->operations[3].result.requested&(1u<<slot)) ||
           (w->operations[3].cpus[slot].value==b && w->operations[3].cpus[slot].loads==pages),"loads_b",3);
    CYCLE_REQUIRE(*w->pointer_a==a && *w->pointer_b==b,"physical_markers",4);
    if(round==320 && !record("[TLB][DEFERRED] invocation=%llu window=%u round=%u token=%llu slot=%u queued=%u "
        "initial=%u completed=%u generation=%llu old_marker=0x%llx handle_released=%u\n",
        (unsigned long long)g_invocation,window,round,(unsigned long long)handle.generation,handle.slot,
        (unsigned)queued,(unsigned)initial,(unsigned)status,
        (unsigned long long)w->operations[2].result.generation,(unsigned long long)*w->pointer_a,released))return false;
    bool ok=record("[TLB][X] data=%llu:%u:%u:%llx:%llx:%llx:%llx:%llx:%llx:%llx:0\n",
        (unsigned long long)g_invocation,window,round,(unsigned long long)paging_tlb_fixture_entry(window),
        (unsigned long long)old_entry,(unsigned long long)new_entry,(unsigned long long)w->frame_a,
        (unsigned long long)w->frame_b,(unsigned long long)a,(unsigned long long)b);
    for(unsigned phase=0;phase<4;++phase)ok=emit_operation(window,round,phase,&interval)&&ok;
    CYCLE_REQUIRE(ok,"record",4);
    return true;
}

static bool prepare(unsigned window)
{
    fixture_window_t *w=&g_windows[window];
    w->contentions=0;
    w->frame_a=(uint64_t)pmm_alloc_frame();w->frame_b=(uint64_t)pmm_alloc_frame();
    if (w->frame_a) w->pointer_a=physical_require_pointer(w->frame_a, PAGE_SIZE, PHYSICAL_RAM);
    if (w->frame_b) w->pointer_b=physical_require_pointer(w->frame_b, PAGE_SIZE, PHYSICAL_RAM);
    if(!w->frame_a || !w->frame_b || w->frame_a==w->frame_b ||
       (w->frame_a&4095) || (w->frame_b&4095) ||
       w->frame_a>=physical_allocation_ceiling() || w->frame_b>=physical_allocation_ceiling() ||
       w->frame_a>=pmm_get_total_memory() || w->frame_b>=pmm_get_total_memory() ||
       !paging_tlb_fixture_prepare(window,w->frame_a) || !paging_image_boot_check("LIVE_SCRATCH"))return false;
    __atomic_store_n(&w->live,1,__ATOMIC_RELEASE);return true;
}

static bool cleanup(unsigned window)
{
    fixture_window_t *w=&g_windows[window];
    if(__atomic_load_n(&g_readers[window],__ATOMIC_ACQUIRE) || !paging_tlb_fixture_clear(window))return false;
    tlb_interval_t interval;
    if(!tlb_interval_normalize(TLB_FIXTURE_BASE+window*TLB_FIXTURE_STRIDE,TLB_FIXTURE_PAGES*4096u,&interval) ||
       !operation(window,0,&interval))return false;
    __atomic_store_n(&w->live,0,__ATOMIC_RELEASE);
    pmm_free_frame((void*)w->frame_a);pmm_free_frame((void*)w->frame_b);
    bool freed=pmm_is_frame_free(w->frame_a)&&pmm_is_frame_free(w->frame_b);
    bool ok=emit_operation(window,UINT32_MAX,0,&interval);
    ok=record("[TLB][RETIRE] invocation=%llu window=%u generation=%llu data_frames=2 released=%u retained_tables=OWNED\n",
        (unsigned long long)g_invocation,window,(unsigned long long)w->operations[0].result.generation,freed?2:0)&&ok;
    w->frame_a=w->frame_b=0;return freed&&ok;
}

static void concurrent_worker(void *argument)
{
    unsigned window=(unsigned)(uintptr_t)argument;fixture_window_t *w=&g_windows[window];
    scheduler_cpu_pin_t pin={0};
    if (!scheduler_cpu_pin(&pin)) {w->failed=1;__atomic_store_n(&w->finished,1,__ATOMIC_RELEASE);return;}
    const task_t *self=get_current_task();
    if (self) {
        g_worker_tasks[window]=(task_handle_t){self->id,self->lifecycle_generation};
        g_worker_stacks[window]=self->stack_owner;
    }
    scheduler_cpu_unpin(&pin);
    if (!self) {w->failed=1;__atomic_store_n(&w->finished,1,__ATOMIC_RELEASE);return;}
    __atomic_add_fetch(&g_barrier,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&g_release,__ATOMIC_ACQUIRE))schedule_voluntary();
    __atomic_store_n(&w->started_ns,clock_monotonic_ns(),__ATOMIC_RELEASE);
    for(unsigned round=0;round<TLB_FIXTURE_CONCURRENT_ROUNDS;++round)
        if(!cycle(window,TLB_FIXTURE_ROUNDS+round)) {w->failed=1;break;} else ++w->rounds;
    w->ended_ns=clock_monotonic_ns();__atomic_store_n(&w->finished,1,__ATOMIC_RELEASE);
}

static bool wait_worker_retirement(void)
{
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    fixture_wait_clock_t wait={0};
    if (!clock_monotonic_read_ns(&wait.last) || wait.last>UINT64_MAX-UINT64_C(15000000000)) return false;
    uint64_t deadline=wait.last+UINT64_C(15000000000);
#else
    uint64_t deadline=clock_monotonic_ns()+UINT64_C(15000000000);
#endif
    for (unsigned window=0;window<
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
        g_workers_created
#else
        2u
#endif
        ;++window) {
        stack_handle_t handle=g_worker_stacks[window];stack_owner_view_t owner;
        bool done=false;
        if (!g_worker_tasks[window].id || !handle.generation) return false;
        while (
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
            fixture_wait_before(deadline,&wait)
#else
            clock_monotonic_ns()<deadline
#endif
        ) {
            task_snapshot_t snapshot;
            if (!scheduler_snapshot_task_by_handle(g_worker_tasks[window],&snapshot) &&
                kernel_stack_read(handle.slot,&owner) &&
                (owner.generation>handle.generation || (owner.generation==handle.generation &&
                    owner.state==STACK_FREE && owner.retirement_generation))) {done=true;break;}
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
            schedule_voluntary();
#else
            timer_sleep(1);
#endif
        }
        if (!done || !record("[TLB][WORKER_REAP] invocation=%llu window=%u task=%llu lifecycle=%llu slot=%u generation=%llu observed_generation=%llu retirement=%llu complete=1\n",
            (unsigned long long)g_invocation,window,(unsigned long long)g_worker_tasks[window].id,
            (unsigned long long)g_worker_tasks[window].lifecycle_generation,handle.slot,
            (unsigned long long)handle.generation,(unsigned long long)owner.generation,
            (unsigned long long)(owner.generation==handle.generation ? owner.retirement_generation : 0))) return false;
    }
    return true;
}

static bool observe_under_load(void)
{
    uint64_t started=clock_monotonic_ns();
    const cpu_control_capture_t *capture=cpu_control_snapshot();
    if(!capture)return false;
    bool complete=capture->complete;
    bool recorded=record("[TLB][OBSERVER] invocation=%llu generation=%llu targets=0x%x complete=0x%x errors=0x%x "
        "start_ns=%llu end_ns=%llu status=%u\n",(unsigned long long)g_invocation,
        (unsigned long long)capture->generation,capture->target_mask,capture->complete_mask,capture->error_mask,
        (unsigned long long)started,(unsigned long long)clock_monotonic_ns(),complete);
    cpu_control_snapshot_release();return complete&&recorded;
}

static uint64_t g_audit_emitted;
static bool emit_audit(uint64_t admitted_before, uint64_t completed_before)
{
    uint64_t admitted, completed;
    if (!cpu_request_audit_snapshot(&admitted, &completed) || admitted != completed) return false;
    bool ok = record("[TLB][AUDIT] invocation=%llu before=%llu completed_before=%llu after=%llu emitted_before=%llu root=0x%llx\n",
        (unsigned long long)g_invocation, (unsigned long long)admitted_before,
        (unsigned long long)completed_before, (unsigned long long)completed,
        (unsigned long long)g_audit_emitted, (unsigned long long)paging_kernel_root_physical());
    for (uint64_t generation=g_audit_emitted+1; ok && generation<=completed; ++generation) {
        cpu_request_audit_t audit;
        if (!cpu_request_audit_copy(generation, &audit)) return false;
        const cpu_request_spec_t *s=&audit.spec;
        const cpu_request_result_t *r=&audit.result;
        char identities[384]; size_t used=0;
        for (unsigned slot=0; slot<HOBBYOS_MAX_CPUS; ++slot) if(s->targets&(1u<<slot)) {
            int n=ksnprintf(identities+used,sizeof(identities)-used,"%s%u:%u",used?",":"",slot,s->apic[slot]);
            if(n<1 || (size_t)n>=sizeof(identities)-used)return false;
            used+=(size_t)n;
        }
        ok=record("[TLB][SERVICE] generation=%llu operation=%u mapping=%u root=0x%llx first=0x%llx last=0x%llx pages=%llu strategy=%u targets=0x%x ack=0x%x missing=0x%x failed=0x%x sent=0x%x send_failed=0x%x notifications=%u status=%u elapsed_ns=%llu identities=%s\n",
            (unsigned long long)s->generation,(unsigned)s->operation,audit.mapping,(unsigned long long)s->root,
            (unsigned long long)s->interval.first,(unsigned long long)s->interval.last,(unsigned long long)s->interval.pages,
            (unsigned)s->interval.strategy,r->requested,r->acknowledged,r->missing,r->failed,r->sent,r->send_failed,
            r->notifications,(unsigned)r->status,(unsigned long long)r->elapsed_ns,identities);
    }
    if(ok)g_audit_emitted=completed;
    return ok;
}

static bool image_leaf_record(void *context,const image_leaf_run_t *leaf)
{
    if (g_proof.state!=TLB_PROOF_UNUSED && ++g_proof_rights>g_proof_rights_limit) {
        g_proof.failed=true;return false;
    }
    unsigned stage=*(const unsigned *)context;
    return record("[TLB][RIGHTS] invocation=%llu stage=%u va=0x%llx physical=0x%llx bytes=%llu shift=%u flags=0x%llx writable=%u nx=%u\n",
        (unsigned long long)g_invocation,stage,(unsigned long long)leaf->address,
        (unsigned long long)leaf->physical,(unsigned long long)leaf->bytes,leaf->shift,
        (unsigned long long)leaf->flags,leaf->writable,leaf->nx);
}

static bool image_stack_record(void *context,const stack_owner_view_t *v)
{
    unsigned stage=*(const unsigned *)context;
    return record("[TLB][STACK] invocation=%llu stage=%u slot=%u generation=%llu kind=%u state=%u task=%llu lifecycle=%llu cpu=%u apic=%u guard=0x%llx base=0x%llx end=0x%llx physical=0x%llx capacity=%u owned=%u published=%u retirement=%llu\n",
        (unsigned long long)g_invocation,stage,v->slot,(unsigned long long)v->generation,v->kind,v->state,
        (unsigned long long)v->task_id,(unsigned long long)v->task_lifecycle,v->cpu,v->apic,
        (unsigned long long)v->guard,(unsigned long long)v->base,(unsigned long long)v->end,
        (unsigned long long)v->physical,v->capacity,v->owned,v->ever_published,(unsigned long long)v->retirement_generation);
}

static bool image_scan_record(unsigned stage)
{
    g_proof_rights=0;
    image_walk_result_t scan={0};
    if (!paging_image_snapshot_owners(&scan,image_leaf_record,image_stack_record,&stage)) return false;
    return record("[TLB][RIGHTSSCAN] invocation=%llu stage=%u root=0x%llx tables=%llu leaves=%llu bytes=%llu image_bytes=%llu direct_bytes=%llu managed_bytes=%llu scratch_bytes=%llu stack_bytes=%llu rwx_bytes=%llu nx=%u complete=%u\n",
        (unsigned long long)g_invocation,stage,(unsigned long long)scan.root,
        (unsigned long long)scan.tables,(unsigned long long)scan.leaves,(unsigned long long)scan.bytes,
        (unsigned long long)scan.image_bytes,(unsigned long long)scan.direct_bytes,
        (unsigned long long)scan.managed_bytes,(unsigned long long)scan.scratch_bytes,(unsigned long long)scan.stack_bytes,
        (unsigned long long)scan.rwx_bytes,scan.nx,scan.complete);
}

int tlb_fixture_run(void)
{
    uint32_t free=0;
    if(!__atomic_compare_exchange_n(&g_owner,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    if(g_quarantined || g_invocation>=UINT32_MAX ||
       (g_proof.state!=TLB_PROOF_UNUSED && (g_proof.state!=TLB_PROOF_CAPTURING ||
        g_proof.failed || g_invocation>=g_proof_invocations))) {__atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return 1;}
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    if (!g_proof_deferred || g_proof.state==TLB_PROOF_UNUSED) {
        __atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return 1;
    }
#else
    if(inb(TLB_TRACE_PORT)!=0xe9u) {__atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return 1;}
#endif
    g_trace_bytes=g_trace_records=0;g_trace_digest=TLB_TRACE_FNV_OFFSET;
    uint32_t tables_before=paging_tlb_fixture_tables();
    uint64_t admitted_before=0, completed_before=0;
    bool checkpoint=cpu_request_audit_snapshot(&admitted_before,&completed_before);
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    g_failure_stage=NULL;g_workers_created=0;g_writers_retired=false;
    memset(g_cycle_failure,0,sizeof(g_cycle_failure));
    memset(g_worker_tasks,0,sizeof(g_worker_tasks));
    memset(g_worker_stacks,0,sizeof(g_worker_stacks));
#endif
    ++g_invocation;bool ok=fixture_check(checkpoint && admitted_before==completed_before,"admission") &&
        fixture_check(prepare(0),"prepare_primary");
    ok=record("[TLB][BEGIN] schema=7 invocation=%llu cpus=%u x2apic=%u sequential=%u concurrent_each=%u "
        "threshold=64 deadline_ns=1000000000 window_pages=65 deferred=1 observers=3\n",(unsigned long long)g_invocation,g_cpu_count,
        lapic_is_x2apic(),TLB_FIXTURE_ROUNDS,g_cpu_count>1?TLB_FIXTURE_CONCURRENT_ROUNDS:0)&&ok;
    if (ok) ok=fixture_check(image_scan_record(0),"initial_scan");
    for(unsigned round=0;ok && round<TLB_FIXTURE_ROUNDS;++round) {
        ok=fixture_check(cycle(0,round),"sequential_cycle");
        if(ok && g_cpu_count==1 && (round==63 || round==127 || round==191))ok=fixture_check(observe_under_load(),"concurrent_observer");
    }
    if(ok && g_cpu_count>1) {
        ok=fixture_check(prepare(1),"prepare_secondary") && fixture_check(image_scan_record(1),"concurrent_scan");g_barrier=g_release=0;
        for(unsigned window=0;ok && window<2;++window) {
            g_windows[window].finished=g_windows[window].failed=g_windows[window].rounds=0;
            g_windows[window].started_ns=0;
            ok=fixture_check(thread_create_named_with_class_flags(concurrent_worker,(void*)(uintptr_t)window,
                TASK_CLASS_NORMAL,"tlb-initiator",TASK_FLAG_SYSTEM|TASK_FLAG_KILL_PROTECTED),"create_initiator");
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
            if (ok) ++g_workers_created;
#endif
        }
        uint64_t deadline=clock_monotonic_ns()+UINT64_C(120000000000);
        while(ok && __atomic_load_n(&g_barrier,__ATOMIC_ACQUIRE)!=2 && clock_monotonic_ns()<deadline)timer_sleep(1);
        ok=fixture_check(ok && __atomic_load_n(&g_barrier,__ATOMIC_ACQUIRE)==2,"initiator_barrier");
        __atomic_store_n(&g_release,1,__ATOMIC_RELEASE);
        while(ok && (!__atomic_load_n(&g_windows[0].started_ns,__ATOMIC_ACQUIRE) ||
              !__atomic_load_n(&g_windows[1].started_ns,__ATOMIC_ACQUIRE)) && clock_monotonic_ns()<deadline)timer_sleep(1);
        for(unsigned observation=0;ok && observation<3;++observation)ok=fixture_check(observe_under_load(),"concurrent_observer");
        while(ok && (!__atomic_load_n(&g_windows[0].finished,__ATOMIC_ACQUIRE) ||
              !__atomic_load_n(&g_windows[1].finished,__ATOMIC_ACQUIRE)) && clock_monotonic_ns()<deadline)timer_sleep(1);
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
        /* Failure keeps mappings quarantined, but all created writers must
         * finish before their immutable proof can be sealed or audited. */
        fixture_wait_clock_t wait={0};
        for (unsigned window=0;window<g_workers_created;++window)
            while (!__atomic_load_n(&g_windows[window].finished,__ATOMIC_ACQUIRE) && fixture_wait_before(deadline,&wait))
                schedule_voluntary();
#endif
        for(unsigned window=0;window<2;++window) {
            fixture_window_t *w=&g_windows[window];
            ok=fixture_check(ok && __atomic_load_n(&w->finished,__ATOMIC_ACQUIRE) && !w->failed && w->rounds==TLB_FIXTURE_CONCURRENT_ROUNDS,"initiator_completion");
            if (__atomic_load_n(&w->finished,__ATOMIC_ACQUIRE))
                ok=record("[TLB][INITIATOR] invocation=%llu window=%u start_ns=%llu end_ns=%llu rounds=%u contention=%llu\n",
                    (unsigned long long)g_invocation,window,(unsigned long long)w->started_ns,
                    (unsigned long long)w->ended_ns,w->rounds,(unsigned long long)w->contentions)&&ok;
        }
        ok=fixture_check(ok && g_windows[0].started_ns<g_windows[1].ended_ns && g_windows[1].started_ns<g_windows[0].ended_ns,"initiator_overlap");
        if(ok)ok=fixture_check(cleanup(1),"cleanup_secondary");
    }
    if(ok)ok=fixture_check(cycle(0,320),"deferred_cycle");
    if(ok)ok=fixture_check(cleanup(0),"cleanup_primary");
    /* Reaper invalidations are real service traffic. Seal this invocation only
     * after both real initiator stacks have completed their normal retirement. */
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    bool finished=true;
    for (unsigned window=0;window<g_workers_created;++window)
        finished=finished && __atomic_load_n(&g_windows[window].finished,__ATOMIC_ACQUIRE);
    g_writers_retired=finished && (!g_workers_created || wait_worker_retirement());
    ok=fixture_check(g_writers_retired,"initiator_retirement") && ok;
#else
    if(ok && g_cpu_count>1)ok=wait_worker_retirement();
#endif
    if(ok)ok=fixture_check(image_scan_record(2),"final_scan");
    uint32_t tables_after=paging_tlb_fixture_tables();
    ok=record("[TLB][TABLES] invocation=%llu before=%u after=%u retained=OWNED\n",
        (unsigned long long)g_invocation,tables_before,tables_after)&&ok;
    if(g_invocation>1 && tables_before!=tables_after)ok=fixture_check(false,"table_growth");
    ok=fixture_check(emit_audit(admitted_before,completed_before),"audit")&&ok;
    if(!ok)g_quarantined=true;
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    if (g_writers_retired) for (unsigned window=0;window<TLB_FIXTURE_WINDOWS;++window) {
        if (!g_cycle_failure[window].stage) continue;
        unsigned phase=g_cycle_failure[window].phase;
        const cpu_request_result_t *r=phase<4 ? &g_windows[window].operations[phase].result : NULL;
        ok=record("[TLB][CYCLE_FAILURE] invocation=%llu window=%u round=%u stage=%s phase=%u generation=%llu status=%u targets=0x%x ack=0x%x missing=0x%x failed=0x%x\n",
            (unsigned long long)g_invocation,window,g_cycle_failure[window].round,g_cycle_failure[window].stage,phase,
            (unsigned long long)(r?r->generation:0),r?(unsigned)r->status:CPU_REQUEST_INVALID,
            r?r->requested:0,r?r->acknowledged:0,r?r->missing:0,r?r->failed:0)&&ok;
    }
    ok=record("[TLB][OUTCOME] invocation=%llu status=%s stage=%s writers_retired=%u workers_created=%u quarantined=%u\n",
        (unsigned long long)g_invocation,ok?"PASS":"FAIL",g_failure_stage?g_failure_stage:ok?"complete":"record",
        g_writers_retired,g_workers_created,g_quarantined)&&ok;
#endif
    ok=record("[TLB][END] invocation=%llu status=%s replacements=%u retained=%u\n",
        (unsigned long long)g_invocation,ok?"PASS":"FAIL",TLB_FIXTURE_ROUNDS+1+
        (g_cpu_count>1?2*TLB_FIXTURE_CONCURRENT_ROUNDS:0),ok?0:1)&&ok;
    if (!ok) g_quarantined=true;
    if (ok && g_proof.state!=TLB_PROOF_UNUSED) ++g_proof_completed;
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    if (g_proof.state!=TLB_PROOF_UNUSED && g_writers_retired) ++g_proof_ended;
#endif
    char receipt[256];
    int length=ksnprintf(receipt,sizeof(receipt),
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
        "\n[TLB_TRACE][%s] invocation=%llu channel=ram export=uart records=%llu bytes=%llu fnv1a=0x%llx status=%s\n",
#else
        "\n[TLB_TRACE][%s] invocation=%llu channel=debugcon port=0x402 records=%llu bytes=%llu fnv1a=0x%llx status=%s\n",
#endif
        g_proof.state==TLB_PROOF_UNUSED?"COMPLETE":"CAPTURED",
        (unsigned long long)g_invocation,(unsigned long long)g_trace_records,
        (unsigned long long)g_trace_bytes,(unsigned long long)g_trace_digest,ok?"PASS":"FAIL");
    if(length<0 || (size_t)length>=sizeof(receipt))ok=false;
    else serial_write_all(receipt);
    __atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return ok?0:1;
}

static bool failure_audit(uint64_t first, uint64_t *last)
{
    uint64_t admitted,completed;
    if(!last || !cpu_request_audit_snapshot(&admitted,&completed) || admitted!=completed)return false;
    for(uint64_t generation=first;generation<=completed;++generation) {
        cpu_request_audit_t audit;
        if(!cpu_request_audit_copy(generation,&audit))return false;
        const cpu_request_spec_t *s=&audit.spec;
        const cpu_request_result_t *r=&audit.result;
        char identities[384];size_t used=0;
        for(unsigned slot=0;slot<HOBBYOS_MAX_CPUS;++slot)if(s->targets&(1u<<slot)) {
            int n=ksnprintf(identities+used,sizeof(identities)-used,"%s%u:%u",used?",":"",slot,s->apic[slot]);
            if(n<1 || (size_t)n>=sizeof(identities)-used)return false;
            used+=(size_t)n;
        }
        if(!record("[TLB_FAILURE][SERVICE] generation=%llu operation=%u mapping=%u root=0x%llx first=0x%llx last=0x%llx pages=%llu strategy=%u targets=0x%x ack=0x%x missing=0x%x failed=0x%x sent=0x%x send_failed=0x%x notifications=%u status=%u elapsed_ns=%llu identities=%s\n",
            (unsigned long long)s->generation,(unsigned)s->operation,audit.mapping,(unsigned long long)s->root,
            (unsigned long long)s->interval.first,(unsigned long long)s->interval.last,(unsigned long long)s->interval.pages,
            (unsigned)s->interval.strategy,r->requested,r->acknowledged,r->missing,r->failed,r->sent,r->send_failed,
            r->notifications,(unsigned)r->status,(unsigned long long)r->elapsed_ns,identities))return false;
    }
    *last=completed;return true;
}
static bool failure_checkpoint(uint64_t *before)
{
    uint64_t completed;
    if(!cpu_request_audit_snapshot(before,&completed) || *before!=completed)return false;
    return record("[TLB_FAILURE][CHECKPOINT] before=%llu completed=%llu root=0x%llx\n",
        (unsigned long long)*before,(unsigned long long)completed,
        (unsigned long long)paging_kernel_root_physical()) && failure_audit(1,&completed) && completed==*before;
}

int tlb_fixture_context_failure(void)
{
    uint64_t before;
    if(!failure_checkpoint(&before))return 1;
    irq_flags_t flags=irq_save();
    cpu_request_result_t result=kernel_tlb_invalidate(TLB_FIXTURE_BASE,4096);
    irq_restore(flags);
    return result.status==CPU_REQUEST_CONTEXT?0:1;
}

int tlb_fixture_delayed_response(void)
{
    uint32_t free=0;
    if(g_cpu_count<2 || !__atomic_compare_exchange_n(&g_owner,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    uint64_t before;
    if(g_quarantined || !failure_checkpoint(&before) || !prepare(0)) {g_quarantined=true;return 1;}
    fixture_window_t *w=&g_windows[0];
    *w->pointer_a=UINT64_C(0xa55aa55aa55aa55a);
    *w->pointer_b=UINT64_C(0xb55bb55bb55bb55b);
    tlb_interval_t interval;uint64_t old_entry,new_entry;
    bool ok=tlb_interval_normalize(TLB_FIXTURE_BASE,4096,&interval) &&
        operation(0,0,&interval) && operation(0,1,&interval) &&
        paging_tlb_fixture_replace(0,1,w->frame_a,w->frame_b,&old_entry,&new_entry) &&
        cpu_request_test_delay(1);
    cpu_request_result_t result={.status=CPU_REQUEST_INVALID},after=result;
    lapic_delivery_result_t delivery=LAPIC_DELIVERY_INVALID;
    uint32_t delayed=0;
    if(ok) {
        result=kernel_tlb_invalidate(TLB_FIXTURE_BASE,4096);
        delayed=cpu_request_test_delayed_entries();cpu_request_test_release_delay();
        delivery=lapic_send_ipi(g_cpus[1].apic_id,INT_VECTOR_CPU_REQUEST);
        timer_sleep(10);
        after=kernel_tlb_invalidate(TLB_FIXTURE_BASE,4096);
        ok=result.status==CPU_REQUEST_TIMEOUT && result.missing==2 && delayed &&
            result.elapsed_ns>=CPU_REQUEST_DEADLINE_NS && result.elapsed_ns<CPU_REQUEST_DEADLINE_NS*2 &&
            delivery==LAPIC_DELIVERY_SUBMITTED && after.status==CPU_REQUEST_QUARANTINED &&
            *w->pointer_a==UINT64_C(0xa55aa55aa55aa55a) &&
            !pmm_is_frame_free(w->frame_a) && !pmm_is_frame_free(w->frame_b);
    }
    g_quarantined=true;
    uint64_t terminal;
    ok=failure_audit(before+1,&terminal) && terminal==result.generation && ok;
    (void)record("[TLB_FAILURE][DELAYED] status=%s generation=%llu slot=1 apic=%u expected=0x%x ack=0x%x missing=0x%x "
        "result=%u elapsed_ns=%llu withheld=%u late_delivery=%u reuse_result=%u frame_a=0x%llx frame_b=0x%llx retained=2\n",
        ok?"EXPECTED":"INVALID",(unsigned long long)result.generation,g_cpus[1].apic_id,
        result.requested,result.acknowledged,result.missing,(unsigned)result.status,(unsigned long long)result.elapsed_ns,
        delayed,(unsigned)delivery,(unsigned)after.status,(unsigned long long)w->frame_a,(unsigned long long)w->frame_b);
    __atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return ok?0:1;
}
