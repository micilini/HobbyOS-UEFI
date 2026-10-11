#ifndef ACPI_WDAT_H
#define ACPI_WDAT_H

#include <stdint.h>
#include <stdbool.h>

int acpi_wdat_disable(void);
bool acpi_wdat_discovered_extent(uint64_t source, uint64_t *physical, uint64_t *bytes);

#endif