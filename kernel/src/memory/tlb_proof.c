#include "tlb_proof.h"

bool tlb_proof_open(tlb_proof_t *p, unsigned char *data, size_t capacity, uint64_t session)
{
    if (!p || p->state != TLB_PROOF_UNUSED || !data || !capacity || !session) return false;
    *p = (tlb_proof_t){.data=data, .capacity=capacity, .session=session,
        .digest=TLB_PROOF_FNV_OFFSET, .state=TLB_PROOF_CAPTURING};
    return true;
}

bool tlb_proof_append(tlb_proof_t *p, const char *line, size_t bytes)
{
    if (!p) return false;
    if (p->failed || p->state != TLB_PROOF_CAPTURING || !line || !bytes ||
        bytes > TLB_PROOF_LINE_BYTES || p->bytes > p->capacity ||
        bytes > p->capacity-p->bytes || p->records == UINT64_MAX) {
        p->failed=true;
        return false;
    }
    size_t start=p->bytes;
    uint64_t digest=p->digest;
    for (size_t i=0; i<bytes; ++i) {
        unsigned char value=(unsigned char)line[i];
        p->data[start+i]=value;
        digest=(digest^value)*TLB_PROOF_FNV_PRIME;
    }
    p->digest=digest;
    p->bytes=start+bytes;
    ++p->records;
    return true;
}

bool tlb_proof_seal(tlb_proof_t *p, uint64_t session, bool writers_idle, uint64_t service)
{
    if (!p || p->failed || p->state!=TLB_PROOF_CAPTURING || !writers_idle ||
        session!=p->session || !p->records || !p->bytes || !service) return false;
    p->service_generation=service;
    p->state=TLB_PROOF_SEALED;
    return true;
}

bool tlb_proof_claim(tlb_proof_t *p, uint64_t session)
{
    if (!p || p->failed || p->state!=TLB_PROOF_SEALED || session!=p->session) return false;
    p->state=TLB_PROOF_EXPORTING;
    return true;
}

bool tlb_proof_finish(tlb_proof_t *p, uint64_t session, size_t bytes)
{
    if (!p || p->failed || p->state!=TLB_PROOF_EXPORTING || session!=p->session ||
        bytes!=p->bytes) return false;
    p->state=TLB_PROOF_EXPORTED;
    return true;
}
