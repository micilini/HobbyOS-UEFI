#ifndef OWNED_MAPPING_H
#define OWNED_MAPPING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cache_attributes.h"

typedef enum {
    MAPPING_OK, MAPPING_INVALID, MAPPING_CONTEXT, MAPPING_BUSY,
    MAPPING_EXHAUSTED, MAPPING_CONFLICT, MAPPING_NO_MEMORY,
    MAPPING_STALE, MAPPING_QUARANTINED, MAPPING_UNSUPPORTED
} mapping_status_t;
typedef struct { uint64_t generation; uint32_t slot, domain; } mapping_handle_t;
typedef struct {
    mapping_handle_t handle;
    void *cpu;
    uint64_t physical;
    size_t bytes;
} mapping_view_t;

#define DEVICE_WINDOW_BASE UINT64_C(0xffffd00000000000)
#define DEVICE_WINDOW_BYTES UINT64_C(0x40000000)
#define OWNED_MAPPING_CAPACITY 512u

/* Nonempty byte ranges; rounding and the whole physical extent are checked.
 * A view owns only its managed VA, never the underlying device/firmware bytes.
 * Compatible physical overlaps get independent VA reservations. Releasing one
 * cannot remove another view. Outputs are unchanged on failure.
 * After boot sealing, acquire/release require eligible IF-enabled thread
 * context without target-needed locks. Failed invalidation quarantines the
 * handle and VA for the boot. Success is the only reuse boundary.
 */
mapping_status_t owned_mapping_release(mapping_handle_t handle);
bool owned_mapping_subview(mapping_handle_t handle, size_t offset, size_t bytes,
                            void **cpu);
bool owned_mapping_physical(const void *cpu, size_t bytes, uint64_t *physical);
void owned_mapping_close_bootstrap(void);
/* Copy-only inspection of an existing device view; this never acquires one. */
bool owned_mapping_device_view(uint64_t physical, size_t bytes, mapping_view_t *out);

#endif
