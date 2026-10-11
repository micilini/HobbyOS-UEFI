#include "protection_probe.h"
#include "image_walk.h"
#include "paging.h"
#include "../cpu/cpu.h"
#include "../cpu/control_policy.h"
#include "../smp/smp_topology.h"
#include "../core/io.h"
#include "../core/idt.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../shell/commands/cmd_tasktest.h"
#include <stdarg.h>

extern uint8_t protection_text_target[],protection_rodata_target[],protection_data_target[];
extern char protection_store_site[],protection_store_recovery[],protection_fetch_recovery[];
extern unsigned protection_probe_store(void *,unsigned),protection_probe_fetch(void *);
typedef struct {
    protection_probe_state_t attempts[PROTECTION_ATTEMPTS];
    uint64_t generation,cr0,cr3,cr4,efer;
    uint32_t extended_edx,completed,unsupported;
    uint8_t before[PROTECTION_ATTEMPTS],after[PROTECTION_ATTEMPTS];
} protection_cpu_result_t;
static protection_cpu_result_t g_results[HOBBYOS_MAX_CPUS];
static protection_target_t g_targets[PROTECTION_ATTEMPTS];
static uint32_t g_active_attempt[HOBBYOS_MAX_CPUS],g_owner,g_active;
static uint64_t g_collection,g_expected_root;
static bool g_nx,g_quarantined;
static uint64_t g_records,g_bytes,g_digest;

static bool emit(const char *format,...) __attribute__((format(printf,1,2)));
static bool emit(const char *format,...)
{
    char line[768];va_list args;va_start(args,format);
    int n=kvsnprintf(line,sizeof(line),format,args);va_end(args);
    if(n<1 || (size_t)n>=sizeof(line)-32 || line[n-1]!='\n') return false;
    uint32_t crc=tasktest_transport_crc32(line,n-1);
    int tail=ksnprintf(line+n-1,sizeof(line)-(size_t)n+1," crc=%08x\n",crc);
    if(tail<0 || (size_t)tail>=sizeof(line)-(size_t)n+1) return false;
    irq_flags_t flags=irq_save();
    for(const unsigned char *p=(const unsigned char *)line;*p;++p) {
        outb(0x402,*p);g_digest=(g_digest^*p)*UINT64_C(1099511628211);++g_bytes;
    }
    ++g_records;irq_restore(flags);return true;
}
#define U64(x) ((unsigned long long)(x))
static bool leaf(void *context,const image_leaf_run_t *r)
{
    (void)context;
    return emit("[IMAGE_PROTECTION][LEAF] va=0x%llx physical=0x%llx bytes=%llu shift=%u flags=0x%llx writable=%u nx=%u\n",
        U64(r->address),U64(r->physical),U64(r->bytes),r->shift,U64(r->flags),r->writable,r->nx);
}

static bool stack_owner(void *context,const stack_owner_view_t *v)
{
    (void)context;
    return emit("[IMAGE_PROTECTION][STACK] slot=%u generation=%llu kind=%u state=%u task=%llu lifecycle=%llu cpu=%u apic=%u guard=0x%llx base=0x%llx end=0x%llx physical=0x%llx capacity=%u owned=%u published=%u retirement=%llu\n",
        v->slot,U64(v->generation),v->kind,v->state,U64(v->task_id),U64(v->task_lifecycle),v->cpu,v->apic,
        U64(v->guard),U64(v->base),U64(v->end),U64(v->physical),v->capacity,v->owned,v->ever_published,U64(v->retirement_generation));
}

bool protection_probe_fixup(InterruptFrame *frame,uint64_t address,uint64_t error)
{
    cpu_slot_t slot;uint32_t apic;
    if (!frame || !__atomic_load_n(&g_active,__ATOMIC_ACQUIRE) || !smp_current_cpu_slot(&slot) ||
        slot>=HOBBYOS_MAX_CPUS || !cpu_local_arch_id(&apic)) return false;
    unsigned selected=__atomic_load_n(&g_active_attempt[slot],__ATOMIC_ACQUIRE);
    if (!selected || selected>PROTECTION_ATTEMPTS) return false;
    protection_probe_state_t *s=&g_results[slot].attempts[selected-1];
    uint64_t root;__asm__ volatile("mov %%cr3,%0" : "=r"(root));
    protection_fault_t event={g_results[slot].generation,g_collection,root,frame->rip,address,error,frame->cs,slot,apic};
    if (root!=g_expected_root || !protection_probe_match(s,&g_targets[selected-1],&event)) return false;
    s->fault_rip=frame->rip;s->fault_address=address;s->fault_error=error;s->fault_cs=frame->cs;
    s->seen=true;s->armed=false;frame->rip=s->recovery;return true;
}

