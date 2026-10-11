/* Actual activation formatter, including its real bounded serial envelope. */
#include "../kernel/src/memory/irq_latency_test.c"
#include "../kernel/src/memory/stack_test.c"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

uint32_t g_cpu_count;
static unsigned printed;
static char captured[768], activation[768];
static clocksource_terminal_t terminal;
const clocksource_terminal_t *clocksource_terminal_read(cpu_slot_t cpu)
{assert(cpu<g_cpu_count);return &terminal;}
bool irq_latency_memory_scope_active(void) { return false; }
bool stack_report_enqueue(const char *line,size_t bytes)
{ (void)line;(void)bytes;assert(false);return false; }
void serial_write_all(const char *line)
{ assert(strlen(line)<sizeof(captured));strcpy(captured,line);if(strstr(line,"[BEGIN]"))strcpy(activation,line);++printed; }
int kvsnprintf(char *out,size_t size,const char *fmt,va_list ap)
{ return vsnprintf(out,size,fmt,ap); }
int ksnprintf(char *out,size_t size,const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int n=vsnprintf(out,size,fmt,ap);va_end(ap);return n; }
uint32_t tasktest_transport_crc32(const char *data,uint32_t size)
{ uint32_t c=~0u;for(uint32_t i=0;i<size;++i){c^=(unsigned char)data[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c; }
int main(void)
{
    const unsigned cpus[]={1,8,24,32};
    for(unsigned i=0;i<4;++i)for(unsigned valid=0;valid<2;++valid){
        g_cpu_count=cpus[i];unsigned before=printed;
        assert(activation_report(valid) && printed==before+2);
        assert(strstr(activation," proof_output=2 ") && strstr(activation," crc="));
        assert(strstr(captured,"[STACK_COVERAGE_METHOD] version=1 ") && strstr(captured," epoch=1 crc="));
        assert(strstr(activation,valid ? "status=ACTIVE" : "status=UNAVAILABLE_TIMEBASE"));
    }
    terminal=(clocksource_terminal_t){.source=2,.epoch=2,.registered=1,
        .reads=UINT64_MAX,.references=UINT64_MAX,.maximum_reference_ns=100000,
        .last_tsc=UINT64_MAX,.last_ns=INT64_MAX};
    terminal.maximum=(pvclock_reference_t){.ticks=UINT64_MAX,.ns=INT64_MAX,
        .period_fs=100000000,.mode=1,.retries=4,.valid=true};
    terminal.maximum.before=(pvclock_sample_t){.tsc=UINT64_MAX,.ns=INT64_MAX,
        .version=UINT32_MAX-1,.multiplier=UINT32_MAX,.shift=-63,.flags=1,.retries=3};
    terminal.maximum.after=terminal.maximum.before;
    unsigned records=printed;
    terminal.previous=terminal.maximum;
    assert(clocksource_terminal_report(31) && printed==records+3);
    assert(strstr(captured,"[CLOCKSOURCE_REFERENCE]") && strstr(captured," crc="));
    terminal.source=3;assert(!clocksource_terminal_report(31));
    terminal.source=2;terminal.busy=1;assert(!clocksource_terminal_report(31));
    unsigned before=printed;char excess[756];memset(excess,'x',sizeof(excess));
    excess[sizeof(excess)-2]='\n';excess[sizeof(excess)-1]=0;
    assert(!stack_test_emit("%s",excess) && printed==before);
    puts("IRQ activation actual formatter: PASS bounded_record=CHECKED refusal=RETAINED");
}
