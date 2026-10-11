#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "../kernel/src/memory/warning_report.c"
static bool enabled=true, scoped;
static uint64_t tick;
static unsigned emitted, markers;
static bool parallel;
static bool seen[8][8];
static pthread_barrier_t barrier;
bool irq_is_enabled(void) { return enabled; }
bool irq_latency_memory_scope_active(void) { return scoped; }
uint64_t timer_get_uptime_ms(void) { return tick; }
void timer_sleep(uint64_t value) { tick+=value; }
void serial_write_all(const char *line)
{
    assert(!scoped && enabled);
    if (parallel) {
        unsigned producer,index;assert(sscanf(line,"proof p=%u n=%u",&producer,&index)==2 && producer<8 && index<8);
        assert(!seen[producer][index]);seen[producer][index]=true;
    } else assert(!strcmp(line,"proof\n"));
    ++emitted;
}
bool stack_test_emit(const char *fmt,...)
{ assert(fmt && !scoped && enabled); ++markers; return true; }
uint32_t tasktest_transport_crc32(const char *data,uint32_t size)
{ uint32_t crc=~0u;for(uint32_t n=0;n<size;++n){crc^=(unsigned char)data[n];for(unsigned b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}return ~crc; }
static void reset(void)
{
    memset(g_warning_reports,0,sizeof(g_warning_reports));
    g_warning_report_next=g_warning_report_read=g_warning_report_failed=0;g_warning_report_consumer=0;
    tick=emitted=markers=0;enabled=true;scoped=false;
}
static void *producer(void *argument)
{
    int result=pthread_barrier_wait(&barrier);assert(!result || result==PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned n=0;n<8;++n) {
        char line[48];int bytes=snprintf(line,sizeof(line),"proof p=%u n=%u\n",(unsigned)(uintptr_t)argument,n);
        assert(bytes>0 && warning_report_enqueue(line,(size_t)bytes)); memset(line,'x',sizeof(line));
    }
    return NULL;
}
int main(void)
{
    reset(); assert(warning_report_quiet() && warning_report_flush()); assert(!markers);
    assert(warning_report_enqueue("proof\n",6)); assert(!emitted && !warning_report_quiet());
    scoped=true; assert(!warning_report_flush()); scoped=false; enabled=false; assert(!warning_report_flush()); enabled=true;
    assert(warning_report_flush() && emitted==1 && markers==3 && warning_report_quiet());
    for (unsigned cycle=0;cycle<3;++cycle) {
        for (unsigned n=0;n<WARNING_REPORT_CAPACITY;++n) assert(warning_report_enqueue("proof\n",6));
        assert(warning_report_flush() && warning_report_quiet());
    }
    reset(); for (unsigned n=0;n<WARNING_REPORT_CAPACITY;++n) assert(warning_report_enqueue("proof\n",6));
    assert(!warning_report_enqueue("proof\n",6) && g_warning_reports[0].ready==1);
    assert(!warning_report_flush() && !warning_report_quiet() && !emitted);
    reset();g_warning_report_next=1; assert(!warning_report_flush() && tick==5000 && !emitted);
    reset();assert(warning_report_enqueue("proof\n",6));g_warning_reports[0].ready=2;assert(!warning_report_flush());
    reset();assert(warning_report_enqueue("proof\n",6));g_warning_reports[0].line[6]='x';assert(!warning_report_flush());
    reset();g_warning_report_next=g_warning_report_read=(UINT64_C(1)<<63);
    assert(!warning_report_enqueue("proof\n",6) && g_warning_report_next==(UINT64_C(1)<<63));
    reset();g_warning_report_next=g_warning_report_read=UINT64_MAX;assert(!warning_report_enqueue("proof\n",6));
    reset();parallel=true;pthread_t threads[8];assert(!pthread_barrier_init(&barrier,NULL,8));
    for(uintptr_t n=0;n<8;++n)assert(!pthread_create(&threads[n],NULL,producer,(void *)n));
    for(unsigned n=0;n<8;++n)assert(!pthread_join(threads[n],NULL));
    assert(warning_report_flush() && emitted==64 && warning_report_quiet());assert(!pthread_barrier_destroy(&barrier));
    puts("Memory warning actual-C bounded publication and drain: PASS");
}
