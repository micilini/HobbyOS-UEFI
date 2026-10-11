#include "null_probe.h"
#include "paging.h"
#include "bootstrap_memory.h"
#include "../core/scheduler.h"
#include "../drivers/serial.h"
#include "../libc/string.h"

extern uint64_t null_probe_read(void), null_destructive_read(void);
extern char null_probe_fault[], null_probe_recovery[], null_destructive_fault[];
static null_probe_state_t g_null_probe;
static uint64_t g_null_generation;
static uint32_t g_null_owner;

bool null_probe_fixup(InterruptFrame *frame, uint64_t address, uint64_t error)
{
    cpu_slot_t slot;
    if (!smp_current_cpu_slot(&slot) || !frame ||
        !null_probe_match(&g_null_probe,frame->rip,address,error,frame->cs,slot,g_null_generation)) return false;
    g_null_probe.fault_rip=frame->rip; g_null_probe.fault_address=address; g_null_probe.fault_error=error;
    g_null_probe.seen=true; g_null_probe.armed=false;
    frame->rip=g_null_probe.recovery;
    return true;
}

int null_probe_run(void)
{
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&g_null_owner,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    bool ok=paging_low_half_empty();
    const bootstrap_lifetime_t *life=paging_bootstrap_lifetime();
    ok=ok && (life->state==BOOTSTRAP_RETIRED || life->state==BOOTSTRAP_NOT_NEEDED);
    for (unsigned i=0; ok && life->state==BOOTSTRAP_RETIRED && i<4; ++i) {
        uint64_t physical,hash;
        ok=paging_bootstrap_snapshot_entry(i,&physical,&hash);
        if (ok) {
            char line[192];
            int n=ksnprintf(line,sizeof(line),
                "[LOW_ROOT][BOOT_SNAPSHOT] generation=%llu index=%u physical=%016llx hash=%016llx capture=BEFORE_AP_LAUNCH\n",
                (unsigned long long)life->generation,i,(unsigned long long)physical,(unsigned long long)hash);
            if (n>0 && (size_t)n<sizeof(line)) serial_write_all(line); else ok=false;
        }
    }
    scheduler_cpu_pin_t pin={0};
    if (!ok || g_null_generation==UINT64_MAX || !scheduler_cpu_pin(&pin)) {
        __atomic_store_n(&g_null_owner,0,__ATOMIC_RELEASE); return 1;
    }
    uint64_t root;
    __asm__ volatile("mov %%cr3,%0" : "=r"(root));
    cpu_slot_t slot;
    ok=smp_current_cpu_slot(&slot) && root==paging_kernel_root_physical();
    if (ok) {
        ++g_null_generation;
        g_null_probe=(null_probe_state_t){.generation=g_null_generation,.site=(uintptr_t)null_probe_fault,
            .recovery=(uintptr_t)null_probe_recovery,.slot=slot,.armed=true};
        __asm__ volatile("" ::: "memory");
        uint64_t value=null_probe_read();
        __asm__ volatile("" ::: "memory");
        ok=g_null_probe.seen && !g_null_probe.armed && !value &&
            g_null_probe.fault_rip==(uintptr_t)null_probe_fault && !g_null_probe.fault_address && !g_null_probe.fault_error;
        g_null_probe.armed=false;
    }
    scheduler_cpu_unpin(&pin);
    char line[384];
    int n=ksnprintf(line,sizeof(line),
        "[LOW_ROOT][NULL] generation=%llu slot=%u root=%016llx rip=%016llx cr2=%016llx error=%llx recovery=%016llx fault=%u status=%s\n",
        (unsigned long long)g_null_probe.generation,g_null_probe.slot,(unsigned long long)root,
        (unsigned long long)g_null_probe.fault_rip,(unsigned long long)g_null_probe.fault_address,
        (unsigned long long)g_null_probe.fault_error,(unsigned long long)g_null_probe.recovery,g_null_probe.seen,ok?"PASS":"FAIL");
    if (n>0 && (size_t)n<sizeof(line)) serial_write_all(line); else ok=false;
    __atomic_store_n(&g_null_owner,0,__ATOMIC_RELEASE);
    return ok?0:1;
}

int null_probe_destructive(void)
{
    uint32_t expected=0;
    if (!paging_low_half_empty() || !__atomic_compare_exchange_n(&g_null_owner,&expected,1,false,
        __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    char line[160];
    int n=ksnprintf(line,sizeof(line),"[LOW_ROOT][DESTRUCTIVE] site=%016llx cr2=0 error=0 armed=0\n",
        (unsigned long long)(uintptr_t)null_destructive_fault);
    if (n>0 && (size_t)n<sizeof(line)) serial_write_all(line);
    (void)null_destructive_read();
    __atomic_store_n(&g_null_owner,0,__ATOMIC_RELEASE);
    return 1;
}
