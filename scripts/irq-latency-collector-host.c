/* Actual completed-ledger selection/report integration, synthetic clocks only. */
#include "../kernel/src/memory/irq_latency_test.c"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

uint32_t g_cpu_count=2;
static FILE *capture;
static unsigned lines;
bool stack_test_emit(const char *format,...)
{
    char text[1024]; va_list ap; va_start(ap,format);
    int n=vsnprintf(text,sizeof(text),format,ap);va_end(ap);
    assert(n>0 && n<(int)sizeof(text) && text[n-1]=='\n');text[--n]=0;
    uint32_t crc=UINT32_MAX;
    for (int i=0;i<n;++i) {
        crc^=(unsigned char)text[i];
        for (unsigned j=0;j<8;++j) crc=(crc>>1)^((crc&1) ? 0xedb88320u : 0);
    }
    if (capture) fprintf(capture,"%s crc=%08x\n",text,~crc);
    ++lines;return true;
}
static void reset(unsigned role)
{
    calibration_series_t *s=&g_calibration[role];memset(s,0,sizeof(*s));
    s->limit=CAL_WINDOWS_PER_CPU*g_cpu_count;s->workers=g_cpu_count;s->joined=1;
    for (unsigned c=0;c<g_cpu_count;++c) {
        s->caps_seen[c]=1;
        s->caps[c]=(irq_latency_capabilities_t){.maximum_basic=1,.leaf1_edx=16};
    }
    for (unsigned i=0;i<s->limit;++i) {
        calibration_attempt_t *a=&s->attempt[i];a->worker=i%g_cpu_count;a->complete=1;a->acquisition=1;
        for (unsigned e=0;e<2;++e) {
            uint64_t ticks=1000+i*100000+role*100000000;
            uint64_t t=UINT64_C(1000000000000)*e+ticks*240;
            irq_latency_reference_point_t *p=e?&a->window.end:&a->window.begin;
            *p=(irq_latency_reference_point_t){.tsc_before=t,.tsc_after=t+100,.ticks=ticks,
                .cpu=e,.end_cpu=e,.mode=1,.counter_bits=64,.valid=true,.initialized=true,
                .interrupts_enabled=true,.acquired=true,.sample_valid=true,.identity_valid=true,
                .if_after=true,.period_fs=100000000,.period_after=100000000};
        }
        a->window.period_fs=100000000;
        for (unsigned j=0;j<4;++j)
            a->probe[j]=(irq_latency_irq_probe_t){.cpu=j%2,.end_cpu=j%2,.valid=1,
                .outer_begin=100,.inner_begin=150,.inner_end=250,.outer_end=300,
                .sequence_after=1,.irq_after=1};
    }
}
int main(int argc,char **argv)
{
    reset(0);assert(calibration_select(0));
    assert(g_calibration[0].pair_count==84 && g_calibration[0].valid_windows[0]==42 &&
           g_calibration[0].valid_windows[1]==42 && g_calibration[0].valid_probes[0]==256);
    for (unsigned i=0;i<128;++i) assert(!g_calibration[0].attempt[i].valid);
    reset(1);assert(calibration_select(1));
    if (argc==2) { capture=fopen(argv[1],"w");assert(capture); }
    assert(calibration_report(0,true));assert(calibration_report(1,true));assert(lines>1000);
    assert(stack_test_emit("[IRQ_LATENCY][BEGIN] cpus=2 epoch=1 calibration=cpu-local-reference-pairs calibration_version=1 witness_cells=29 schema=2 status=ACTIVE\n"));
    if (capture) { assert(!fclose(capture));capture=NULL; }
    reset(0);g_calibration[0].attempt[64].complete=0;assert(!calibration_select(0));
    reset(0);g_calibration[0].attempt[0].window.begin.switch_after=1;
    g_calibration[0].attempt[0].window.begin.valid=false;assert(calibration_select(0));
    assert(g_calibration[0].disposition[0].kind==IRQ_POINT_INELIGIBLE);
    reset(0);g_calibration[0].attempt[2].window.begin.tsc_after+=1000000;
    assert(calibration_select(0) && !g_calibration[0].pairs[0].valid && g_calibration[0].pairs[0].end==4);
    reset(0);
    for (unsigned i=6;i<128;++i) {
        irq_latency_reference_point_t *p=&g_calibration[0].attempt[i].window.end;
        p->sample_valid=p->valid=false;
    }
    assert(!calibration_select(0) && g_calibration[0].valid_windows[1]==2 &&
           (g_calibration[0].clock_errors[1]&CAL_CLOCK_WINDOWS));
    reset(0);assert(calibration_select(0));reset(1);
    g_calibration[1].attempt[0].probe[0].outer_end=1000;
    assert(!calibration_select(1) && (g_calibration[1].observer_errors[0]&CAL_OBSERVER_DRIFT));
    puts("IRQ latency actual-C completed collector and reports: PASS clocks=HOSTED");
}
