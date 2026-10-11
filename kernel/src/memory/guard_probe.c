#include "guard_probe.h"
#include "kernel_stack.h"
#include "paging_private.h"
#include "../core/scheduler.h"
#include "../smp/smp_topology.h"
#include "../drivers/serial.h"
#include "../libc/string.h"

extern unsigned guard_probe_read(uint64_t address);
extern uint64_t guard_payload_read(uint64_t address);
extern char guard_probe_site[],guard_probe_recovery[];
static guard_probe_state_t g_guard_probe;
static uint64_t g_guard_generation;
static uint32_t g_guard_owner;

bool guard_probe_fixup(InterruptFrame *frame,uint64_t address,uint64_t error)
{
    cpu_slot_t cpu;uint64_t root;stack_owner_view_t owner;
    __asm__ volatile("mov %%cr3,%0" : "=r"(root));
    if (!frame || !smp_current_cpu_slot(&cpu) || cpu>=g_cpu_count ||
        !kernel_stack_guard_owner(address,&owner) ||
        !guard_probe_match(&g_guard_probe,&owner,g_guard_generation,root,cpu,g_cpus[cpu].apic_id,
            frame->rip,address,error,frame->cs,(uintptr_t)guard_probe_site,(uintptr_t)guard_probe_recovery)) return false;
    g_guard_probe.fault_rip=frame->rip;g_guard_probe.fault_address=address;g_guard_probe.fault_error=error;
    g_guard_probe.seen=true;g_guard_probe.armed=false;frame->rip=(uintptr_t)guard_probe_recovery;
    return true;
}

bool guard_probe_owner(const stack_owner_view_t *expected)
{
    uint32_t free=0;
    if (!expected || expected->state!=STACK_LIVE ||
        !__atomic_compare_exchange_n(&g_guard_owner,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return false;
    uint64_t physical;
    bool ok=!paging_query_range(expected->guard,4096,&physical) &&
        paging_query_range(expected->base,expected->capacity,&physical) && physical==expected->physical;
    for (unsigned edge=0;ok && edge<2;++edge) {
        scheduler_cpu_pin_t pin={0};
        if (g_guard_generation==UINT64_MAX || !scheduler_cpu_pin(&pin)) {ok=false;break;}
        cpu_slot_t cpu;uint64_t root;stack_owner_view_t owner;
        __asm__ volatile("mov %%cr3,%0" : "=r"(root));
        uint64_t address=edge ? expected->base-1 : expected->guard;
        ok=smp_current_cpu_slot(&cpu) && cpu<g_cpu_count && root==paging_kernel_root_physical() &&
            kernel_stack_guard_owner(address,&owner) && owner.generation==expected->generation &&
            owner.slot==expected->slot && owner.state==STACK_LIVE;
        if (ok) {
            g_guard_probe=(guard_probe_state_t){.generation=++g_guard_generation,.root=root,.address=address,
                .owner_generation=owner.generation,.owner_slot=owner.slot,.cpu=cpu,.apic=g_cpus[cpu].apic_id,
                .site=(uintptr_t)guard_probe_site,.recovery=(uintptr_t)guard_probe_recovery,.armed=true};
            __asm__ volatile("" ::: "memory");
            unsigned denied=guard_probe_read(address);
            __asm__ volatile("" ::: "memory");
            ok=denied==1 && g_guard_probe.seen && !g_guard_probe.armed &&
                g_guard_probe.fault_rip==(uintptr_t)guard_probe_site &&
                g_guard_probe.fault_address==address && !g_guard_probe.fault_error;
            g_guard_probe.armed=false;
        }
        scheduler_cpu_unpin(&pin);
        bool printed=stack_test_emit(
            "[STACK_GUARD][FAULT] generation=%llu actor_cpu=%u actor_apic=%u owner_slot=%u owner_generation=%llu owner_kind=%u owner_cpu=%u root=%016llx rip=%016llx cr2=%016llx error=%llx recovery=%016llx seen=%u status=%s\n",
            (unsigned long long)g_guard_probe.generation,g_guard_probe.cpu,g_guard_probe.apic,expected->slot,
            (unsigned long long)expected->generation,expected->kind,expected->cpu,(unsigned long long)g_guard_probe.root,
            (unsigned long long)g_guard_probe.fault_rip,(unsigned long long)g_guard_probe.fault_address,
            (unsigned long long)g_guard_probe.fault_error,(unsigned long long)g_guard_probe.recovery,
            g_guard_probe.seen,ok ? "PASS" : "FAIL");
        if (!printed) ok=false;
    }
    if (ok) {
        (void)guard_payload_read(expected->base);(void)guard_payload_read(expected->end-1);
    }
    __atomic_store_n(&g_guard_owner,0,__ATOMIC_RELEASE);return ok;
}
