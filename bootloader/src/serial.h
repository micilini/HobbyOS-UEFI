#pragma once
#include <efi.h>
#include <efilib.h>
#include <stdint.h>

#include "../../shared/protocol.h" 

void serial_discover_ports(BootInfo* boot_info);
void serial_broadcast_boot_banner(const BootInfo* boot_info);
void serial_try_init_and_write(uint16_t io_base, const char* msg);


void serial_write_all(const BootInfo* boot_info, const char* msg);
void serial_write_hex64_all(const BootInfo* boot_info, uint64_t value);
