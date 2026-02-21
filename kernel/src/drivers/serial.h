#pragma once
#include <stdint.h>
#include "../../../shared/protocol.h"

void serial_init_from_bootinfo(const BootInfo *boot_info);

void serial_putc_all(char c);
void serial_write_all(const char *s);
void serial_write_hex64_all(uint64_t v);
