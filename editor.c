#include "editor.h"
#include "utils.h"
#include "filesystem.h"
#include "commands.h"

#define SCREEN_ROWS 25
#define SCREEN_COLS 80

#define EDIT_ROWS 22 
#define EDIT_COLS 78 

#define MAX_FILE_LINES 100

#define OFFSET_X 1
#define OFFSET_Y 1

static CHAR16 TextBuffer[MAX_FILE_LINES][EDIT_COLS + 1];

static UINTN  CursorX = 0;
static UINTN  CursorY = 0;

static UINTN  ScrollOffset = 0; 
static BOOLEAN IsDirty = FALSE;

void DrawBorder(EFI_SYSTEM_TABLE *SystemTable) {
    SetColor(SystemTable, EFI_LIGHTGRAY);
    
    SetCursorPos(SystemTable, 0, 0);
    Print(L"+");
    for(int i=0; i<EDIT_COLS; i++) Print(L"-");
    Print(L"+");

    for(int i=1; i <= EDIT_ROWS; i++) {
        SetCursorPos(SystemTable, 0, i);
        Print(L"|");
        SetCursorPos(SystemTable, EDIT_COLS + 1, i);
        Print(L"|");
    }

    SetCursorPos(SystemTable, 0, EDIT_ROWS + 1);
    Print(L"+");
    for(int i=0; i<EDIT_COLS; i++) Print(L"-");
    Print(L"+");
}

void DrawStatusBar(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName, CHAR16* StatusMsg) {
    SetColor(SystemTable, COLOR_WHITE);
    SetCursorPos(SystemTable, 0, EDIT_ROWS + 2);
    Print(L"                                                                                ");
    SetCursorPos(SystemTable, 0, EDIT_ROWS + 3);
    Print(L"                                                                                ");

    SetColor(SystemTable, COLOR_CYAN);
    SetCursorPos(SystemTable, 0, EDIT_ROWS + 2);
    Print(L" FILE: %s  Lin: %d/%d  Col: %d", FileName, (int)CursorY+1, EDIT_ROWS, (int)CursorX+1);
    
    SetColor(SystemTable, COLOR_WHITE);
    SetCursorPos(SystemTable, 0, EDIT_ROWS + 3);
    Print(L" ^X Exit  ^C Save   Status: %s", StatusMsg);
}

void RedrawCurrentLine(EFI_SYSTEM_TABLE *SystemTable) {
    if (CursorY < ScrollOffset || CursorY >= ScrollOffset + EDIT_ROWS) return;

    UINTN ScreenRow = CursorY - ScrollOffset;

    SetCursorPos(SystemTable, OFFSET_X, OFFSET_Y + ScreenRow);
    for(int i=0; i<EDIT_COLS; i++) Print(L" ");
    
    SetCursorPos(SystemTable, OFFSET_X, OFFSET_Y + ScreenRow);
    Print(L"%s", TextBuffer[CursorY]);
}

void DrawInterface(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName, CHAR16* StatusMsg) {
    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    DrawBorder(SystemTable);
    
    SetColor(SystemTable, COLOR_WHITE);
    
    for (int r = 0; r < EDIT_ROWS; r++) {
        UINTN FileRow = ScrollOffset + r;
        if (FileRow >= MAX_FILE_LINES) break;

        SetCursorPos(SystemTable, OFFSET_X, OFFSET_Y + r);
        Print(L"%s", TextBuffer[FileRow]);
    }

    DrawStatusBar(SystemTable, FileName, StatusMsg);
    
    if (CursorY >= ScrollOffset && CursorY < ScrollOffset + EDIT_ROWS) {
        SetCursorPos(SystemTable, OFFSET_X + CursorX, OFFSET_Y + (CursorY - ScrollOffset));
    }
}

void ClearEditorBuffer() {
    for (int r = 0; r < MAX_FILE_LINES; r++) {
        for (int c = 0; c <= EDIT_COLS; c++) {
            TextBuffer[r][c] = 0;
        }
    }
    CursorX = 0;
    CursorY = 0;
    ScrollOffset = 0;
    IsDirty = FALSE;
}