bool protection_probe_execute(const cpu_request_spec_t *spec,cpu_request_sample_t *sample)
{
    cpu_slot_t slot;uint32_t apic,a,b,c,d;
    if (!spec || !sample || spec->operation!=CPU_REQUEST_PROTECTION || spec->interval.pages ||
        !__atomic_load_n(&g_active,__ATOMIC_ACQUIRE) || !smp_current_cpu_slot(&slot) ||
        slot>=HOBBYOS_MAX_CPUS || slot!=sample->slot || !cpu_local_arch_id(&apic) ||
        apic!=sample->apic || apic!=spec->apic[slot] || spec->root!=g_expected_root ||
        !spec->generation || g_results[slot].generation || !cpu_get_cpuid_count(0x80000001,0,&a,&b,&c,&d)) return false;
    protection_cpu_result_t *result=&g_results[slot];
    __asm__ volatile("mov %%cr0,%0; mov %%cr3,%1; mov %%cr4,%2"
        : "=r"(result->cr0),"=r"(result->cr3),"=r"(result->cr4));
    if (!cpu_read_msr_safe(CPU_MSR_EFER,&result->efer) || result->cr0!=CPU_CR0_FINAL ||
        result->cr3!=spec->root || result->cr4!=CPU_CR4_FINAL ||
        result->efer!=(uint64_t)(CPU_EFER_LME|CPU_EFER_LMA|(g_nx ? CPU_EFER_NXE : 0)) ||
        !!(d&CPU_FEATURE_NX)!=g_nx) return false;
    result->generation=spec->generation;result->extended_edx=d;
    protection_fault_t owner={.generation=spec->generation,.collection=g_collection,
        .root=result->cr3,.slot=slot,.apic=apic};
    for (unsigned i=0;i<PROTECTION_ATTEMPTS;++i) {
        if (i>=4 && !g_nx) {result->unsupported|=1u<<i;continue;}
        const protection_target_t *target=&g_targets[i];
        protection_probe_state_t *state=&result->attempts[i];
        result->before[i]=*(volatile uint8_t *)(uintptr_t)target->address;
        if (result->before[i]!=target->original || !protection_probe_arm(state,target,&owner,i,g_nx)) return false;
        __atomic_store_n(&g_active_attempt[slot],i+1,__ATOMIC_RELEASE);
        __asm__ volatile("" ::: "memory");
        unsigned returned=target->kind==PROTECTION_FETCH_DATA ? protection_probe_fetch((void *)(uintptr_t)target->address) :
            protection_probe_store((void *)(uintptr_t)target->address,target->original);
        __asm__ volatile("" ::: "memory");
        __atomic_store_n(&g_active_attempt[slot],0,__ATOMIC_RELEASE);
        result->after[i]=*(volatile uint8_t *)(uintptr_t)target->address;
        if (!protection_probe_complete(state,target,returned,result->after[i])) {state->armed=false;return false;}
        result->completed|=1u<<i;
    }
    sample->value=result->completed;sample->loads=0;
    /* The request handler publishes its ACK only after this return. */
    return result->completed==(g_nx?63u:15u) && result->unsupported==(g_nx?0u:48u) && !g_active_attempt[slot];
}

static bool prepare_targets(void)
{
    const uint64_t linked[]={ (uintptr_t)protection_text_target,(uintptr_t)protection_rodata_target,(uintptr_t)protection_data_target };
    const image_role_t roles[]={IMAGE_TEXT,IMAGE_RODATA,IMAGE_DATA};
    const uint8_t original[]={0x5a,0xa7,0xc3};
    for (unsigned kind=0;kind<3;++kind) {
        uint64_t physical,direct;bool nx;
        if (!paging_image_target(linked[kind],8,roles[kind],&physical,&direct,&nx) || (kind && nx!=g_nx)) return false;
        g_nx=nx;
        for (unsigned alias=0;alias<2;++alias) {
            uint64_t address=alias ? direct : linked[kind];
            g_targets[kind*2+alias]=(protection_target_t){address,physical,
                kind==PROTECTION_FETCH_DATA ? address : (uintptr_t)protection_store_site,
                kind==PROTECTION_FETCH_DATA ? (uintptr_t)protection_fetch_recovery : (uintptr_t)protection_store_recovery,
                kind,alias,original[kind]};
            if (*(volatile uint8_t *)(uintptr_t)address!=original[kind]) return false;
        }
    }
    for (unsigned i=1;i<8;++i) if (protection_data_target[i]) return false;
    return true;
}

