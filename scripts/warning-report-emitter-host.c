#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/core/panic.c"
static unsigned queued, printed;
static bool scoped=true;
static bool reporting;
static uint64_t host_flags=512;
static task_t host_task;
static char retained[1024];
SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
uint32_t g_cpu_count;
bool irq_latency_memory_scope_active(void) { return scoped; }
bool irq_latency_test_reporting(void) { return reporting; }
bool warning_report_enqueue(const char *line,size_t bytes)
{ assert(bytes==strlen(line) && bytes<sizeof(retained));memcpy(retained,line,bytes+1);++queued;return true; }
irq_flags_t irq_save(void) { irq_flags_t flags=host_flags;host_flags=0;return flags; }
void irq_restore(irq_flags_t flags) { host_flags=flags; }
task_t *get_current_task(void) { return &host_task; }
void serial_write_all(const char *line) { assert(line);strcpy(retained,line);++printed; }
int ksnprintf(char *out,size_t size,const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int n=vsnprintf(out,size,fmt,ap);va_end(ap);return n; }
int main(void)
{
    host_task.id=12;host_task.lifecycle_generation=27;
    assertion_context_t context={.file="memory.c",.expression="overflow",.line=34};
    assertion_warn_report(&context);
    assert(queued==1 && !printed && host_flags==512);
    assert(strstr(retained,"file_hex=6d656d6f72792e63 file_bytes=8 file_valid=1") &&
           strstr(retained,"line=34 expression_hex=6f766572666c6f77") &&
           strstr(retained,"task_id=12 task_generation=27 task_valid=1 if_enabled=1"));
    scoped=false;assertion_warn_report(&context);assert(printed==1 && queued==1 && host_flags==512);
    scoped=true;host_flags=0;assertion_warn_report(&context);assert(queued==2 && printed==1 && !host_flags);
    assert(strstr(retained,"if_enabled=0"));
    scoped=false;reporting=true;assertion_warn_report(&context);
    assert(queued==3 && printed==1 && !host_flags);
    reporting=false;
    puts("Memory warning actual-C emitter preserves context and defers UART: PASS");
}
