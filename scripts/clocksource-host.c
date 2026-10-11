#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../kernel/src/core/clocksource.c"
#include "../kernel/src/core/clock.c"

SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
uint32_t g_cpu_count=2,g_bsp_apic_id;
static uint32_t host_cpu,msr_writes,hpet_reads,queries,control,read_action;
static uint64_t host_flags,host_time=1000000,tsc_step=10000,msrs[HOBBYOS_MAX_CPUS];
static bool has_features=true,write_ok=true,read_ok=true,physical_ok=true,hpet_ok=true;
static char report[16384];
static size_t report_size;

irq_flags_t irq_save(void) {uint64_t f=host_flags;host_flags&=~512ULL;return f;}
void irq_restore(irq_flags_t f) {host_flags=f;}
bool irq_is_enabled(void) {return (host_flags&512)!=0;}
bool smp_current_cpu_slot(cpu_slot_t *slot) {++queries;*slot=host_cpu;return host_cpu<g_cpu_count;}
cpu_slot_t smp_bsp_cpu_slot(void) {return 0;}
uint32_t lapic_get_id(void) {return host_cpu;}
bool hpet_is_available(void) {return true;}
uint64_t hpet_frequency_hz(void) {return 100000000;}
uint64_t hpet_period_fs(void) {return 10000000;}
bool hpet_read_clock_sample(hpet_clock_sample_t *p)
{
    ++hpet_reads;
    *p=(hpet_clock_sample_t){.ticks=(host_time+100000)/10,.ns=host_time+100000,
        .valid=hpet_ok,.mode=HPET_COUNTER_READ_SPLIT64_STABLE};
    if((control==22 && hpet_reads%4==2) || (control==23 && host_cpu==1)) {
        p->ticks+=100000;p->ns+=1000000;
    }
    if((control==30 || control==31) && hpet_reads>8) {
        uint64_t drift=control==30 ? 50000 : 20000000;
        p->ticks+=drift/10;p->ns+=drift;
    }
    return hpet_ok;
}
bool physical_image_address_range(uint64_t v,size_t bytes,uint64_t *physical)
{
    assert(v==(uintptr_t)&g_source_pages[host_cpu] && bytes==64);
    *physical=UINT64_C(0x100000)+host_cpu*64;return physical_ok;
}
bool cpu_write_msr_safe(uint32_t msr,uint64_t value)
{
    assert(msr==PVCLOCK_SYSTEM_TIME_MSR && !irq_is_enabled());++msr_writes;
    if(!write_ok)return false;
    msrs[host_cpu]=value;
    if(value&1)g_source_pages[host_cpu].page=(pvclock_page_t){.version=2,
        .tsc_anchor=host_cpu*1000000,.ns_anchor=0,.multiplier=0x80000000,.shift=1,.flags=1};
    return true;
}
bool cpu_read_msr_safe(uint32_t msr,uint64_t *out)
{
    assert(msr==PVCLOCK_SYSTEM_TIME_MSR && !irq_is_enabled());
    if(!read_ok)return false;
    *out=msrs[host_cpu];
    if(control==24 && *out)*out^=64;
    return true;
}
bool cpu_get_cpuid_count(uint32_t leaf,uint32_t sub,uint32_t *a,uint32_t *b,uint32_t *c,uint32_t *d)
{
    assert(sub==0);*a=*b=*c=*d=0;
    if(leaf==0){*a=1;*b=0x756e6547;*c=0x6c65746e;*d=0x49656e69;return true;}
    if(leaf==1){*c=1u<<31;*d=(1u<<4)|(1u<<5)|(1u<<26);return true;}
    if(leaf==PVCLOCK_CPUID_BASE){*a=0x40000001;*b=0x4b4d564b;*c=0x564b4d56;*d=0x4d;return has_features;}
    if(leaf==PVCLOCK_CPUID_FEATURES){*a=(1u<<3)|(1u<<24);return has_features;}
    return false;
}
uint64_t irq_latency_host_read_tsc(void)
{
    host_time+=tsc_step;
    uint32_t action=read_action;read_action=0;
    if(action==1) {
        uint64_t value=999;
        assert(!clock_monotonic_read_ns(&value) && value==999);
    }
    if(action==2)host_cpu=1;
    if(action==3)host_flags=514;
    return host_time+host_cpu*1000000;
}
void spinlock_init(spinlock_t *s){memset(s,0,sizeof(*s));}
int spin_trylock(spinlock_t *s){(void)s;return 0;}
void spin_unlock(spinlock_t *s){(void)s;}
__attribute__((noreturn)) void kpanic(const char *message)
{
    if(control==20 && !strcmp(message,"CLOCK: no valid monotonic sample"))_exit(0);
    fprintf(stderr,"unexpected panic: %s\n",message);abort();
}
int ksnprintf(char *s,size_t n,const char *format,...)
{va_list ap;va_start(ap,format);int result=vsnprintf(s,n,format,ap);va_end(ap);return result;}
void serial_write_all(const char *line)
{size_t n=strlen(line);assert(n<640 && report_size+n<sizeof(report));memcpy(report+report_size,line,n+1);report_size+=n;}

