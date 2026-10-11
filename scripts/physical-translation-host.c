#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/physical_internal.h"
#include "../kernel/src/utils/bitmap.h"

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static physical_contract_t contract;

static void translation(void)
{
    EfiMemoryDescriptor descriptors[] = {
        {.Type=7,.PhysicalStart=0,.NumberOfPages=0x80000},
        {.Type=7,.PhysicalStart=0x80000000,.NumberOfPages=0x70000},
        {.Type=11,.PhysicalStart=0xf0000000,.NumberOfPages=0x10000},
        {.Type=7,.PhysicalStart=0x100000000,.NumberOfPages=16},
        {.Type=0,.PhysicalStart=0xffffffff000,.NumberOfPages=1}
    };
    MemoryMap map={descriptors,sizeof(descriptors),sizeof(*descriptors),17};
    CHECK(physical_contract_init(&contract,&map,0xffffffff82000000ULL,0xffffffff83000000ULL,1));
    CHECK(contract.identity_limit==0x100000000ULL && contract.allocation_ceiling==0x100000000ULL);
    const uint64_t edges[]={0,1,4095,4096,0x3fffffff,0x40000000,0x7fffffff,
        0x80000000,0x80001000,0xefffffff,0xf0000000,0xffffffff,0x100000000};
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);++i) {
        uint64_t out=0xfeed;
        bool good=edges[i]<0x100000000ULL;
        CHECK(physical_contract_translate(&contract,edges[i],1,PHYSICAL_KNOWN,&out)==good);
        CHECK(out==(good?edges[i]:0xfeed));
    }
    uint64_t out=7;
    CHECK(physical_contract_translate(&contract,0x80001000,4096,PHYSICAL_RAM,&out) && out==0x80001000);
    uint64_t old=(0x80001000ULL|0xffffffff80000000ULL)&~0xffffffff80000000ULL;
    CHECK(old==4096 && out!=old);
    CHECK(!physical_contract_translate(&contract,0xffffffff,2,PHYSICAL_KNOWN,&out));
    CHECK(!physical_contract_translate(&contract,0xf0000000,1,PHYSICAL_RAM,&out));
    CHECK(physical_contract_translate(&contract,0xf0000000,4096,PHYSICAL_DEVICE,&out));
    CHECK(!physical_contract_translate(&contract,0x100000000ULL,4096,PHYSICAL_RAM,&out));
    CHECK(!physical_contract_translate(&contract,0,0,PHYSICAL_RAM,&out));
    CHECK(!physical_contract_translate(&contract,UINT64_MAX,4096,PHYSICAL_RAM,&out));
    CHECK(!physical_contract_translate(&contract,0,1,(physical_kind_t)123,&out));
    CHECK(physical_contract_inverse(&contract,0xffffffff82000000ULL,0x1000000,&out) && out==0x2000000);
    CHECK(!physical_contract_inverse(&contract,0xffffffff83000000ULL,1,&out));
    CHECK(!physical_contract_inverse(&contract,0xffffffff80001000ULL,1,&out));
    CHECK(!physical_contract_inverse(&contract,0xffffc000001ff000ULL,1,&out));
    CHECK(physical_contract_inverse(&contract,0x80001000,4096,&out) && out==0x80001000);
    CHECK(!physical_canonical_range(0x7fffffffffffULL,2));
    CHECK(!physical_canonical_range(0x800000000000ULL,1));
    CHECK(physical_canonical_range(UINT64_MAX-4095,4096));
    CHECK(!physical_canonical_range(UINT64_MAX-4095,4097));
    physical_borrow_t borrow={4096,4096,1};
    CHECK(physical_contract_borrow(&contract,&borrow,PHYSICAL_RAM,&out));
    contract.epoch=2;
    CHECK(!physical_contract_borrow(&contract,&borrow,PHYSICAL_RAM,&out));
    contract.epoch=1;
    CHECK(!physical_contract_init(&contract,&map,0xffffffff82000000ULL,0xffffffff83000000ULL,2));
    physical_contract_t *bad=calloc(1,sizeof(*bad));CHECK(bad);
    descriptors[3].NumberOfPages=UINT64_MAX;
    CHECK(!physical_contract_init(bad,&map,0xffffffff82000000ULL,0xffffffff83000000ULL,1) && !bad->ready);
    descriptors[3].NumberOfPages=16;descriptors[3].PhysicalStart=4096;
    CHECK(!physical_contract_init(bad,&map,0xffffffff82000000ULL,0xffffffff83000000ULL,1) && !bad->ready);
    free(bad);
}

static bool oracle(const Bitmap *b,size_t start,size_t limit,size_t count,size_t *out)
{
    if(!count || start>limit || limit>b->total_bits || count>limit-start)return false;
    for(size_t i=start;i<=limit-count;++i) {
        size_t j=0;for(;j<count;++j)if(bitmap_get(b,i+j))break;
        if(j==count){*out=i;return true;}
    }
    return false;
}
static void bounded_search(void)
{
    uint8_t bytes[40],before[40];uint64_t random=0x834710bf;
    const size_t sizes[]={0,1,7,8,9,63,64,65,127,128,129,257};
    for(size_t n=0;n<sizeof(sizes)/sizeof(*sizes);++n) {
        size_t bits=sizes[n],size=(bits+7)/8;Bitmap b={bytes+1,size,bits};
        for(unsigned round=0;round<120;++round) {
            for(size_t i=0;i<sizeof(bytes);++i){random^=random<<13;random^=random>>7;random^=random<<17;bytes[i]=(uint8_t)random;}
            memcpy(before,bytes,sizeof(bytes));
            for(size_t limit=0;limit<=bits;++limit) {
                size_t start=round%(bits+1),count=round%70+1,out=123456,expected=123456;
                BitmapSearchStats stats={123};
                bool good=oracle(&b,start,limit,count,&expected);
                CHECK(bitmap_find_zero_run_below(&b,start,limit,count,&out,&stats)==good);
                CHECK(out==expected && !memcmp(bytes,before,sizeof(bytes)));
                CHECK(stats.words_examined<=limit/64+(limit%64!=0));
            }
        }
    }
    size_t out=99;Bitmap bad={NULL,1,8};
    CHECK(!bitmap_find_zero_run_below(&bad,0,8,1,&out,NULL) && out==99);
    bad=(Bitmap){bytes,0,8};CHECK(!bitmap_find_zero_run_below(&bad,0,8,1,&out,NULL));
    bad=(Bitmap){bytes,1,8};CHECK(!bitmap_find_zero_run_below(&bad,0,9,1,&out,NULL));
    CHECK(!bitmap_find_zero_run_below(&bad,0,8,SIZE_MAX,&out,NULL));
    size_t bits=268435456,span=bits/8;
    uint8_t *full=malloc(span),*reserved=malloc(span);CHECK(full && reserved);
    memset(full,255,span);memset(reserved,255,span);
    Bitmap a={full,span,bits},r={reserved,span,bits};
    bitmap_set(&a,bits-1,false);bitmap_set(&r,bits-1,false);
    size_t free_bits=0;
    CHECK(!bitmap_find_zero_run_below(&a,256,0x100000,1,&out,NULL));
    CHECK(bitmap_validate_allocation(&a,&r,&free_bits,NULL) && free_bits==1);
    bitmap_set(&r,bits-1,true);
    CHECK(!bitmap_validate_allocation(&a,&r,&free_bits,NULL) && free_bits==1);
    free(full);free(reserved);
}
int main(void)
{
    translation();bounded_search();
    printf("PHYSICAL_TRANSLATION_HOST status=PASS checks=%u privileged_operations=NONE\n",checks);
}
