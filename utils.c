#include "utils.h"

#define MAX_HISTORY 10
#define MAX_CMD_SIZE 256

static CHAR16 gHistory[MAX_HISTORY][MAX_CMD_SIZE];
static UINTN  gHistoryCount = 0;
static INTN   gHistoryViewIdx = -1;

void HistoryAdd(CHAR16* Cmd) {
    if (Cmd[0] == 0) return;

    if (gHistoryCount > 0 && StrCmpCustom(Cmd, gHistory[gHistoryCount-1]) == 0) {
        gHistoryViewIdx = -1;
        return;
    }

    if (gHistoryCount >= MAX_HISTORY) {
        for (UINTN i = 0; i < MAX_HISTORY - 1; i++) {
            StrCpy(gHistory[i], gHistory[i+1]);
        }
        gHistoryCount = MAX_HISTORY - 1;
    }

    StrCpy(gHistory[gHistoryCount], Cmd);
    gHistoryCount++;
    
    gHistoryViewIdx = -1;
}

void SetColor(EFI_SYSTEM_TABLE *SystemTable, UINTN Attribute) {
    uefi_call_wrapper(SystemTable->ConOut->SetAttribute, 2, SystemTable->ConOut, Attribute);
}

INTN StrCmpCustom(CHAR16* s1, CHAR16* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(INT16*)s1 - *(INT16*)s2;
}

void GetCursorPos(EFI_SYSTEM_TABLE *SystemTable, UINTN *Col, UINTN *Row) {
    *Col = SystemTable->ConOut->Mode->CursorColumn;
    *Row = SystemTable->ConOut->Mode->CursorRow;
}

void SetCursorPos(EFI_SYSTEM_TABLE *SystemTable, UINTN Col, UINTN Row) {
    uefi_call_wrapper(SystemTable->ConOut->SetCursorPosition, 3, SystemTable->ConOut, Col, Row);
}

void HobbyInput(EFI_SYSTEM_TABLE *SystemTable, CHAR16* Buffer, UINTN BufferSize) {
    EFI_INPUT_KEY Key;
    UINTN EventIndex;
    
    UINTN Index = 0;      
    UINTN MaxIndex = 0;   
    
    UINTN StartCol, StartRow;
    GetCursorPos(SystemTable, &StartCol, &StartRow);

    gHistoryViewIdx = -1; 

    uefi_call_wrapper(SystemTable->ConOut->EnableCursor, 2, SystemTable->ConOut, TRUE);

    while (1) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3, 1, &SystemTable->ConIn->WaitForKey, &EventIndex);
        uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &Key);

        if (Key.UnicodeChar == L'\r') {
            Print(L"\n");
            Buffer[MaxIndex] = L'\0';
            break;
        }
        
        else if (Key.UnicodeChar == L'\b') {
            if (Index > 0) {
                for (UINTN i = Index - 1; i < MaxIndex; i++) Buffer[i] = Buffer[i + 1];
                Index--;
                MaxIndex--;
                
                SetCursorPos(SystemTable, StartCol, StartRow);
                for (UINTN i = 0; i < MaxIndex; i++) {
                    CHAR16 tmp[2] = {Buffer[i], L'\0'};
                    Print(tmp);
                }
                Print(L" "); 
                SetCursorPos(SystemTable, StartCol + Index, StartRow);
            }
        }

        else if (Key.ScanCode == SCAN_UP) {
            if (gHistoryCount > 0) {
                if (gHistoryViewIdx == -1) gHistoryViewIdx = gHistoryCount - 1;
                else if (gHistoryViewIdx > 0) gHistoryViewIdx--;

                SetCursorPos(SystemTable, StartCol, StartRow);
                for(UINTN k=0; k<MaxIndex; k++) Print(L" ");
                
                StrCpy(Buffer, gHistory[gHistoryViewIdx]);
                MaxIndex = StrLen(Buffer);
                Index = MaxIndex;

                SetCursorPos(SystemTable, StartCol, StartRow);
                Print(Buffer);
            }
        }

        else if (Key.ScanCode == SCAN_DOWN) {

            if (gHistoryViewIdx != -1) {
                gHistoryViewIdx++;


                SetCursorPos(SystemTable, StartCol, StartRow);
                for(UINTN k=0; k<MaxIndex; k++) Print(L" ");


                if (gHistoryViewIdx >= (INTN)gHistoryCount) {
                    gHistoryViewIdx = -1;
                    MaxIndex = 0;
                    Index = 0;
                    Buffer[0] = 0; 
                } else {

                    StrCpy(Buffer, gHistory[gHistoryViewIdx]);
                    MaxIndex = StrLen(Buffer);
                    Index = MaxIndex;
                }

                SetCursorPos(SystemTable, StartCol, StartRow);
                if (MaxIndex > 0) Print(Buffer);
            }
        }
        
        else if (Key.ScanCode == SCAN_LEFT) {
            if (Index > 0) {
                Index--;
                SetCursorPos(SystemTable, StartCol + Index, StartRow);
            }
        }
        
        else if (Key.ScanCode == SCAN_RIGHT) {
            if (Index < MaxIndex) {
                Index++;
                SetCursorPos(SystemTable, StartCol + Index, StartRow);
            }
        }

        else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
            if (MaxIndex < BufferSize - 1) {
                for (UINTN i = MaxIndex; i > Index; i--) Buffer[i] = Buffer[i - 1];
                
                Buffer[Index] = Key.UnicodeChar;
                Index++;
                MaxIndex++;

                SetCursorPos(SystemTable, StartCol, StartRow);
                for (UINTN i = 0; i < MaxIndex; i++) {
                    CHAR16 tmp[2] = {Buffer[i], L'\0'};
                    Print(tmp);
                }
                SetCursorPos(SystemTable, StartCol + Index, StartRow);
            }
        }
    }
}

UINTN HobbyAtoi(CHAR16 *Str) {
    UINTN Num = 0;
    while (*Str >= '0' && *Str <= '9') {
        Num = Num * 10 + (*Str - '0');
        Str++;
    }
    return Num;
}