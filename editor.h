#ifndef EDITOR_H
#define EDITOR_H

#include <efi.h>
#include <efilib.h>

void cmd_edit(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName);

#endif