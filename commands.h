#ifndef COMMANDS_H
#define COMMANDS_H

#include <efi.h>
#include <efilib.h>

void cmd_ver(EFI_SYSTEM_TABLE *SystemTable);
void cmd_shutdown(EFI_SYSTEM_TABLE *SystemTable);
void cmd_restart(EFI_SYSTEM_TABLE *SystemTable);
void print_header(EFI_SYSTEM_TABLE *SystemTable);
void cmd_date(EFI_SYSTEM_TABLE *SystemTable);
void cmd_mem(EFI_SYSTEM_TABLE *SystemTable);
void cmd_hexdump(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName);

#endif