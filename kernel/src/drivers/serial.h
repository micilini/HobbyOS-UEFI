#pragma once
#include <stdint.h>
#include "../../../shared/protocol.h"

void serial_init_from_bootinfo(const BootInfo *boot_info);

void serial_putc_all(char c);
void serial_write_all(const char *s);
void serial_write_hex64_all(uint64_t v);

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
#include <stdbool.h>
#include <stddef.h>
/* Test-only exclusive export. Normal diagnostics are retained until close;
 * panic output always bypasses this ownership and invalidates the export. */
bool serial_proof_begin(uint64_t session);
bool serial_proof_write(uint64_t session, const void *bytes, size_t length);
/* At a complete proof-record boundary, export pending ordinary bytes in
 * length/digest-bound auxiliary frames without releasing UART ownership. */
bool serial_proof_flush(uint64_t session);
bool serial_proof_end(uint64_t session);
bool serial_proof_record(const void *bytes, size_t length);
void serial_proof_aux_totals(uint64_t *records, uint64_t *bytes);
uint32_t serial_proof_failure_mask(void);
#endif
