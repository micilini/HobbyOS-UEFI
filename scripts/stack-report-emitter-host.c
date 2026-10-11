#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifndef STACK_TEST_SOURCE
#define STACK_TEST_SOURCE "../kernel/src/memory/stack_test.c"
#endif
#include STACK_TEST_SOURCE
static unsigned queued, printed;
static bool scoped=true;
static bool reporting;
static char retained[768];
bool irq_latency_memory_scope_active(void) { return scoped; }
bool irq_latency_test_reporting(void) { return reporting; }
bool stack_report_enqueue(const char *line,size_t bytes)
{ assert(bytes==strlen(line) && bytes<sizeof(retained)); memcpy(retained,line,bytes+1); ++queued; return true; }
void serial_write_all(const char *line) { assert(line); ++printed; }
int kvsnprintf(char *out,size_t size,const char *fmt,va_list ap) { return vsnprintf(out,size,fmt,ap); }
int ksnprintf(char *out,size_t size,const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int n=vsnprintf(out,size,fmt,ap);va_end(ap);return n; }
uint32_t tasktest_transport_crc32(const char *data,uint32_t size)
{ uint32_t crc=~0u;for(uint32_t n=0;n<size;++n){crc^=(unsigned char)data[n];for(unsigned b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}return ~crc; }
int main(void)
{
    assert(stack_test_emit("[STACK_GUARD][ALLOC] slot=%u generation=%u\n",3u,7u));
    assert(queued==1 && !printed && strstr(retained," crc="));
    assert(stack_test_emit("[STACK_GUARD][RETIRE] slot=%u generation=%u\n",3u,7u));assert(queued==2 && !printed);
    assert(stack_test_emit("[STACK_GUARD][SCAN] complete=1\n"));assert(printed==1);
    scoped=false;assert(stack_test_emit("[STACK_GUARD][ALLOC] slot=3 generation=8\n"));assert(printed==2 && queued==2);
    reporting=true;
    assert(stack_test_emit("[STACK_GUARD][ALLOC] slot=3 generation=9\n") && queued==3 && printed==2);
    assert(stack_test_emit("[STACK_GUARD][RETIRE] slot=3 generation=9\n") && queued==4 && printed==2);
    assert(stack_test_emit("[IRQ_LATENCY][SNAPSHOT_READY] epoch=1\n") && printed==3);
    reporting=false;
    puts("Stack proof actual-C emitter defers exact lifetime rows: PASS");
}
