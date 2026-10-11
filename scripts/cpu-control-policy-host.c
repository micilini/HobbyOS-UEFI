/* Hosted substitutes exercise the production C sequence, never host controls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/cpu/control_policy.h"
#include "../kernel/src/memory/cache_attributes.h"
#include "../kernel/src/memory/page_table_decode.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "check failed at line %u: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint64_t cr[5], efer, pat, flags;
static char trace[1024];
static unsigned used, fail_read, fail_write, reads, writes;
static bool corrupt_readback;
static void event(char value) { CHECK(used + 1 < sizeof(trace)); trace[used++] = value; trace[used] = 0; }
uint64_t cpu_policy_test_read_cr(unsigned i) { event((char)('a' + i)); return cr[i]; }
void cpu_policy_test_write_cr(unsigned i, uint64_t v) { event((char)('A' + i)); cr[i] = v; }
bool cpu_policy_test_read_msr(uint32_t m, uint64_t *v)
{
    event(m == 0xc0000080u ? 'r' : 'p');
    if (++reads == fail_read) return false;
    *v = m == 0xc0000080u ? efer : pat;
    if (corrupt_readback && writes && m == 0x277) *v ^= 1;
    return true;
}
bool cpu_policy_test_write_msr(uint32_t m, uint64_t v)
{
    event(m == 0xc0000080u ? 'R' : 'P');
    if (++writes == fail_write) return false;
    if (m == 0xc0000080u) efer = v | 0x400; else pat = v;
    return true;
}
uint64_t cpu_policy_test_irq_save(void) { event('s'); uint64_t f = flags; flags &= ~512ull; return f; }
void cpu_policy_test_irq_restore(uint64_t f) { event('i'); flags = f; }
void cpu_policy_test_wbinvd(void) { event('W'); CHECK(!(flags & 512)); CHECK((cr[0] & 0x60000000) == 0x40000000); }

static cpu_policy_features_t features(bool nx)
{
    return (cpu_policy_features_t){.basic_edx=0x10060,
        .extended_edx=0x24000000u | (nx ? 0x100000u : 0), .apic_id=7,
        .basic_valid=true, .extended_valid=true, .identity_valid=true};
}
static void reset(uint64_t f)
{
    memset(cr,0,sizeof(cr)); cr[0]=0x80010033; cr[3]=0x12345000;
    cr[4]=0x668; efer=0xd00; pat=0x0007040600070406ull; flags=f;
    used=reads=writes=fail_read=fail_write=0; trace[0]=0; corrupt_readback=false;
}
static uint64_t tables[7][512];
static bool table_read(void *context, uint64_t table, uint16_t ix, uint64_t *v)
{
    (void)context;
    if (table < 0x1000 || table > 0x7000 || table % 4096) return false;
    *v = tables[table / 4096 - 1][ix]; return true;
}
static void decisions(void)
{
    cpu_policy_t p, saved;
    for (unsigned nx=0; nx<2; ++nx) {
        cpu_policy_features_t f=features(nx);
        CHECK(cpu_policy_build(&f,&p)==CPU_POLICY_OK);
        CHECK(p.cr0_protected==0x10033 && p.cr0_final==0x80010033);
        CHECK(p.cr4==0x20 && p.efer_write==(nx ? 0x900u : 0x100u));
        CHECK(p.pat==0x0007010600070406ull);
        CHECK(p.required_basic==0x10060 && p.required_extended==(nx ? 0x20100000u : 0x20000000u));
        CHECK(cpu_policy_validate(&p,&f)==CPU_POLICY_OK);
        saved=p;
        p.reserved=1;CHECK(cpu_policy_validate(&p,&f)==CPU_POLICY_INVALID);p=saved;
        p.page1g_supported=2;CHECK(cpu_policy_validate(&p,&f)==CPU_POLICY_INVALID);p=saved;
        uint64_t *fields[]={&p.cr0_protected,&p.cr0_final,&p.cr4,&p.efer_write,&p.pat};
        for (unsigned i=0;i<5;i++) for (unsigned bit=0;bit<64;bit++) {
            *fields[i] ^= 1ull<<bit;
            CHECK(cpu_policy_validate(&p,&f)!=CPU_POLICY_OK);
            p=saved;
        }
        for (unsigned bit=0;bit<32;bit++) {
            p.required_basic ^= 1u<<bit; CHECK(cpu_policy_validate(&p,&f)!=CPU_POLICY_OK);p=saved;
            p.required_extended ^= 1u<<bit; CHECK(cpu_policy_validate(&p,&f)!=CPU_POLICY_OK);p=saved;
        }
        cpu_policy_features_t bad=f;
        bad.basic_valid=false; CHECK(cpu_policy_build(&bad,&p)==CPU_POLICY_FEATURES); CHECK(!memcmp(&p,&saved,sizeof(p)));
        bad=f;bad.extended_valid=false;CHECK(cpu_policy_build(&bad,&p)==CPU_POLICY_FEATURES);
        bad=f;bad.identity_valid=false;CHECK(cpu_policy_build(&bad,&p)==CPU_POLICY_IDENTITY);
        const unsigned missing[]={5,6,16};
        for(unsigned i=0;i<3;i++){bad=f;bad.basic_edx &= ~(1u<<missing[i]);CHECK(cpu_policy_build(&bad,&p)==CPU_POLICY_FEATURES);}
        bad=f;bad.extended_edx &= ~(1u<<29);CHECK(cpu_policy_build(&bad,&p)==CPU_POLICY_FEATURES);
        if(nx){bad=f;bad.extended_edx &= ~(1u<<20);CHECK(cpu_policy_validate(&p,&bad)==CPU_POLICY_FEATURES);}
        cpu_policy_readback_t actual={0x80010033,0x12345000,0x20,nx ? 0xd00u:0x500u,0x0007010600070406ull,7};
        CHECK(cpu_policy_match(&p,&f,&actual,0x12345000,7)==CPU_POLICY_OK);
        uint8_t publication=0;
        CHECK(cpu_policy_publish_ready(&publication,&p,&f,&actual,0x12345000,6)==CPU_POLICY_IDENTITY);CHECK(publication==0);
        actual.cr0^=0x10000;CHECK(cpu_policy_publish_ready(&publication,&p,&f,&actual,0x12345000,7)==CPU_POLICY_READBACK);CHECK(publication==0);actual.cr0^=0x10000;
        CHECK(cpu_policy_publish_ready(&publication,&p,&f,&actual,0x12345000,7)==CPU_POLICY_OK);CHECK(publication==1);
        CHECK(cpu_policy_publish_ready(&publication,&p,&f,&actual,0x12345000,7)==CPU_POLICY_DUPLICATE);CHECK(publication==1);
        CHECK(cpu_policy_match(&p,&f,&actual,0x12345000,6)==CPU_POLICY_IDENTITY);
        CHECK(cpu_policy_match(&p,&f,&actual,0x12346000,7)==CPU_POLICY_READBACK);
        uint64_t *registers[]={&actual.cr0,&actual.cr3,&actual.cr4,&actual.efer,&actual.pat};
        for(unsigned i=0;i<5;i++)for(unsigned b=0;b<64;b++){
            *registers[i] ^= 1ull<<b;CHECK(cpu_policy_match(&p,&f,&actual,0x12345000,7)!=CPU_POLICY_OK);*registers[i] ^= 1ull<<b;
        }
    }
    CHECK(cpu_policy_build(NULL,&p)==CPU_POLICY_INVALID);
    cpu_policy_features_t f=features(true);CHECK(cpu_policy_build(&f,NULL)==CPU_POLICY_INVALID);
    unsigned bytes[]={6,4,7,0,6,1,7,0};
    for(unsigned i=0;i<8;i++)CHECK(((p.pat>>(i*8))&255)==bytes[i]);
    for(unsigned v=0;v<256;v++)for(unsigned i=0;i<8;i++){
        uint64_t x=(p.pat&~(255ull<<(i*8)))|((uint64_t)v<<(i*8));
        CHECK(cpu_policy_pat_valid(x)==(v==0||v==1||v==4||v==5||v==6||v==7));
    }
    CHECK(cpu_policy_preserves_slots(0x0007040600070406ull,p.pat,0xdf));
    CHECK(!cpu_policy_preserves_slots(0x0007040600070406ull,p.pat,0xff));
    CHECK(!cpu_policy_preserves_slots(0x0007040600070406ull,p.pat^(1ull<<32),0x10));
}
static void encodings(void)
{
    cpu_policy_t p;cpu_policy_features_t f=features(true);CHECK(cpu_policy_build(&f,&p)==CPU_POLICY_OK);
    const cache_leaf_t leaves[]={CACHE_LEAF_4K,CACHE_LEAF_2M,CACHE_LEAF_1G};
    const uint64_t small[]={0,0x88,0x10,0x18},large[]={0,0x1008,0x10,0x18};
    const unsigned slots[]={0,5,2,3},types[]={6,1,7,0};
    for(unsigned t=0;t<4;t++)for(unsigned k=0;k<3;k++){
        uint64_t bits=0xdeadbeef;CHECK(cache_attribute_encode(&p,(cache_attribute_t)t,leaves[k],&bits));
        CHECK(bits==(k ? large[t]:small[t]));CHECK(!(bits&~(k?0x1018ull:0x98ull)));
        memset(tables,0,sizeof(tables));tables[0][0]=0x2003;tables[1][0]=0x3003;tables[2][0]=0x4003;
        unsigned level=k==0?1:k==1?2:3;tables[4-level][0]=bits|3|(k?128:0);
        pt_controls_t ctl={.cr0=0x80010033,.cr3=0x1000,.cr4=0x20,.efer=0xd00,.pat=p.pat,
            .physical_bits=48,.efer_valid=true,.pat_supported=true,.pat_valid=true,.nx_supported=true,.page1g_supported=true};
        pt_result_t result;page_table_decode(0,&ctl,table_read,NULL,&result);
        CHECK(result.status==PT_MAPPED && result.pat_index==slots[t] && result.pat_type==types[t]);
        CHECK(result.leaf_level==level);
    }
    uint64_t out=123;
    for(int leaf=-1;leaf<=32;leaf++)if(leaf!=12&&leaf!=21&&leaf!=30){CHECK(!cache_attribute_encode(&p,CACHE_WRITE_BACK,(cache_leaf_t)leaf,&out));CHECK(out==123);}
    CHECK(!cache_attribute_encode(&p,(cache_attribute_t)4,CACHE_LEAF_4K,&out));CHECK(out==123);
    CHECK(!cache_attribute_encode(NULL,CACHE_WRITE_BACK,CACHE_LEAF_4K,&out));
    p.page1g_supported=0;CHECK(!cache_attribute_encode(&p,CACHE_WRITE_BACK,CACHE_LEAF_1G,&out));CHECK(out==123);p.page1g_supported=1;
    p.required_basic &= ~0x10000u;CHECK(!cache_attribute_encode(&p,CACHE_WRITE_BACK,CACHE_LEAF_4K,&out));CHECK(out==123);
}
static void bootstrap(void)
{
    memset(tables,0,sizeof(tables));tables[0][0]=0x2003;tables[0][511]=0x3003;
    for(unsigned i=0;i<4;i++)tables[1][i]=0x4003+i*4096;
    tables[2][510]=0x4003;
    for(unsigned i=0;i<2048;i++)tables[3+i/512][i%512]=(uint64_t)i*0x200000|0x83;
    CHECK(cpu_policy_bootstrap_map(&tables[0][0],sizeof(tables),0x1000,0x1000));
    for(unsigned f=0;f<7;f++)for(unsigned i=0;i<512;i++){
        uint64_t old=tables[f][i];tables[f][i]^=0x1008;
        CHECK(!cpu_policy_bootstrap_map(&tables[0][0],sizeof(tables),0x1000,0x1000));tables[f][i]=old;
    }
    CHECK(!cpu_policy_bootstrap_map(NULL,sizeof(tables),0x1000,0x1000));
    CHECK(!cpu_policy_bootstrap_map(&tables[0][0],sizeof(tables)-1,0x1000,0x1000));
    CHECK(!cpu_policy_bootstrap_map(&tables[0][0],sizeof(tables),0x1000,0x1018));
}
static void sequence(void)
{
    cpu_policy_features_t f=features(true);cpu_policy_t p;CHECK(cpu_policy_build(&f,&p)==CPU_POLICY_OK);
    cpu_policy_readback_t out, sentinel;memset(&sentinel,0xa5,sizeof(sentinel));
    for(unsigned enabled=0;enabled<2;enabled++){
        reset(enabled?0x202:2);out=sentinel;
        CHECK(cpu_policy_apply(&p,&f,0x12345000,0x1d,&out)==CPU_POLICY_OK);
        CHECK(!strcmp(trace,"saderpAWEDRPWDAaderpi"));
        CHECK(flags==(enabled?0x202u:2u));CHECK(cr[0]==0x80010033&&cr[4]==0x20&&cr[3]==0x12345000);
        CHECK(cpu_policy_match(&p,&f,&out,0x12345000,7)==CPU_POLICY_OK);
    }
    reset(0x202);cr[4]|=0x80;CHECK(cpu_policy_apply(&p,&f,cr[3],0x1d,&out)==CPU_POLICY_OK);CHECK(cr[4]==0x20);
    for(unsigned bit=0;bit<64;bit++)if(!(0x6e8ull&(1ull<<bit))){
        reset(0x202);cr[4]|=1ull<<bit;out=sentinel;
        CHECK(cpu_policy_apply(&p,&f,cr[3],0x1d,&out)==CPU_POLICY_INCOMING);
        CHECK(writes==0&&!strchr(trace,'A')&&flags==0x202);CHECK(!memcmp(&out,&sentinel,sizeof(out)));
    }
    reset(0x202);out=sentinel;CHECK(cpu_policy_apply(&p,&f,cr[3],0x20,&out)==CPU_POLICY_MAPPING);CHECK(writes==0&&flags==0x202);
    reset(0x202);f.basic_edx=0;CHECK(cpu_policy_apply(&p,&f,cr[3],1,&out)==CPU_POLICY_FEATURES);CHECK(writes==0&&reads==0&&flags==0x202);f=features(true);
    for(unsigned nth=1;nth<=4;nth++){
        reset(0x202);fail_read=nth;out=sentinel;
        CHECK(cpu_policy_apply(&p,&f,cr[3],0x1d,&out)==CPU_POLICY_READ);
        CHECK(flags==0x202&&cr[0]==0x80010033);CHECK(!memcmp(&out,&sentinel,sizeof(out)));
    }
    for(unsigned nth=1;nth<=2;nth++){
        reset(0x202);fail_write=nth;out=sentinel;
        CHECK(cpu_policy_apply(&p,&f,cr[3],0x1d,&out)==CPU_POLICY_WRITE);
        CHECK(flags==0x202&&cr[0]==0x80010033);CHECK(!memcmp(&out,&sentinel,sizeof(out)));
        CHECK(strstr(trace,"WDA")!=NULL);
    }
    reset(0x202);corrupt_readback=true;out=sentinel;
    CHECK(cpu_policy_apply(&p,&f,cr[3],0x1d,&out)==CPU_POLICY_READBACK);CHECK(flags==0x202);CHECK(!memcmp(&out,&sentinel,sizeof(out)));
    f=features(false);CHECK(cpu_policy_build(&f,&p)==CPU_POLICY_OK);reset(2);efer=0x500;
    CHECK(cpu_policy_apply(&p,&f,cr[3],0x1d,&out)==CPU_POLICY_OK);CHECK(efer==0x500);
}
int main(void)
{
    decisions();encodings();bootstrap();sequence();
    printf("CPU_POLICY_HOST status=PASS checks=%u privileged_operations=SUBSTITUTED\n",checks);
    return 0;
}
