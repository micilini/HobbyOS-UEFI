#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>
static void host_outb(uint16_t port,uint8_t byte);
static uint8_t host_inb(uint16_t port);
#include SERIAL_ACTUAL_SOURCE
volatile int g_panic_in_progress;
static _Thread_local bool enabled=true,held;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned char wire[8388608],decoded[2097152];
static size_t length,polls,delayed;
static size_t feed_remaining;
static bool enforce,inject;
static uint16_t broken;
bool irq_is_enabled(void) {return enabled;}
int ksnprintf(char *out,size_t size,const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(out,size,format,args);va_end(args);return n;}
void spinlock_init(spinlock_t *lock) {memset(lock,0,sizeof(*lock));}
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock==&g_serial_lock && !held);
    irq_flags_t flags=enabled?512:0;enabled=false;
    assert(!pthread_mutex_lock(&mutex));held=true;return flags;
}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t flags)
{
    assert(lock==&g_serial_lock && held && !enabled);
    held=false;assert(!pthread_mutex_unlock(&mutex));enabled=(flags&512)!=0;
}
static uint8_t host_inb(uint16_t port)
{
    if(enforce) assert(enabled && !held);
    ++polls;if (delayed) {--delayed;return 0;}return port==broken+5?0xff:0x20;
}
static void host_outb(uint16_t port,uint8_t byte)
{
    if(enforce) assert(enabled && !held);
    assert(port==0x3f8 || port==0x2f8);
    if(port==0x3f8) {assert(length<sizeof(wire));wire[length++]=byte;}
    if(inject) {inject=false;serial_write_all("interrupt\n");}
    if (feed_remaining && byte=='\n') {
        size_t count=feed_remaining>128 ? 128 : feed_remaining;
        feed_remaining-=count;
        for(size_t i=0;i<count;++i)serial_putc_all('x');
    }
}
static unsigned digit(unsigned char c)
{assert((c>='0' && c<='9') || (c>='a' && c<='f'));return c<='9'?c-'0':c-'a'+10;}
static size_t decoded_size(void)
{
    size_t at=0,used=0;uint64_t sequence=0;
    while(at<length) {
        if (length-at<13 || memcmp(wire+at,"[SERIAL_AUX] ",13)) {decoded[used++]=wire[at++];continue;}
        char line[448];size_t n=0;while(at+n<length && wire[at+n]!='\n')++n;
        assert(n<sizeof(line) && at+n<length);memcpy(line,wire+at,n);line[n]=0;
        unsigned long long build,seq,bytes,hash;char hex[257];int consumed=0;
        assert(sscanf(line,"[SERIAL_AUX] build=0x%llx seq=%llu bytes=%llu data=%256s fnv1a=0x%llx%n",
                      &build,&seq,&bytes,hex,&hash,&consumed)==5 && consumed==(int)n);
        assert(build==HOBBYOS_SERIAL_PROOF_ID && seq==++sequence && bytes>0 && bytes<=128 && strlen(hex)==2*bytes);
        uint64_t digest=UINT64_C(14695981039346656037);
        for(size_t i=0;i<bytes;++i) {unsigned char c=(unsigned char)(digit(hex[2*i])*16+digit(hex[2*i+1]));decoded[used++]=c;digest=(digest^c)*UINT64_C(1099511628211);}
        assert(digest==hash);at+=n+1;
    }
    assert(used<sizeof(decoded));return used;
}
static void reset(void)
{
    assert(!g_serial_proof_session);g_serial_count=1;g_serial_ports[0]=0x3f8;
    memset(g_serial_dead,0,sizeof(g_serial_dead));memset(g_serial_fail,0,sizeof(g_serial_fail));
    g_serial_inited=g_serial_lock_inited=1;g_panic_in_progress=0;
    g_panic_serial_poll_budget=PANIC_UART_TOTAL_POLL_BUDGET;
    enabled=true;enforce=true;length=polls=delayed=0;broken=0;inject=false;
    feed_remaining=0;g_serial_aux_sequence=g_serial_aux_bytes=0;
    g_serial_proof_failed_ports=0;
}
static void *writer(void *arg)
{
    (void)arg;for(unsigned i=0;i<100;++i)serial_write_all("parallel\n");return NULL;
}
int main(void)
{
    reset();assert(!serial_proof_begin(0));enabled=false;
    assert(!serial_proof_begin(7) && !enabled);enabled=true;
    assert(serial_proof_begin(7) && !serial_proof_begin(8));
    assert(!serial_proof_write(8,"x",1) && !serial_proof_end(8));
    assert(!serial_proof_write(7,NULL,1));
    enabled=false;assert(!serial_proof_write(7,"x",1) && !enabled);enabled=true;
    serial_write_all("ordinary\n");serial_write_hex64_all(0xabc);serial_putc_all('\n');
    assert(!length && !polls);inject=true;
    assert(serial_proof_write(7,"raw\n",4));assert(serial_proof_end(7));
    const char *expected="raw\nordinary\r\n0000000000000ABC\r\ninterrupt\r\n";
    assert(decoded_size()==strlen(expected) && !memcmp(decoded,expected,strlen(expected)));
    assert(!serial_proof_end(7) && !serial_proof_write(7,"x",1));
    reset();assert(serial_proof_begin(1));pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,writer,NULL));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(!length && !polls && serial_proof_end(1));
    assert(decoded_size()==800*10);for(size_t i=0;i<800*10;i+=10)assert(!memcmp(decoded+i,"parallel\r\n",10));
    reset();assert(serial_proof_begin(2));
    for(unsigned i=0;i<SERIAL_PROOF_PENDING_BYTES;++i)serial_putc_all('a');
    assert(!g_serial_proof_failed && serial_proof_end(2) && decoded_size()==SERIAL_PROOF_PENDING_BYTES);
    reset();assert(serial_proof_begin(3));
    for(unsigned i=0;i<SERIAL_PROOF_PENDING_BYTES-1;++i)serial_putc_all('a');
    serial_putc_all('\n');assert(g_serial_proof_failed && g_serial_proof_pending_length==SERIAL_PROOF_PENDING_BYTES-1);
    assert(!serial_proof_write(3,"footer",6) && !serial_proof_end(3));
    reset();g_serial_count=0;assert(!serial_proof_begin(4));g_serial_count=1;
    g_serial_dead[0]=1;assert(!serial_proof_begin(4));
    reset();assert(serial_proof_begin(5));broken=0x3f8;
    assert(!serial_proof_write(5,"x",1));assert(!serial_proof_end(5));assert(!length);
    reset();g_serial_count=2;g_serial_ports[1]=0x2f8;broken=0x2f8;
    assert(serial_proof_begin(6) && serial_proof_write(6,"survivor",8) && serial_proof_end(6));
    assert(length==8 && !memcmp(wire,"survivor",8));
    assert(g_serial_dead[1] && !g_serial_dead[0] && serial_proof_failure_mask()==2);
    broken=0;
    assert(serial_proof_record("complete\n",9) && length==17 && g_serial_dead[1]);
    reset();g_serial_count=2;g_serial_ports[1]=0x2f8;broken=0x3f8;
    assert(serial_proof_record("lost\n",5) && !length && g_serial_dead[0]);
    broken=0;
    assert(serial_proof_record("footer\n",7) && !length && g_serial_dead[0] && serial_proof_failure_mask()==1);
    enforce=false;serial_write_all("ordinary\n");assert(!length);enforce=true;
    reset();broken=0x3f8;
    assert(!serial_proof_record("lost\n",5) && g_serial_dead[0]);
    broken=0;
    assert(!serial_proof_record("footer\n",7) && !length && !g_serial_proof_session);
    reset();assert(serial_proof_begin(9));g_panic_in_progress=1;
    assert(!serial_proof_write(9,"footer",6));enforce=false;enabled=false;
    serial_write_all("panic\n");assert(!enabled && length==7);
    enabled=true;assert(!serial_proof_end(9));
    reset();enforce=false;delayed=UART_NORMAL_POLL_LIMIT+1u;
    serial_write_all("ab");
    assert(length==1 && wire[0]=='b'); /* Actual legacy silent-drop witness. */
    reset();delayed=UART_NORMAL_POLL_LIMIT+1u;
    assert(!serial_proof_record("ab",2) && !length && !g_serial_proof_session);
    reset();delayed=UART_NORMAL_POLL_LIMIT-1u;
    assert(serial_proof_record("ab",2) && length==2 && !memcmp(wire,"ab",2));
    reset();enabled=false;assert(!serial_proof_record("x",1) && !enabled && !polls);enabled=true;
    assert(!serial_proof_record(NULL,1) && !serial_proof_record("x",769));
    inject=true;assert(serial_proof_record("record\n",7));
    assert(decoded_size()==18 && !memcmp(decoded,"record\ninterrupt\r\n",18));
    reset();feed_remaining=75000;assert(serial_proof_record("opening\n",8));
    assert(!feed_remaining && decoded_size()==75008 && !memcmp(decoded,"opening\n",8));
    for(size_t i=8;i<75008;++i)assert(decoded[i]=='x');
    uint64_t aux_count,aux_bytes;serial_proof_aux_totals(&aux_count,&aux_bytes);
    assert(aux_bytes==75000 && aux_count>1 && !g_serial_proof_session);
    reset();assert(serial_proof_begin(70));
    for(unsigned i=0;i<800;++i) {serial_write_all("between records\n");assert(serial_proof_write(70,"proof\n",6));assert(serial_proof_flush(70));}
    assert(serial_proof_end(70) && decoded_size()==800*(6+17));
    reset();feed_remaining=SERIAL_PROOF_DRAIN_BYTES+128;
    assert(!serial_proof_record("opening\n",8) && g_serial_proof_failed && !g_serial_proof_session);
    puts("Serial proof actual functions: PASS exact_bytes concurrent_queue capacity flags UART_failure panic");
}
