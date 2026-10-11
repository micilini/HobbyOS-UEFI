#ifndef TLB_PROOF_H
#define TLB_PROOF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TLB_PROOF_LINE_BYTES 766u
#define TLB_PROOF_FNV_OFFSET UINT64_C(14695981039346656037)
#define TLB_PROOF_FNV_PRIME UINT64_C(1099511628211)
typedef enum {
    TLB_PROOF_UNUSED, TLB_PROOF_CAPTURING, TLB_PROOF_SEALED,
    TLB_PROOF_EXPORTING, TLB_PROOF_EXPORTED
} tlb_proof_state_t;
typedef struct {
    unsigned char *data;
    size_t capacity, bytes;
    uint64_t records, digest, session, service_generation;
    tlb_proof_state_t state;
    bool failed;
} tlb_proof_t;

/* The fixture serializes these bounded transitions with its trace lock.
 * Only the exclusive exporter may read data after claim; no transition then
 * changes data, length or digest. Failed writes never publish partial bytes. */
bool tlb_proof_open(tlb_proof_t *, unsigned char *, size_t, uint64_t);
bool tlb_proof_append(tlb_proof_t *, const char *, size_t);
bool tlb_proof_seal(tlb_proof_t *, uint64_t, bool, uint64_t);
bool tlb_proof_claim(tlb_proof_t *, uint64_t);
bool tlb_proof_finish(tlb_proof_t *, uint64_t, size_t);
#endif
