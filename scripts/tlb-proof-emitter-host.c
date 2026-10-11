#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#define IO_H
static void outb(uint16_t, uint8_t);
#if !defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) || !HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
static uint8_t inb(uint16_t port) { (void)port; return 0xe9; }
#endif
#include "../kernel/src/memory/tlb_fixture.c"

static _Thread_local bool enabled=true;
static _Thread_local bool held;
static bool exporting;
static size_t ports, serials, wire_length, sidebands;
static char wire_line[768];
static uint64_t port_digest=TLB_PROOF_FNV_OFFSET;
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char storage[8*1024*1024];
bool irq_is_enabled(void) { return enabled; }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock==&g_trace_lock && !held);
    irq_flags_t flags=enabled ? 512 : 0;enabled=false;
    assert(!pthread_mutex_lock(&host_lock));held=true;
    return flags;
}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t flags)
{
    assert(lock==&g_trace_lock && held && !enabled);
    held=false;assert(!pthread_mutex_unlock(&host_lock));enabled=(flags&512)!=0;
}
static void outb(uint16_t port,uint8_t byte)
{
    assert(port==TLB_TRACE_PORT);
    if (exporting) assert(enabled && !held);
    else assert(!enabled && held && !g_proof_deferred);
    assert(wire_length<sizeof(wire_line)-1);wire_line[wire_length++]=(char)byte;
    if (byte=='\n') {
        wire_line[wire_length]=0;
        if (!strncmp(wire_line,"[TLB_PROOF]",11)) {assert(exporting);++sidebands;}
        else {
            assert(!strncmp(wire_line,"[TLB]",5));
            for(size_t i=0;i<wire_length;++i){++ports;port_digest=(port_digest^(unsigned char)wire_line[i])*TLB_PROOF_FNV_PRIME;}
        }
        wire_length=0;
    }
}

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
static uint64_t wait_time;static bool wait_valid=true;
bool clock_monotonic_read_ns(uint64_t *out) {if(!wait_valid)return false;*out=wait_time;return true;}
bool serial_proof_begin(uint64_t session)
{ assert(session==1234 && enabled && !held);return true; }
bool serial_proof_write(uint64_t session,const void *data,size_t bytes)
{
    assert(session==1234 && enabled && !held);
    /* The UART implementation has a separate actual-function test. */
    if(bytes>1 && !memcmp(data,"\n[SERIAL_PROOF]",14)) return true;
    for(size_t i=0;i<bytes;++i)outb(TLB_TRACE_PORT,((const unsigned char *)data)[i]);
    return true;
}
bool serial_proof_end(uint64_t session)
{ assert(session==1234 && enabled && !held);return true; }
bool serial_proof_flush(uint64_t session)
{ assert(session==1234 && enabled && !held && !wire_length);return true; }
#endif

