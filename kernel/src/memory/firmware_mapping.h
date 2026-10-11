#ifndef FIRMWARE_MAPPING_H
#define FIRMWARE_MAPPING_H
#include "owned_mapping.h"

/* WB, read-only managed CPU view of retained firmware bytes. Validate a fixed
 * header through one view before acquiring its declared full extent. Aliased
 * physical pages keep independent view ownership and identical cache types.
 * The existing identity alias is not claimed to be globally read-only.
 */
mapping_status_t firmware_mapping_acquire(uint64_t physical, size_t bytes,
                                          mapping_view_t *out);
#endif