int protection_probe_run(void)
{
    uint32_t empty=0;
    if (!cpu_request_context_allowed() || !__atomic_compare_exchange_n(&g_owner,&empty,1,false,
        __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    if (g_quarantined || g_collection==UINT64_MAX || !prepare_targets()) {
        __atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return 1;
    }
    ++g_collection;g_expected_root=paging_kernel_root_physical();
    memset(g_results,0,sizeof(g_results));memset(g_active_attempt,0,sizeof(g_active_attempt));
    g_records=g_bytes=0;g_digest=UINT64_C(14695981039346656037);
    bool ok=emit("[IMAGE_PROTECTION][BEGIN] schema=1 collection=%llu root=0x%llx nx=%u\n",U64(g_collection),U64(g_expected_root),g_nx);
    image_walk_result_t snapshot={0};
    ok=paging_image_snapshot_owners(&snapshot,leaf,stack_owner,NULL) && ok;
    const image_permissions_t *plan=paging_image_plan();
    for (unsigned i=0;ok && i<IMAGE_ROLE_COUNT;++i) {
        const image_section_t *s=&plan->input.sections[i];
        ok=emit("[IMAGE_PROTECTION][SECTION] role=%u first=0x%llx end=0x%llx\n",i,U64(s->first),U64(s->end));
    }
    tlb_interval_t interval={0};cpu_request_result_t result={.status=CPU_REQUEST_INVALID};
    if (ok) {
        __atomic_store_n(&g_active,1,__ATOMIC_RELEASE);
        result=cpu_request_run(CPU_REQUEST_PROTECTION,&interval);
    }
    unsigned expected=g_cpu_count==32 ? UINT32_MAX : (g_cpu_count && g_cpu_count<32 ? (1u<<g_cpu_count)-1 : 0);
    ok=ok && result.status==CPU_REQUEST_OK && result.requested==expected && expected &&
        result.acknowledged==expected && !result.missing && !result.failed && !result.send_failed;
    if (result.status!=CPU_REQUEST_OK) {
        /* Claimed late handlers retain every operand and result slot. */
        g_quarantined=true;
    } else {
        __atomic_store_n(&g_active,0,__ATOMIC_RELEASE);
        for (unsigned slot=0;ok && slot<g_cpu_count;++slot) {
            const protection_cpu_result_t *r=&g_results[slot];
            ok=r->generation==result.generation && !g_active_attempt[slot] &&
                r->completed==(g_nx?63u:15u) && r->unsupported==(g_nx?0u:48u) &&
                emit("[IMAGE_PROTECTION][CPU] collection=%llu generation=%llu slot=%u apic=%u cr0=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx extended_edx=0x%x completed=0x%x unsupported=0x%x disarmed=1\n",
                    U64(g_collection),U64(r->generation),slot,g_cpus[slot].apic_id,U64(r->cr0),U64(r->cr3),U64(r->cr4),U64(r->efer),r->extended_edx,r->completed,r->unsupported);
            for (unsigned i=0;ok && i<PROTECTION_ATTEMPTS;++i) {
                const protection_probe_state_t *p=&r->attempts[i];const protection_target_t *t=&g_targets[i];
                if (i>=4 && !g_nx) {
                    ok=!p->generation && !p->armed && !p->seen && emit("[IMAGE_PROTECTION][UNSUPPORTED] collection=%llu generation=%llu slot=%u attempt=%u reason=UNAVAILABLE_NX executed=0\n",
                        U64(g_collection),U64(r->generation),slot,i);continue;
                }
                ok=p->completed && p->seen && !p->armed && p->generation==r->generation && p->slot==slot && p->apic==g_cpus[slot].apic_id &&
                    emit("[IMAGE_PROTECTION][FAULT] collection=%llu generation=%llu slot=%u apic=%u attempt=%u kind=%u alias=%u root=0x%llx physical=0x%llx target=0x%llx rip=0x%llx cr2=0x%llx error=0x%llx cs=0x%llx recovery=0x%llx before=0x%x after=0x%x completed=1\n",
                        U64(g_collection),U64(p->generation),slot,p->apic,i,p->kind,p->alias,U64(p->root),U64(t->physical),U64(t->address),U64(p->fault_rip),U64(p->fault_address),U64(p->fault_error),U64(p->fault_cs),U64(p->recovery),r->before[i],r->after[i]);
            }
        }
    }
    ok=emit("[IMAGE_PROTECTION][SERVICE] collection=%llu generation=%llu requested=0x%x acknowledged=0x%x missing=0x%x failed=0x%x notifications=%u status=%u\n",
        U64(g_collection),U64(result.generation),result.requested,result.acknowledged,result.missing,result.failed,result.notifications,result.status) && ok;
    ok=emit("[IMAGE_PROTECTION][SCAN] root=0x%llx tables=%llu leaves=%llu bytes=%llu image_bytes=%llu direct_bytes=%llu managed_bytes=%llu scratch_bytes=%llu stack_bytes=%llu rwx_bytes=%llu nx=%u complete=%u\n",
        U64(snapshot.root),U64(snapshot.tables),U64(snapshot.leaves),U64(snapshot.bytes),U64(snapshot.image_bytes),U64(snapshot.direct_bytes),
        U64(snapshot.managed_bytes),U64(snapshot.scratch_bytes),U64(snapshot.stack_bytes),U64(snapshot.rwx_bytes),snapshot.nx,snapshot.complete) && ok;
    if (ok) ok=paging_image_boot_check("WORKLOAD");
    uint64_t records=g_records,bytes=g_bytes,digest=g_digest;
    ok=emit("[IMAGE_PROTECTION][END] collection=%llu records=%llu bytes=%llu digest=0x%llx status=%s\n",
        U64(g_collection),U64(records),U64(bytes),U64(digest),ok?"PASS":"FAIL") && ok;
    __atomic_store_n(&g_owner,0,__ATOMIC_RELEASE);return ok?0:1;
}
