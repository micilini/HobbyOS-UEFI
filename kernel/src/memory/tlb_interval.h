#ifndef TLB_INTERVAL_H
#define TLB_INTERVAL_H
#include <stdbool.h>
#include <stdint.h>

#define TLB_SMALL_PAGE_LIMIT 64u
typedef enum { TLB_EMPTY, TLB_PAGES, TLB_CONTEXT } tlb_strategy_t;
typedef struct {
    uint64_t first, last, pages;
    tlb_strategy_t strategy;
} tlb_interval_t;
/* start/length describes bytes. Nonempty intervals round outward to pages.
 * last is an inclusive page address, allowing the topmost canonical page.
 * Arithmetic overflow and the canonical hole are rejected without output writes.
 * An empty interval still requires a canonical start and performs no operation. */
bool tlb_interval_normalize(uint64_t start, uint64_t length, tlb_interval_t *out);
bool tlb_interval_valid(const tlb_interval_t *interval);
typedef struct {
    uint64_t (*read_cr3)(void *);
    uint64_t (*read_cr4)(void *);
    void (*invalidate_page)(void *, uint64_t);
    void (*reload_cr3)(void *, uint64_t);
} tlb_local_ops_t;
bool tlb_local_execute(const tlb_interval_t *interval, uint64_t root,
                       const tlb_local_ops_t *ops, void *context);
#endif