void InsertChar(CHAR16 Char) {
    if (CursorX >= EDIT_COLS - 1) {
        if (CursorY < MAX_FILE_LINES - 1) {
            CursorY++;
            CursorX = 0;
        } else {
            return;
        }
    }

    UINTN Len = StrLen(TextBuffer[CursorY]);
    if (CursorX > Len) {
        for (UINTN i = Len; i < CursorX; i++) TextBuffer[CursorY][i] = L' ';
        TextBuffer[CursorY][CursorX] = 0;
    }

    for (INTN i = EDIT_COLS - 1; i > (INTN)CursorX; i--) {
        TextBuffer[CursorY][i] = TextBuffer[CursorY][i-1];
    }

    TextBuffer[CursorY][CursorX] = Char;
    TextBuffer[CursorY][EDIT_COLS] = 0; 

    IsDirty = TRUE;
}

void DeleteChar() {
    if (CursorX == 0) {
        return; 
    }

    for (UINTN i = CursorX - 1; i < EDIT_COLS; i++) {
        TextBuffer[CursorY][i] = TextBuffer[CursorY][i+1];
    }
    CursorX--;
    IsDirty = TRUE;
}

EFI_STATUS SaveFile(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FullPath) {
    EFI_FILE_PROTOCOL *Root = fs_get_root();
    if (!Root) return EFI_NOT_READY;

    EFI_FILE_PROTOCOL *FileHandle;
    EFI_STATUS Status;

    Status = uefi_call_wrapper(Root->Open, 5, Root, &FileHandle, FullPath, 
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(Status)) uefi_call_wrapper(FileHandle->Delete, 1, FileHandle);

    Status = uefi_call_wrapper(Root->Open, 5, Root, &FileHandle, FullPath, 
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(Status)) return Status;

    int LastActiveRow = -1;
    for (int r = MAX_FILE_LINES - 1; r >= 0; r--) {
        if (StrLen(TextBuffer[r]) > 0) {
            LastActiveRow = r;
            break;
        }
    }

    for (int r = 0; r <= LastActiveRow; r++) {
        UINTN Len = StrLen(TextBuffer[r]);
        
        UINT8 LineBuffer[EDIT_COLS * 2]; 
        UINTN WriteSize = 0;

        for(UINTN c=0; c<Len; c++) LineBuffer[c] = (UINT8)TextBuffer[r][c];
        WriteSize = Len;

        LineBuffer[WriteSize++] = '\r';
        LineBuffer[WriteSize++] = '\n';

        uefi_call_wrapper(FileHandle->Write, 3, FileHandle, &WriteSize, LineBuffer);
    }

    uefi_call_wrapper(FileHandle->Flush, 1, FileHandle);
    uefi_call_wrapper(FileHandle->Close, 1, FileHandle);

    IsDirty = FALSE;

    return EFI_SUCCESS;
}

void LoadFile(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FullPath) {
    ClearEditorBuffer();
    EFI_FILE_PROTOCOL *Root = fs_get_root();
    if (!Root) return;

    EFI_FILE_PROTOCOL *FileHandle;
    EFI_STATUS Status = uefi_call_wrapper(Root->Open, 5, Root, &FileHandle, FullPath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) return; 

    UINT8 CharBuf;
    UINTN ReadSize = 1;
    int r = 0, c = 0;

    while(1) {
        ReadSize = 1;
        Status = uefi_call_wrapper(FileHandle->Read, 3, FileHandle, &ReadSize, &CharBuf);
        if (EFI_ERROR(Status) || ReadSize == 0) break;

        CHAR16 UChar = (CHAR16)CharBuf;
        if (UChar == L'\r') continue;
        if (UChar == L'\n') {
            r++; c = 0;
            if (r >= EDIT_ROWS) break; 
            continue;
        }
        if (c < EDIT_COLS) {
            TextBuffer[r][c] = UChar;
            c++;
        }
    }
    uefi_call_wrapper(FileHandle->Close, 1, FileHandle);
}