void serial_write_all(const char *line)
{
    assert(line && !held);
    assert(!exporting);
    ++serials;
}
bool cpu_request_context_allowed(void) { return enabled && !held; }
bool cpu_request_audit_snapshot(uint64_t *admitted,uint64_t *completed)
{ *admitted=*completed=71;return true; }
int kvsnprintf(char *out,size_t bytes,const char *fmt,va_list ap)
{ return vsnprintf(out,bytes,fmt,ap); }
int ksnprintf(char *out,size_t bytes,const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int n=vsnprintf(out,bytes,fmt,ap);va_end(ap);return n; }
uint32_t tasktest_transport_crc32(const char *data,uint32_t bytes)
{
    uint32_t crc=~0u;
    for (uint32_t i=0;i<bytes;++i) {
        crc^=(unsigned char)data[i];
        for(unsigned j=0;j<8;++j) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
static void reset(bool deferred,size_t capacity)
{
    memset(&g_proof,0,sizeof(g_proof));memset(storage,0xa5,sizeof(storage));
    assert(tlb_proof_open(&g_proof,storage,capacity,1234));
    g_proof_deferred=deferred;g_proof_invocations=g_proof_completed=1;
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    g_proof_ended=1;g_writers_retired=true;
#endif
    g_trace_bytes=g_trace_records=g_proof_port_bytes=ports=serials=wire_length=sidebands=0;
    g_trace_digest=port_digest=TLB_PROOF_FNV_OFFSET;
    g_owner=g_quarantined=0;enabled=true;exporting=false;
}
static void export_checked(void)
{
    uint64_t bytes=g_trace_bytes,records=g_trace_records,digest=g_trace_digest;
    enabled=false;assert(!tlb_fixture_proof_export(71) && !enabled);enabled=true;
    g_owner=1;assert(!tlb_fixture_proof_export(71));g_owner=0;
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    g_proof_ended=0;assert(!tlb_fixture_proof_export(71));g_proof_ended=1;
    g_writers_retired=false;assert(!tlb_fixture_proof_export(71));g_writers_retired=true;
#else
    g_proof_completed=0;assert(!tlb_fixture_proof_export(71));g_proof_completed=1;
#endif
    assert(!tlb_fixture_proof_export(70));
    exporting=true;assert(tlb_fixture_proof_export(71));
    assert(enabled && !held && ports==bytes && port_digest==digest && sidebands==2 && !wire_length);
    assert(g_trace_bytes==bytes && g_trace_records==records && g_trace_digest==digest);
    assert(!tlb_fixture_proof_export(71) && ports==bytes);
}
static void *producer(void *arg)
{
    for(unsigned i=0;i<100;++i) assert(record("[TLB][HOSTED] writer=%u n=%u\n",(unsigned)(uintptr_t)arg,i));
    return NULL;
}
int main(int argc,char **argv)
{
#if !defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) || !HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    reset(false,sizeof(storage));
    enabled=false;assert(record("[TLB][HOSTED] value=%u\n",3u) && !enabled);
    enabled=true;assert(g_trace_records==1 && ports==g_trace_bytes);
    export_checked();
#endif
    reset(true,sizeof(storage));
    enabled=false;assert(record("[TLB][HOSTED] value=%u\n",3u) && !enabled && !ports);
    enabled=true;assert(strstr((char *)storage," crc="));export_checked();
    reset(true,5);assert(!record("[TLB][HOSTED] value=4\n"));
    assert(g_proof.failed && !ports && !g_proof.bytes && !g_trace_bytes && !tlb_fixture_proof_export(71));
    reset(true,sizeof(storage));pthread_t tasks[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&tasks[i],NULL,producer,(void *)(uintptr_t)i));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(tasks[i],NULL));
    assert(g_trace_records==800 && !ports);export_checked();
    reset(true,sizeof(storage));
    assert(emit_line("\n[OTHER] proof\n") && serials==1 && !g_trace_records && !ports);
    char line[768];memset(line,'x',sizeof(line));memcpy(line,"\n[TLB]",6);line[766]='\n';line[767]=0;
    assert(emit_line(line) && g_trace_bytes==766);export_checked();
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
    reset(true,sizeof(storage));
    assert(record("[TLB][END] invocation=1 status=FAIL retained=1\n"));
    g_quarantined=true;g_proof_completed=0;
    export_checked();assert(g_quarantined && !g_proof_completed);
    /* Unretired writers cannot publish a failure seal either. */
    reset(true,sizeof(storage));assert(record("[TLB][END] status=FAIL\n"));
    g_quarantined=true;g_writers_retired=false;
    assert(!tlb_fixture_proof_export(71) && !ports && g_proof.state==TLB_PROOF_CAPTURING);
    assert(cycle_failure(1,272,2,"invalidate_b")==false);
    assert(g_cycle_failure[1].phase==2 && g_cycle_failure[1].round==272);
    g_failure_stage=NULL;assert(!fixture_check(false,"first"));assert(!fixture_check(false,"second"));
    assert(!strcmp(g_failure_stage,"first"));
    fixture_wait_clock_t clock={0};wait_time=1;
    assert(fixture_wait_before(10,&clock));wait_time=9;assert(fixture_wait_before(10,&clock));
    wait_time=10;assert(!fixture_wait_before(10,&clock));
    wait_time=8;assert(!fixture_wait_before(10,&clock));
    wait_time=9;wait_valid=false;assert(!fixture_wait_before(10,&clock));wait_valid=true;
    clock=(fixture_wait_clock_t){.last=9};
    for(unsigned i=0;i<1023;++i)assert(fixture_wait_before(10,&clock));
    assert(!fixture_wait_before(10,&clock));
    wait_time=10;assert(fixture_wait_before(11,&clock) && !clock.stalled);
#endif
    if(argc==2) {
        reset(true,sizeof(storage));FILE *input=fopen(argv[1],"rb");assert(input);
        size_t total=0,records=0;line[0]='\n';
        while(fgets(line+1,sizeof(line)-1,input)) {
            size_t n=strlen(line+1);assert(n && line[n]=='\n' && !strncmp(line+1,"[TLB]",5));
            assert(emit_line(line));total+=n;++records;
        }
        assert(!ferror(input) && !fclose(input));
        assert(g_trace_bytes==total && g_trace_records==records && !ports);
        export_checked();
        printf("RAW_RECONSTRUCTION bytes=%zu records=%zu fnv1a=%016llx\n",total,records,(unsigned long long)g_trace_digest);
    }
    puts("TLB proof actual emitter/export: PASS capture_port_bytes=0 flags=EXACT export_IF=1");
}
