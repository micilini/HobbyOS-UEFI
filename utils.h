#ifndef UTILS_H
#define UTILS_H

#include <efi.h>
#include <efilib.h>

#define SCAN_NULL       0x0000
#define SCAN_UP         0x0001
#define SCAN_DOWN       0x0002
#define SCAN_RIGHT      0x0003
#define SCAN_LEFT       0x0004
#define SCAN_HOME       0x0005
#define SCAN_END        0x0006
#define SCAN_INSERT     0x0007
#define SCAN_DELETE     0x0008
#define SCAN_PAGE_UP    0x0009
#define SCAN_PAGE_DOWN  0x000A


#define COLOR_RESET   EFI_LIGHTGRAY
#define COLOR_CYAN    EFI_LIGHTCYAN
#define COLOR_RED     EFI_LIGHTRED
#define COLOR_GREEN   EFI_LIGHTGREEN
#define COLOR_WHITE   EFI_WHITE
#define COLOR_YELLOW  EFI_YELLOW


void SetColor(EFI_SYSTEM_TABLE *SystemTable, UINTN Attribute);
void HistoryAdd(CHAR16* Cmd);
void HobbyInput(EFI_SYSTEM_TABLE *SystemTable, CHAR16* Buffer, UINTN BufferSize);
INTN StrCmpCustom(CHAR16* s1, CHAR16* s2);
UINTN HobbyAtoi(CHAR16 *Str);
void SetCursorPos(EFI_SYSTEM_TABLE *SystemTable, UINTN Col, UINTN Row);
void GetCursorPos(EFI_SYSTEM_TABLE *SystemTable, UINTN *Col, UINTN *Row);

#endif