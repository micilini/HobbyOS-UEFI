#ifndef ACPI_SLEEP_H
#define ACPI_SLEEP_H

#include <stdint.h>
#include <stdbool.h>

bool acpi_get_s5_sleep_types(uint8_t *out_typa, uint8_t *out_typb);

#endif