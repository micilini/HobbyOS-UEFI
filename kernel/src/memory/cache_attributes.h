#ifndef MEMORY_CACHE_ATTRIBUTES_H
#define MEMORY_CACHE_ATTRIBUTES_H
#include "../cpu/control_policy.h"

typedef enum {
    CACHE_WRITE_BACK, CACHE_WRITE_COMBINING, CACHE_UNCACHED_MINUS,
    CACHE_UNCACHEABLE
} cache_attribute_t;
typedef enum {
    CACHE_LEAF_4K = 12, CACHE_LEAF_2M = 21, CACHE_LEAF_1G = 30
} cache_leaf_t;

/* Selection bits only. A successful WB encoding is zero. Failure leaves out
 * unchanged. The caller owns mapping, permissions and the large-leaf PS bit. */
bool cache_attribute_encode(const cpu_policy_t *policy,
                             cache_attribute_t type, cache_leaf_t leaf,
                             uint64_t *out);
#endif
