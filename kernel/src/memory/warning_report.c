#include "warning_report.h"
#include "../core/idt.h"
#include "../core/irq_latency.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "guard_probe.h"
#include "../shell/commands/cmd_tasktest.h"

#define WARNING_REPORT_CAPACITY 64u
#define WARNING_REPORT_BYTES 1024u
typedef struct { uint64_t ready; size_t bytes; char line[WARNING_REPORT_BYTES]; } warning_report_t;
static warning_report_t g_warning_reports[WARNING_REPORT_CAPACITY];
static uint64_t g_warning_report_next, g_warning_report_read, g_warning_report_failed;
static uint32_t g_warning_report_consumer;

bool warning_report_enqueue(const char *line, size_t bytes)
{
    if (!line || !bytes || bytes >= WARNING_REPORT_BYTES || line[bytes] ||
        __atomic_load_n(&g_warning_report_failed,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_warning_report_failed,1,__ATOMIC_RELEASE);return false;
    }
    /* One reservation per caller; contention cannot exhaust a retry allowance.
     * Stop in the lower half of the counter before any bounded set of in-flight
     * stack owners could approach wrap. Capacity is checked before slot access. */
    if (__atomic_load_n(&g_warning_report_next,__ATOMIC_RELAXED) >= (UINT64_C(1)<<63)) {
        __atomic_store_n(&g_warning_report_failed,1,__ATOMIC_RELEASE);return false;
    }
    uint64_t sequence = __atomic_fetch_add(&g_warning_report_next,1,__ATOMIC_ACQ_REL);
    uint64_t consumed = __atomic_load_n(&g_warning_report_read,__ATOMIC_ACQUIRE);
    if (sequence >= (UINT64_C(1)<<63) || sequence < consumed || sequence-consumed >= WARNING_REPORT_CAPACITY) {
        __atomic_store_n(&g_warning_report_failed,1,__ATOMIC_RELEASE);return false;
    }
    warning_report_t *record = &g_warning_reports[sequence % WARNING_REPORT_CAPACITY];
    if (__atomic_load_n(&record->ready,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_warning_report_failed,1,__ATOMIC_RELEASE); return false;
    }
    memcpy(record->line,line,bytes+1); record->bytes=bytes;
    __atomic_store_n(&record->ready,sequence+1,__ATOMIC_RELEASE);
    return true;
}

bool warning_report_quiet(void)
{
    return !__atomic_load_n(&g_warning_report_failed,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&g_warning_report_read,__ATOMIC_ACQUIRE) ==
        __atomic_load_n(&g_warning_report_next,__ATOMIC_ACQUIRE);
}

bool warning_report_checkpoint(const char *role)
{
    uint64_t admitted=__atomic_load_n(&g_warning_report_next,__ATOMIC_ACQUIRE);
    uint64_t consumed=__atomic_load_n(&g_warning_report_read,__ATOMIC_ACQUIRE);
    return admitted==consumed && !__atomic_load_n(&g_warning_report_failed,__ATOMIC_ACQUIRE) &&
        stack_test_emit("[IRQ_LATENCY][WARNING_REPORT_CUTOFF] role=%s admitted=%llu consumed=%llu complete=1\n",
            role,(unsigned long long)admitted,(unsigned long long)consumed);
}

bool warning_report_flush(void)
{
    if (!irq_is_enabled() || irq_latency_memory_scope_active()) return false;
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&g_warning_report_consumer,&expected,1,false,
            __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return false;
    uint64_t first=__atomic_load_n(&g_warning_report_read,__ATOMIC_RELAXED);
    uint64_t deadline=timer_get_uptime_ms()+UINT64_C(5000), consumed=first;
    uint64_t limit=__atomic_load_n(&g_warning_report_next,__ATOMIC_ACQUIRE);
    bool valid=first==limit || stack_test_emit("[IRQ_LATENCY][WARNING_REPORT_BEGIN] first=%llu limit=%llu\n",
        (unsigned long long)first,(unsigned long long)limit);
    for (;;) {
        if (!valid || __atomic_load_n(&g_warning_report_failed,__ATOMIC_ACQUIRE) || consumed>limit) { valid=false; break; }
        if (consumed==limit) break;
        warning_report_t *record=&g_warning_reports[consumed % WARNING_REPORT_CAPACITY];
        uint64_t ready=__atomic_load_n(&record->ready,__ATOMIC_ACQUIRE);
        if (!ready) {
            if (timer_get_uptime_ms()>=deadline) { valid=false; break; }
            timer_sleep(1); continue;
        }
        if (ready!=consumed+1 || !record->bytes || record->bytes>=WARNING_REPORT_BYTES || record->line[record->bytes]) {
            valid=false; break;
        }
        if (record->line[record->bytes-1]!='\n' ||
            !stack_test_emit("[IRQ_LATENCY][WARNING_REPORT_ITEM] sequence=%llu bytes=%llu payload_crc=%08x\n",
                (unsigned long long)consumed,(unsigned long long)(record->bytes-1),
                tasktest_transport_crc32(record->line,(uint32_t)record->bytes-1))) { valid=false;break; }
        serial_write_all(record->line);
        __atomic_store_n(&record->ready,0,__ATOMIC_RELEASE);
        __atomic_store_n(&g_warning_report_read,++consumed,__ATOMIC_RELEASE);
    }
    if (!valid) __atomic_store_n(&g_warning_report_failed,1,__ATOMIC_RELEASE);
    if (consumed!=first || !valid)
        valid=stack_test_emit("[IRQ_LATENCY][WARNING_REPORT_END] first=%llu after=%llu admitted=%llu status=%s\n",
            (unsigned long long)first,(unsigned long long)consumed,
            (unsigned long long)__atomic_load_n(&g_warning_report_next,__ATOMIC_ACQUIRE),valid ? "COMPLETE" : "INCOMPLETE") && valid;
    __atomic_store_n(&g_warning_report_consumer,0,__ATOMIC_RELEASE);
    return valid;
}
