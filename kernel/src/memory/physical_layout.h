#ifndef PHYSICAL_LAYOUT_H
#define PHYSICAL_LAYOUT_H
#include <stdint.h>

/* C-side contract for the unchanged entry tables and linked image. */
#define PHYSICAL_ENTRY_LIMIT UINT64_C(0x100000000)
#define KERNEL_IMAGE_OFFSET UINT64_C(0xffffffff80000000)
#define PHYSICAL_PAGE_BYTES UINT64_C(4096)
#define PHYSICAL_ADDRESS_LIMIT (UINT64_C(1) << 48)
#define RAM_DIRECT_BASE UINT64_C(0xffff800000000000)
#define RAM_DIRECT_BYTES UINT64_C(0x0000400000000000)
#define RAM_DIRECT_END (RAM_DIRECT_BASE + RAM_DIRECT_BYTES)
#define FUTURE_STACK_BASE UINT64_C(0xffffe00000000000)
_Static_assert(RAM_DIRECT_END == UINT64_C(0xffffc00000000000), "RAM reservation ends before fixture windows");
#endif