static void setup(void)
{
    assert(clock_monotonic_init());
    for(host_cpu=0;host_cpu<g_cpu_count;++host_cpu) {
        g_cpus[host_cpu].apic_id=host_cpu;g_cpus[host_cpu].slot=host_cpu;
        clocksource_prepare_local();
    }
    host_cpu=0;clocksource_select();clocksource_report();
}

static void run_case(unsigned kind)
{
    control=kind;
    if(kind==1 || kind==25)has_features=false;
    if(kind==2)write_ok=false;
    if(kind==3)read_ok=false;
    if(kind==4)physical_ok=false;
    if(kind==5)hpet_ok=false;
    if(kind==6)tsc_step=0;
    if(kind==7)host_flags=514;
    setup();
    if((kind>=1 && kind<=7) || (kind>=22 && kind<=25)) {
        assert(clocksource_current()==CLOCKSOURCE_HPET && clocksource_epoch()==1);
        if(kind==1 || kind==4 || kind==7)assert(msr_writes==0);
        if(kind==2 || kind==3)assert(g_source_cpu[0].status==SOURCE_QUARANTINED && g_source_cpu[0].registered);
        if(kind==5 || kind==6)assert(!g_source_cpu[0].registered);
        uint64_t now=9;
        hpet_ok=true;assert(clock_monotonic_read_ns(&now) && now>9);
        if(kind==23)assert(g_source_cpu[0].status==SOURCE_QUALIFIED && g_source_cpu[1].status==SOURCE_QUALIFIED);
        if(kind==24)assert(g_source_cpu[0].reason==SOURCE_REASON_MSR && !g_source_cpu[0].registered);
        if(kind==25) {
            for(unsigned n=1;n<CLOCK_STALLED_SAMPLE_LIMIT;++n)assert(clock_monotonic_read_ns(&now));
            now=42;assert(!clock_monotonic_read_ns(&now) && now==42);
            host_time+=10;assert(clock_monotonic_read_ns(&now) && now>42);
        }
        return;
    }
    assert(clocksource_current()==CLOCKSOURCE_KVM && clocksource_epoch()==2);
    assert(g_source_offset==95000 && hpet_reads==8 && msr_writes==2);
    assert(strstr(report,"source=2 epoch=2 cpus=2") && strstr(report,"index=3"));
    uint64_t value=0;host_flags=514;
    assert(clock_monotonic_read_ns(&value) && value==host_time+95000 && host_flags==514);
    uint64_t first=value;unsigned prior_hpet=hpet_reads;
    host_cpu=1;
    assert(clock_monotonic_read_ns(&value) && value>first && host_flags==514 && hpet_reads==prior_hpet);
    host_cpu=0;
    if(kind==21) {
        assert(!clocksource_terminal_read(0) && !clocksource_terminal_capture(0));
        host_flags=70;g_source_cpu[0].busy=1;assert(!clocksource_terminal_capture(0));
        g_source_cpu[0].busy=0;assert(clocksource_terminal_capture(0));
        const clocksource_terminal_t *t=clocksource_terminal_read(0);
        assert(t && t->source==2 && t->epoch==2 && t->registered && !t->busy);
        uint64_t saved=t->reads;assert(clock_monotonic_read_ns(&value));
        assert(t->reads==saved && g_source_cpu[0].reads>saved && !clocksource_terminal_capture(0));
        assert(!clocksource_terminal_read(1) && !clocksource_terminal_read(UINT32_MAX));
        return;
    }
    if(!kind || kind==26 || kind==29 || kind==30 || kind==32) {
        unsigned original_writes=msr_writes;
        clocksource_prepare_local();assert(msr_writes==original_writes);
        if(kind==29)tsc_step=10;
        if(kind==32)tsc_step=124594;
        host_time+=CLOCKSOURCE_REFERENCE_NS;
        assert(clock_monotonic_read_ns(&value) && hpet_reads==prior_hpet+1 && g_source_cpu[0].references==1);
        assert(g_source_cpu[0].maximum_reference_ns==tsc_step);
        host_flags=70;assert(clock_monotonic_read_ns(&value) && host_flags==70);
        uint64_t selected=clocksource_epoch();clocksource_select();assert(clocksource_epoch()==selected);
        return;
    }
    if(kind==8)g_source_pages[0].page.version=3;
    if(kind==9)g_source_pages[0].page.flags=3;
    if(kind==10)g_source_pages[0].page.multiplier=1;
    if(kind==11)host_time-=1000000;
    if(kind==12){host_time=g_source_cpu[0].last_tsc;tsc_step=0;}
    if(kind==13)g_source_pages[0].page.ns_anchor+=1000000;
    if(kind==14){host_time+=CLOCKSOURCE_REFERENCE_NS;hpet_ok=false;}
    if(kind==15)g_source_cpu[0].busy=1;
    if(kind==16)read_action=1;
    if(kind==17)read_action=2;
    if(kind==18)read_action=3;
    if(kind==19)host_cpu=2;
    if(kind==20){g_source_pages[0].page.version=3;(void)clock_monotonic_ns();assert(false);}
    if(kind==27)g_source_cpu[0].reads=UINT64_MAX;
    if(kind==28){g_source_cpu[0].references=UINT64_MAX;host_time+=CLOCKSOURCE_REFERENCE_NS;}
    if(kind==31)host_time+=CLOCKSOURCE_REFERENCE_NS;
    if(kind==33){host_time+=CLOCKSOURCE_REFERENCE_NS;tsc_step=20000000;}
    value=999;
    size_t report_before=report_size;
    assert(!clock_monotonic_read_ns(&value) && value==999 && host_flags==514);
    assert(clocksource_current()==CLOCKSOURCE_FAILED && clocksource_epoch()==3);
    assert(report_size==report_before);
    unsigned reads_before=hpet_reads;
    g_source_pages[0].page.version=4;hpet_ok=true;host_cpu=0;
    assert(!clock_monotonic_read_ns(&value) && value==999 && hpet_reads==reads_before);
}

