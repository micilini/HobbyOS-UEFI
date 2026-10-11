#include "cache_attributes.h"

bool cache_attribute_encode(const cpu_policy_t *p, cache_attribute_t type,
                             cache_leaf_t leaf, uint64_t *out)
{
    if (!p || !out || (leaf != CACHE_LEAF_4K && leaf != CACHE_LEAF_2M &&
                      leaf != CACHE_LEAF_1G)) return false;
    cpu_policy_features_t declared = {.basic_edx = p->required_basic,
        .extended_edx = p->required_extended | (p->page1g_supported ? CPU_FEATURE_PAGE1G : 0), .basic_valid = true,
        .extended_valid = true, .identity_valid = true};
    if (cpu_policy_validate(p, &declared) != CPU_POLICY_OK) return false;
    if (leaf == CACHE_LEAF_1G && !p->page1g_supported) return false;
    unsigned slot;
    switch (type) {
        case CACHE_WRITE_BACK: slot = 0; break;
        case CACHE_WRITE_COMBINING: slot = 5; break;
        case CACHE_UNCACHED_MINUS: slot = 2; break;
        case CACHE_UNCACHEABLE: slot = 3; break;
        default: return false;
    }
    *out = ((uint64_t)(slot & 3) << 3) |
           ((uint64_t)(slot >> 2) << (leaf == CACHE_LEAF_4K ? 7 : 12));
    return true;
}
