#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../kernel/src/memory/tlb_proof.h"

static tlb_proof_t proof;
static unsigned char storage[1024 * 1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void reset(size_t capacity)
{
    memset(&proof, 0, sizeof(proof));
    memset(storage, 0xa5, sizeof(storage));
    assert(tlb_proof_open(&proof, storage, capacity, 17));
}
static void *writer(void *arg)
{
    for (unsigned i = 0; i < 100; ++i) {
        char line[64];
        int n = snprintf(line, sizeof(line), "[TLB] writer=%u record=%u\n", (unsigned)(uintptr_t)arg, i);
        assert(!pthread_mutex_lock(&lock));
        assert(tlb_proof_append(&proof, line, (size_t)n));
        assert(!pthread_mutex_unlock(&lock));
        memset(line, 0, sizeof(line));
    }
    return NULL;
}
int main(void)
{
    reset(4);
    assert(!tlb_proof_open(&proof, storage, 4, 17));
    assert(tlb_proof_append(&proof, "abc\n", 4));
    assert(proof.bytes == 4 && proof.records == 1 && storage[4] == 0xa5);
    assert(!tlb_proof_append(&proof, "x", 1));
    assert(proof.failed && proof.bytes == 4 && storage[4] == 0xa5);
    assert(!tlb_proof_seal(&proof, 17, true, 9));
    reset(4);
    assert(!tlb_proof_append(&proof, "abcde", 5));
    assert(proof.bytes == 0 && proof.records == 0 && storage[0] == 0xa5);
    reset(sizeof(storage));
    assert(!tlb_proof_seal(&proof, 17, false, 9));
    assert(!tlb_proof_claim(&proof, 17));
    assert(tlb_proof_append(&proof, "abc\n", 4));
    assert(!tlb_proof_seal(&proof, 18, true, 9));
    assert(tlb_proof_seal(&proof, 17, true, 9));
    assert(!tlb_proof_seal(&proof, 17, true, 9));
    assert(!tlb_proof_claim(&proof, 18));
    assert(tlb_proof_claim(&proof, 17));
    assert(!tlb_proof_claim(&proof, 17));
    assert(!tlb_proof_finish(&proof, 17, 3));
    assert(proof.state == TLB_PROOF_EXPORTING);
    assert(tlb_proof_finish(&proof, 17, 4));
    assert(!tlb_proof_finish(&proof, 17, 4));
    reset(sizeof(storage));
    pthread_t threads[8];
    for (unsigned i=0;i<8;++i) assert(!pthread_create(&threads[i],NULL,writer,(void *)(uintptr_t)i));
    for (unsigned i=0;i<8;++i) assert(!pthread_join(threads[i],NULL));
    assert(proof.records == 800 && tlb_proof_seal(&proof,17,true,123));
    bool seen[8][100]={{0}};
    size_t offset=0;
    while (offset<proof.bytes) {
        unsigned p,n; int length=0;
        assert(sscanf((char *)storage+offset,"[TLB] writer=%u record=%u\n%n",&p,&n,&length)==2);
        assert(p<8 && n<100 && !seen[p][n] && length>0);
        seen[p][n]=true;offset+=(unsigned)length;
    }
    uint64_t hash=TLB_PROOF_FNV_OFFSET;
    for(size_t i=0;i<proof.bytes;++i) hash=(hash^storage[i])*TLB_PROOF_FNV_PRIME;
    assert(hash==proof.digest && proof.service_generation==123);
    assert(!tlb_proof_append(&proof,"late\n",5) && proof.failed);
    assert(!tlb_proof_claim(&proof,17));
    puts("TLB proof actual-C bounded whole records and immutable ownership: PASS");
}
