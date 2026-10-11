#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/core/pvclock.h"

static pvclock_page_t shared;
static uint64_t counter;
static unsigned reads, action;

uint64_t irq_latency_host_read_tsc(void)
{
    ++reads;
    if (action == 1 || (action == 2 && reads == 1)) shared.version += 2;
    if (action == 3) shared.version |= 1;
    if (action == 4) { shared.version += 2; shared.ns_anchor += 1000; }
    return counter;
}

static pvclock_page_t page(void)
{
    return (pvclock_page_t){.version=2,.tsc_anchor=100,.ns_anchor=1000,
        .multiplier=UINT32_C(0x80000000),.shift=1,.flags=PVCLOCK_FLAG_STABLE};
}

static pvclock_reference_t point(uint64_t ns)
{
    return (pvclock_reference_t){
        .before={.tsc=ns-100,.ns=ns-100,.version=2,.multiplier=0x80000000,
                 .shift=1,.flags=PVCLOCK_FLAG_STABLE},
        .after={.tsc=ns+100,.ns=ns+100,.version=2,.multiplier=0x80000000,
                .shift=1,.flags=PVCLOCK_FLAG_STABLE},
        .ticks=ns/10,.ns=ns,.period_fs=10000000,.mode=1,.valid=true};
}

int main(void)
{
    uint32_t bits=PVCLOCK_FEATURE_SYSTEM_TIME|PVCLOCK_FEATURE_STABLE;
    assert(pvclock_features(0x40000001,0x4b4d564b,0x564b4d56,0x4d,bits));
    assert(!pvclock_features(0x40000000,0x4b4d564b,0x564b4d56,0x4d,bits));
    assert(!pvclock_features(0x40000001,0x4b4d564a,0x564b4d56,0x4d,bits));
    assert(!pvclock_features(0x40000001,0x4b4d564b,0x564b4d56,0x4d,8));
    assert(!pvclock_features(0x40000001,0x4b4d564b,0x564b4d56,0x4d,1<<24));
    pvclock_page_t p=page();uint64_t ns=99;
    assert(pvclock_interpolate(&p,12345,&ns)==PVCLOCK_OK && ns==13245);
    p.shift=-1;
    assert(pvclock_interpolate(&p,12345,&ns)==PVCLOCK_OK && ns==4061);
    p.shift=0;p.multiplier=0x55555555;
    assert(pvclock_interpolate(&p,1000100,&ns)==PVCLOCK_OK && ns==334333);
    p.multiplier=UINT32_MAX;
    assert(pvclock_interpolate(&p,UINT64_C(4294967396),&ns)==PVCLOCK_OK && ns==UINT64_C(4294968295));
    p=page();p.shift=63;
    assert(pvclock_interpolate(&p,101,&ns)==PVCLOCK_OK && ns==UINT64_C(4611686018427388904));
    ns=99;assert(pvclock_interpolate(&p,102,&ns)==PVCLOCK_OVERFLOW && ns==99);
    for (int shift=-128;shift<=127;++shift) {
        p=page();p.shift=(int8_t)shift;ns=99;
        pvclock_status_t s=pvclock_interpolate(&p,100,&ns);
        if (shift<=-64 || shift>=64) assert(s==PVCLOCK_INVALID && ns==99);
        else assert(s==PVCLOCK_OK && ns==1000);
    }
    for (unsigned mutation=0;mutation<9;++mutation) {
        p=page();uint64_t t=200;ns=99;
        if(mutation==0)p.version=0;
        if(mutation==1)p.version=3;
        if(mutation==2)p.multiplier=0;
        if(mutation==3)p.flags=0;
        if(mutation==4)p.flags=5;
        if(mutation==5)p.flags=3;
        if(mutation==6)t=99;
        if(mutation==7)p.ns_anchor=UINT64_MAX;
        if(mutation==8)p.shift=64;
        assert(pvclock_interpolate(&p,t,&ns)!=PVCLOCK_OK && ns==99);
    }
    assert(!pvclock_apply_offset(UINT64_MAX,1,&ns));
    assert(!pvclock_apply_offset(0,-1,&ns));
    assert(pvclock_apply_offset(UINT64_C(1)<<63,INT64_MIN,&ns) && ns==0);
    assert(!pvclock_apply_offset((UINT64_C(1)<<63)-1,INT64_MIN,&ns));
    assert(pvclock_apply_offset(1234,-234,&ns) && ns==1000);
    assert(pvclock_apply_offset(1000,234,&ns) && ns==1234);

    for (action=0;action<=4;++action) {
        shared=page();counter=1000;reads=0;
        pvclock_sample_t s;memset(&s,0xa5,sizeof(s));pvclock_sample_t before=s;
        pvclock_status_t status=pvclock_snapshot(&shared,&s);
        if(action==0 || action==2) {
            assert(status==PVCLOCK_OK && s.ns==1900 && s.tsc==1000);
            assert(reads==(action==2?2u:1u) && s.retries==reads-1);
        } else {
            assert(status==PVCLOCK_BUSY && !memcmp(&s,&before,sizeof(s)));
            assert(reads==(action==3?1u:PVCLOCK_SNAPSHOT_ATTEMPTS));
        }
    }
    shared=page();shared.version=3;reads=0;
    pvclock_sample_t s={0};assert(pvclock_snapshot(&shared,&s)==PVCLOCK_BUSY && !reads);
    shared=page();shared.flags=3;action=0;reads=0;
    assert(pvclock_snapshot(&shared,&s)==PVCLOCK_STOPPED && reads==1);

    pvclock_reference_t a=point(1000000),b=point(21000000);
    int64_t lo=9,hi=9;
    assert(pvclock_reference_bounds(&a,&lo,&hi) && lo==-110 && hi==110);
    assert(pvclock_reference_rate(&a,&b));
    assert(pvclock_reference_offset_at(&a,2000000,&lo,&hi) && lo==-10110 && hi==10110);
    assert(!pvclock_reference_offset_at(&a,0,&lo,&hi));
    pvclock_reference_t later=point(1001000000);
    assert(pvclock_reference_drift(&a,&later) && !pvclock_reference_rate(&a,&later));
    pvclock_reference_t delayed=later;
    delayed.after.ns+=124594;delayed.after.tsc+=124594;
    assert(pvclock_reference_drift(&a,&delayed));
    delayed.after.ns+=20000000;delayed.after.tsc+=20000000;
    assert(!pvclock_reference_drift(&a,&delayed));
    later.ns+=50000;later.ticks+=5000;
    assert(pvclock_reference_drift(&a,&later));
    later.ns+=20000000;later.ticks+=2000000;
    assert(!pvclock_reference_drift(&a,&later));
    for(unsigned mutation=0;mutation<19;++mutation) {
        pvclock_reference_t bad=b;
        if(mutation==0)bad.valid=false;
        if(mutation==1)bad.period_fs=0;
        if(mutation==2)bad.mode=0;
        if(mutation==3)bad.retries=5;
        if(mutation==4)bad.before.flags=3;
        if(mutation==5)bad.after.version=3;
        if(mutation==6)bad.before.ns=bad.after.ns;
        if(mutation==7)bad.before.tsc=bad.after.tsc;
        if(mutation==8)bad.after.ns+=100001;
        if(mutation==9)bad.before.multiplier=0;
        if(mutation==10)bad.after.shift=2;
        if(mutation==11)bad.before.retries=4;
        if(mutation==12)bad.after.retries=4;
        if(mutation==13)bad.ns=UINT64_MAX;
        if(mutation==14)bad.ticks=a.ticks;
        if(mutation==15)bad.ns=a.ns+1000;
        if(mutation==16){bad.before.ns+=300000;bad.after.ns+=300000;}
        if(mutation==17){bad.before.ns-=300000;bad.after.ns-=300000;}
        if(mutation==18){bad.mode=2;bad.retries=1;}
        assert(!pvclock_reference_rate(&a,&bad));
    }
    puts("KVM pvclock actual ABI/arithmetic/snapshot: PASS bounded_retry=4 independent_vectors=CHECKED");
}
