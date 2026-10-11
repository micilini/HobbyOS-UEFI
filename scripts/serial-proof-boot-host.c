#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/core/runtime_ready.c"

static unsigned publications,blocker_records;
uint64_t clock_monotonic_ns(void) { return 123456; }
void selftest_last_summary(selftest_summary_t *out) { memset(out,0,sizeof(*out)); }
void serial_write_all(const char *line)
{
    if (strstr(line,"[BOOT][READY_")) {
        assert(strstr(line,"violations=3") || strstr(line,"active=2"));
        ++blocker_records;return;
    }
    assert(strstr(line,"[BOOT][TEST_READY]"));
    assert(!runtime_ready_boot_diagnostics_finished());
    ++publications;
}
int main(void)
{
    runtime_ready_init();assert(!runtime_ready_boot_diagnostics_finished());
    g_ready_state=RUNTIME_READY_READY;
    runtime_ready_mark_test_complete(false,true);
    assert(publications==1 && runtime_ready_boot_diagnostics_finished() && runtime_ready_is_test_ready());
    runtime_ready_init();assert(!runtime_ready_boot_diagnostics_finished());
    g_ready_state=RUNTIME_READY_FAILED;
    runtime_ready_mark_test_complete(false,false);
    assert(publications==2 && runtime_ready_boot_diagnostics_finished() && !runtime_ready_is_test_ready());
    runtime_ready_snapshot_t failed={.cpus_expected=24,.cpus_online=24,.violations=3,.hotplug_active=2};
    record_failed_readiness(&failed);assert(blocker_records==2);
    puts("Actual boot publication: PASS completion_after_output failed_boot_not_pass");
}