static void emit_reference(unsigned cpu,unsigned point,const pvclock_reference_t *p)
{
    printf("[CLOCKSOURCE_REFERENCE] version=3 cpu=%u epoch=2 point=%u "
        "tsc0=%llu ns0=%llu ver0=%u mul0=%u shift0=%d flags0=%u retry0=%u "
        "tsc1=%llu ns1=%llu ver1=%u mul1=%u shift1=%d flags1=%u retry1=%u "
        "ticks=%llu ns=%llu period=%llu mode=%u retries=%u valid=%u\n",
        cpu,point,(unsigned long long)p->before.tsc,(unsigned long long)p->before.ns,
        p->before.version,p->before.multiplier,p->before.shift,p->before.flags,p->before.retries,
        (unsigned long long)p->after.tsc,(unsigned long long)p->after.ns,
        p->after.version,p->after.multiplier,p->after.shift,p->after.flags,p->after.retries,
        (unsigned long long)p->ticks,(unsigned long long)p->ns,
        (unsigned long long)p->period_fs,p->mode,p->retries,p->valid);
}

/* Serialize actual controller captures. Hardware time is synthetic, not guest timing. */
static void terminal_case(unsigned kind,bool emit)
{
    assert(kind<8);setup();
    for(host_cpu=0;host_cpu<g_cpu_count;++host_cpu) {
        source_cpu_t *s=&g_source_cpu[host_cpu];
        unsigned steps=(kind==1 || kind==3 || kind==4 || kind==5) ? 3 : 1;
        for(unsigned i=0;i<steps;++i) {
            if(kind>=2)host_time+=CLOCKSOURCE_REFERENCE_NS;
            tsc_step=kind==3 ? 10000*(i+1) : kind==4 && i==1 ? 30000 : 10000;
            uint64_t value=42;host_flags=514;
            assert(clock_monotonic_read_ns(&value) && value>42 && host_flags==514);
            assert(s->last_tsc>s->points[3].after.tsc && s->last_ns>s->points[3].after.ns);
            assert(s->references<=s->reads);
            if(kind>=2)assert(s->last_tsc<s->reference.before.tsc && s->last_ns<s->reference.before.ns);
        }
        if(kind==7) {
            uint64_t value=42;
            assert(clock_monotonic_read_ns(&value) && s->reads==2 && s->references==1);
            assert(s->last_tsc>s->reference.after.tsc && s->last_ns>s->reference.after.ns);
        }
        if(kind==6) {
            host_time+=CLOCKSOURCE_REFERENCE_NS;hpet_ok=false;host_flags=70;
            clocksource_sample_t output,saved;
            memset(&output,0xA5,sizeof(output));memcpy(&saved,&output,sizeof(saved));
            assert(!clocksource_read_local(host_cpu,&output) && !memcmp(&output,&saved,sizeof(output)));
            assert(clocksource_current()==CLOCKSOURCE_FAILED && s->reads==1 && s->references==2);
            hpet_ok=true;
            assert(!clocksource_read_local(host_cpu,&output) && !memcmp(&output,&saved,sizeof(output)));
            uint64_t value=42;assert(!clock_monotonic_read_ns(&value) && value==42);
        }
        host_flags=70;assert(clocksource_terminal_capture(host_cpu));
        const clocksource_terminal_t *t=clocksource_terminal_read(host_cpu);
        assert(t && t->reads==s->reads && t->references==s->references && !t->busy);
        if(kind<2)assert(t->references==0 && t->maximum_reference_ns==0 && t->reads==steps);
        if(kind==2)assert(t->reads==1 && t->references==1 && t->previous.after.tsc==s->points[3].after.tsc);
        if(kind==3)assert(t->references==3 && t->maximum.after.tsc==s->reference.after.tsc);
        if(kind==4 || kind==5) {
            assert(t->references==3 && t->maximum.after.tsc<s->reference.before.tsc);
            assert((t->previous.after.tsc==s->points[3].after.tsc)==(kind==5));
        }
        if(kind==6)break;
    }
    if(!emit)return;
    fputs(report,stdout);
    for(unsigned cpu=0;cpu<g_cpu_count;++cpu) {
        const clocksource_terminal_t *t=clocksource_terminal_read(cpu);
        if(!t)continue;
        printf("[CLOCKSOURCE_TERMINAL] version=3 cpu=%u source=%u epoch=%llu reason=%u "
            "busy=%u registered=%u reads=%llu references=%llu maximum_reference_ns=%llu "
            "last_tsc=%llu last_ns=%llu\n",cpu,t->source,(unsigned long long)t->epoch,
            t->reason,t->busy,t->registered,(unsigned long long)t->reads,(unsigned long long)t->references,
            (unsigned long long)t->maximum_reference_ns,(unsigned long long)t->last_tsc,(unsigned long long)t->last_ns);
        if(t->references) {emit_reference(cpu,0,&t->previous);emit_reference(cpu,1,&t->maximum);}
    }
}

int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--boot-report")) {setup();fputs(report,stdout);return 0;}
    if(argc==3 && !strcmp(argv[1],"--terminal-report")) {terminal_case((unsigned)strtoul(argv[2],NULL,10),true);return 0;}
    assert(argc==1);
    for(unsigned kind=0;kind<42;++kind) {
        pid_t pid=fork();assert(pid>=0);
        if(!pid){if(kind<34)run_case(kind);else terminal_case(kind-34,false);_exit(0);}
        int status;assert(waitpid(pid,&status,0)==pid);
        if(!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr,"clocksource control %u failed\n",kind);return 1;
        }
    }
    puts("KVM clocksource actual registration/clock controller: PASS cases=42 fallback=BOOT_ONLY lifetime=RETAINED");
}