void cmd_edit(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName) {
    if (FileName[0] == 0) {
        Print(L"Usage: edit <file>\n");
        return;
    }

    CHAR16 FullPath[256];
    ResolvePath(FileName, FullPath);
    LoadFile(SystemTable, FullPath);

    EFI_INPUT_KEY Key;
    UINTN EventIndex;
    BOOLEAN Running = TRUE;
    BOOLEAN ConfirmExit = FALSE;
    CHAR16 StatusMsg[60]; 
    StrCpy(StatusMsg, L"");

    uefi_call_wrapper(SystemTable->ConOut->EnableCursor, 2, SystemTable->ConOut, TRUE);
    DrawInterface(SystemTable, FileName, StatusMsg);

    while (Running) {
        uefi_call_wrapper(SystemTable->BootServices->WaitForEvent, 3, 1, &SystemTable->ConIn->WaitForKey, &EventIndex);
        uefi_call_wrapper(SystemTable->ConIn->ReadKeyStroke, 2, SystemTable->ConIn, &Key);

        if (StatusMsg[0] != 0 && !ConfirmExit) {
            StrCpy(StatusMsg, L"");
            DrawStatusBar(SystemTable, FileName, StatusMsg);
        }

        if (ConfirmExit) {
            if (Key.UnicodeChar == 's' || Key.UnicodeChar == 'S' || Key.UnicodeChar == 'y' || Key.UnicodeChar == 'Y') {
                Running = FALSE;
            } else {
                ConfirmExit = FALSE;
                StrCpy(StatusMsg, L"Exit Canceled.");
                DrawStatusBar(SystemTable, FileName, StatusMsg);
            }
            continue;
        }

        if (Key.UnicodeChar == 24) { 
            if (IsDirty) {
                ConfirmExit = TRUE;
                SetColor(SystemTable, COLOR_RED);
                StrCpy(StatusMsg, L"FILE MODIFIED! Exit without saving? (Y/N)");
                DrawStatusBar(SystemTable, FileName, StatusMsg);
            } else {
                Running = FALSE;
            }
        }

        else if (Key.UnicodeChar == 3) { 
            EFI_STATUS S = SaveFile(SystemTable, FullPath);
            if (EFI_ERROR(S)) StrCpy(StatusMsg, L"Error recording!");
            else StrCpy(StatusMsg, L"Recorded with success!");
            DrawStatusBar(SystemTable, FileName, StatusMsg);
        }

        else if (Key.ScanCode == SCAN_UP) {
            if (CursorY > 0) {
                CursorY--;
                if (CursorY < ScrollOffset) {
                    ScrollOffset--;
                    DrawInterface(SystemTable, FileName, StatusMsg);
                }
            }
        }
        else if (Key.ScanCode == SCAN_DOWN) {
            if (CursorY < MAX_FILE_LINES - 1) {
                CursorY++;
                if (CursorY >= ScrollOffset + EDIT_ROWS) {
                    ScrollOffset++;
                    DrawInterface(SystemTable, FileName, StatusMsg);
                }
            }
        }
        else if (Key.ScanCode == SCAN_LEFT) {
            if (CursorX > 0) CursorX--;
        }
        else if (Key.ScanCode == SCAN_RIGHT) {
            if (CursorX < EDIT_COLS - 1) CursorX++;
        }

        else if (Key.UnicodeChar == L'\r') {
            if (CursorY < MAX_FILE_LINES - 1) {
                CursorY++;
                CursorX = 0;
                
                if (CursorY >= ScrollOffset + EDIT_ROWS) {
                    ScrollOffset++;
                }
                DrawInterface(SystemTable, FileName, StatusMsg);
            } else {
                StrCpy(StatusMsg, L"Line limit reached (100)!");
                DrawStatusBar(SystemTable, FileName, StatusMsg);
            }
        }
        else if (Key.UnicodeChar == L'\b') {
            DeleteChar();
            RedrawCurrentLine(SystemTable);
        }
        else if (Key.UnicodeChar >= 32 && Key.UnicodeChar <= 126) {
            InsertChar(Key.UnicodeChar);
        
            UINTN OldY = CursorY; 
            
            if (CursorX < EDIT_COLS - 1) CursorX++;
            
            RedrawCurrentLine(SystemTable);
            
            if (CursorX == 0 && CursorY > OldY) { 
                 if (CursorY >= ScrollOffset + EDIT_ROWS) {
                    ScrollOffset++;
                }
                DrawInterface(SystemTable, FileName, StatusMsg);
            }
        }

        if (CursorY >= ScrollOffset && CursorY < ScrollOffset + EDIT_ROWS) {
            SetCursorPos(SystemTable, OFFSET_X + CursorX, OFFSET_Y + (CursorY - ScrollOffset));
        }
        else {
             DrawInterface(SystemTable, FileName, StatusMsg);
        }
    }

    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);

    print_header(SystemTable);
}