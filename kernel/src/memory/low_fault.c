#include "low_fault.h"
const char *low_fault_description(uint64_t address)
{
    if (!address) return "Page Fault: NULL-address access";
    if (address<UINT64_C(0x0000800000000000))
        return "Page Fault: low-address access; a physical address may have been used as a CPU pointer";
    return "Page Fault";
}
