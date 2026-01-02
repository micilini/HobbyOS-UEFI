#include "filesystem.h"
#include "utils.h"

static EFI_HANDLE gCurrentDeviceHandle = NULL;
EFI_FILE_PROTOCOL *gCurrentRoot = NULL; 
static CHAR16 gCurrentPath[MAX_PATH_LEN];
static UINTN  gCurrentVolIdx = 0;
static CHAR16 gPromptBuffer[MAX_PATH_LEN + 20];

extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiBlockIoProtocolGuid;


void UpdatePrompt() {
    if (gCurrentRoot == NULL) {
        StrCpy(gPromptBuffer, L"HobbyOS (No FS)> ");
    } else {
        StrCpy(gPromptBuffer, L"FS");
        CHAR16 IndexStr[4];
        ValueToHex(IndexStr, gCurrentVolIdx);
        IndexStr[0] = (CHAR16)('0' + gCurrentVolIdx); 
        IndexStr[1] = 0;
        
        StrCat(gPromptBuffer, IndexStr);
        StrCat(gPromptBuffer, L":");
        StrCat(gPromptBuffer, gCurrentPath);
        StrCat(gPromptBuffer, L"> ");
    }
}

CHAR16* fs_get_prompt() {
    return gPromptBuffer;
}

EFI_STATUS MountVolume(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, UINTN VolIdx) {
    EFI_STATUS Status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;

    Status = uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
        DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void **)&Fs,
        NULL, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (EFI_ERROR(Status)) return Status;

    EFI_FILE_PROTOCOL *NewRoot;
    Status = uefi_call_wrapper(Fs->OpenVolume, 2, Fs, &NewRoot);
    if (EFI_ERROR(Status)) return Status;

    if (gCurrentRoot != NULL) {
        uefi_call_wrapper(gCurrentRoot->Close, 1, gCurrentRoot);
    }

    gCurrentDeviceHandle = DeviceHandle;
    gCurrentRoot = NewRoot;
    gCurrentVolIdx = VolIdx;

    StrCpy(gCurrentPath, L"\\");
    
    UpdatePrompt();
    return EFI_SUCCESS;
}

void fs_init(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gCurrentRoot = NULL;
    StrCpy(gPromptBuffer, L"HobbyOS> ");

    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_STATUS Status = uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
        ImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&LoadedImage,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!EFI_ERROR(Status)) {
        MountVolume(LoadedImage->DeviceHandle, SystemTable, 0);
    }
}

void cmd_ls(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    if (gCurrentRoot == NULL) {
        SetColor(SystemTable, COLOR_RED);
        Print(L"No volumes mounted. Use 'vol' to list and 'vol <id>' to mount.\n");
        SetColor(SystemTable, COLOR_WHITE);
        return;
    }

    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *DirHandle;

    Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &DirHandle, gCurrentPath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"Error reading current directory.\n");
        return;
    }

    UINTN BufferSize = 1024;
    UINT8 *Buffer;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, BufferSize, (void**)&Buffer);

    Print(L"\n  Directory: FS%d:%s\n", (int)gCurrentVolIdx, gCurrentPath);
    Print(L"  ------------------------------------------\n");

    while (1) {
        BufferSize = 1024;
        Status = uefi_call_wrapper(DirHandle->Read, 3, DirHandle, &BufferSize, Buffer);
        
        if (EFI_ERROR(Status) || BufferSize == 0) break;

        EFI_FILE_INFO *FileInfo = (EFI_FILE_INFO *)Buffer;

        if (StrCmpCustom(FileInfo->FileName, L".") == 0) continue;
        if (StrCmpCustom(FileInfo->FileName, L"..") == 0) continue;

        if (FileInfo->Attribute & EFI_FILE_DIRECTORY) {
            SetColor(SystemTable, COLOR_YELLOW);
            Print(L"   <DIR>      %s\n", FileInfo->FileName);
        } else {
            SetColor(SystemTable, EFI_LIGHTGRAY);
            UINT64 SizeKB = FileInfo->FileSize / 1024;
            if (SizeKB == 0 && FileInfo->FileSize > 0) SizeKB = 1; 
            
            Print(L"   %8ld KB  %s\n", SizeKB, FileInfo->FileName);
        }
    }

    SetColor(SystemTable, COLOR_WHITE);
    Print(L"\n");

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Buffer);
    uefi_call_wrapper(DirHandle->Close, 1, DirHandle);
}

void cmd_vol(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1) {
    EFI_STATUS Status;
    UINTN HandleCount;
    EFI_HANDLE *HandleBuffer;

    Status = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5, 
        ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HandleCount, &HandleBuffer);

    if (EFI_ERROR(Status)) {
        Print(L"Error when listing volumes.\n");
        return;
    }

    if (Arg1[0] != 0) {
        UINTN TargetIdx = HobbyAtoi(Arg1);
        if (TargetIdx >= HandleCount) {
            Print(L"Invalid volume.\n");
        } else {
            if (MountVolume(HandleBuffer[TargetIdx], SystemTable, TargetIdx) == EFI_SUCCESS) {
                SetColor(SystemTable, COLOR_GREEN);
                Print(L"Volume FS%d mount with success.\n", (int)TargetIdx);
            } else {
                Print(L"Failed when mounting volume.\n");
            }
        }
        SetColor(SystemTable, COLOR_WHITE);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, HandleBuffer);
        return;
    }

    SetColor(SystemTable, COLOR_CYAN);
    Print(L"IDX  Type    LABEL (IF ISSET)\n");
    Print(L"---  ------  -----------------\n");
    SetColor(SystemTable, COLOR_WHITE);

    for (UINTN i = 0; i < HandleCount; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
        uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
            HandleBuffer[i], &gEfiSimpleFileSystemProtocolGuid, (void **)&Fs,
            ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

        EFI_FILE_PROTOCOL *Root;
        uefi_call_wrapper(Fs->OpenVolume, 2, Fs, &Root);

        UINT8 VolInfoBuffer[200];
        UINTN VolInfoSize = sizeof(VolInfoBuffer);
        EFI_FILE_SYSTEM_INFO *VolInfo = (EFI_FILE_SYSTEM_INFO*)VolInfoBuffer;
        
        EFI_GUID FsInfoGuid = EFI_FILE_SYSTEM_INFO_ID;
        Status = uefi_call_wrapper(Root->GetInfo, 4, Root, &FsInfoGuid, &VolInfoSize, VolInfo);

        Print(L"%02d   FAT32   ", i);
        
        if (!EFI_ERROR(Status)) {
            Print(L"%s", VolInfo->VolumeLabel);
        } else {
            Print(L"(Without Label)");
        }

        if (HandleBuffer[i] == gCurrentDeviceHandle) {
            SetColor(SystemTable, COLOR_GREEN);
            Print(L"  <-- Actual");
            SetColor(SystemTable, COLOR_WHITE);
        }
        Print(L"\n");
        
        uefi_call_wrapper(Root->Close, 1, Root);
    }
    Print(L"\n");
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, HandleBuffer);
}

void NormalizePath(CHAR16* Path) {
    for (UINTN i = 0; Path[i] != 0; i++) {
        if (Path[i] == L'/') {
            Path[i] = L'\\';
        }
    }
}

void ResolvePath(CHAR16* Input, CHAR16* OutputBuffer) {
    NormalizePath(Input); 

    if (Input[0] == L'\\') {
        StrCpy(OutputBuffer, Input);
        return;
    }

    StrCpy(OutputBuffer, gCurrentPath);

    UINTN Len = StrLen(OutputBuffer);
    if (Len > 0 && OutputBuffer[Len-1] != L'\\') {
        StrCat(OutputBuffer, L"\\");
    }
    
    StrCat(OutputBuffer, Input);
}

void cmd_pwd(EFI_SYSTEM_TABLE *SystemTable) {
    if (gCurrentRoot == NULL) {
        Print(L"No mount disk.\n");
    } else {
        Print(L"%s\n", gCurrentPath);
    }
}

void cmd_cd(EFI_SYSTEM_TABLE *SystemTable, CHAR16* DirName) {
    if (gCurrentRoot == NULL) return;

    CHAR16 TargetPath[MAX_PATH_LEN];

    if (StrCmpCustom(DirName, L"..") == 0) {
        StrCpy(TargetPath, gCurrentPath);
        
        UINTN Len = StrLen(TargetPath);
        if (Len <= 1) return;

        if (TargetPath[Len-1] == L'\\') {
            TargetPath[Len-1] = 0;
            Len--;
        }

        for (int i = Len - 1; i >= 0; i--) {
            if (TargetPath[i] == L'\\') {
                TargetPath[i+1] = 0;
                if (i == 0) TargetPath[1] = 0;
                break;
            }
        }
        
        StrCpy(gCurrentPath, TargetPath);
        UpdatePrompt();
        return;
    }

    ResolvePath(DirName, TargetPath);

    EFI_FILE_PROTOCOL *NewDir;
    EFI_STATUS Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &NewDir, TargetPath, EFI_FILE_MODE_READ, 0);

    if (EFI_ERROR(Status)) {
        SetColor(SystemTable, COLOR_RED);
        Print(L"Error: path not found.\n");
        SetColor(SystemTable, COLOR_WHITE);
    } else {
        EFI_FILE_INFO *Info;
        UINTN Size = 0;

        uefi_call_wrapper(NewDir->GetInfo, 4, NewDir, &gEfiFileInfoGuid, &Size, NULL);
        if (Size == 0) Size = 200;
        
        uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, Size, (void**)&Info);
        uefi_call_wrapper(NewDir->GetInfo, 4, NewDir, &gEfiFileInfoGuid, &Size, Info);

        if (Info->Attribute & EFI_FILE_DIRECTORY) {
            StrCpy(gCurrentPath, TargetPath);
            UpdatePrompt();
        } else {
            Print(L"Error: '%s' is a file, not a folder.\n", DirName);
        }

        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Info);
        uefi_call_wrapper(NewDir->Close, 1, NewDir);
    }
}

void cmd_mkdir(EFI_SYSTEM_TABLE *SystemTable, CHAR16* DirName) {
    if (gCurrentRoot == NULL) return;

    CHAR16 FullPath[MAX_PATH_LEN];
    ResolvePath(DirName, FullPath);

    EFI_FILE_PROTOCOL *NewDir;
    EFI_STATUS Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &NewDir, FullPath, 
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 
        EFI_FILE_DIRECTORY);

    if (!EFI_ERROR(Status)) {
        Print(L"Directory created: %s\n", FullPath);
        uefi_call_wrapper(NewDir->Close, 1, NewDir);
    } else {
        SetColor(SystemTable, COLOR_RED);
        Print(L"Error when creating directory (Status: %x)\n", Status);
        SetColor(SystemTable, COLOR_WHITE);
    }
}

void cmd_touch(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName) {
    if (gCurrentRoot == NULL) return;

    CHAR16 FullPath[MAX_PATH_LEN];
    ResolvePath(FileName, FullPath);

    EFI_FILE_PROTOCOL *NewFile;

    EFI_STATUS Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &NewFile, FullPath, 
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 
        0);

    if (!EFI_ERROR(Status)) {
        Print(L"File created: %s\n", FullPath);
        uefi_call_wrapper(NewFile->Close, 1, NewFile);
    } else {
        Print(L"Error when creating the file.\n");
    }
}

void cmd_cat(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName) {
    if (gCurrentRoot == NULL) return;

    CHAR16 FullPath[MAX_PATH_LEN];
    ResolvePath(FileName, FullPath);

    EFI_FILE_PROTOCOL *File;
    EFI_STATUS Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &File, FullPath, EFI_FILE_MODE_READ, 0);

    if (EFI_ERROR(Status)) {
        Print(L"Error: File not found.\n");
        return;
    }

    UINT8 Chunk[128];
    UINTN ReadSize = sizeof(Chunk);

    Print(L"\n--- Start of %s ---\n", FileName);
    SetColor(SystemTable, COLOR_CYAN);

    while(1) {
        ReadSize = sizeof(Chunk);
        Status = uefi_call_wrapper(File->Read, 3, File, &ReadSize, Chunk);
        if (EFI_ERROR(Status) || ReadSize == 0) break;

        for (UINTN i = 0; i < ReadSize; i++) {
            CHAR16 c = (CHAR16)Chunk[i];
            
            if (c == L'\n') Print(L"\r");
            
            CHAR16 Str[2] = {c, 0};
            Print(Str);
        }
    }

    SetColor(SystemTable, COLOR_WHITE);
    Print(L"\n--- End ---\n");

    uefi_call_wrapper(File->Close, 1, File);
}

BOOLEAN IsFlag(CHAR16* Str) {
    return (Str != NULL && Str[0] == L'-');
}

BOOLEAN HasFlag(CHAR16* F1, CHAR16* F2, CHAR16* TargetFlag) {
    if (F1 && StrCmpCustom(F1, TargetFlag) == 0) return TRUE;
    if (F2 && StrCmpCustom(F2, TargetFlag) == 0) return TRUE;
    return FALSE;
}

CHAR16* GetPathFromArgs(CHAR16* A1, CHAR16* A2) {
    if (!IsFlag(A1) && A1[0] != 0) return A1;
    if (!IsFlag(A2) && A2[0] != 0) return A2;
    return NULL;
}

EFI_STATUS RecursiveDelete(EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_PROTOCOL *Parent, CHAR16* Name) {
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *Target;

    Status = uefi_call_wrapper(Parent->Open, 5, Parent, &Target, Name, 
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    
    if (EFI_ERROR(Status)) return Status;

    EFI_FILE_INFO *Info;
    UINTN Size = 0;
    
    uefi_call_wrapper(Target->GetInfo, 4, Target, &gEfiFileInfoGuid, &Size, NULL);
    if (Size == 0) Size = 200;

    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, Size, (void**)&Info);
    
    uefi_call_wrapper(Target->GetInfo, 4, Target, &gEfiFileInfoGuid, &Size, Info);

    if (Info->Attribute & EFI_FILE_DIRECTORY) {
        UINTN DirBuffSize = 1024;
        UINT8 *DirBuff;
        
        uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, DirBuffSize, (void**)&DirBuff);

        uefi_call_wrapper(Target->SetPosition, 2, Target, 0);

        while(1) {
            DirBuffSize = 1024;
            Status = uefi_call_wrapper(Target->Read, 3, Target, &DirBuffSize, DirBuff);
            if (EFI_ERROR(Status) || DirBuffSize == 0) break;

            EFI_FILE_INFO *Entry = (EFI_FILE_INFO*)DirBuff;

            if (StrCmpCustom(Entry->FileName, L".") == 0) continue;
            if (StrCmpCustom(Entry->FileName, L"..") == 0) continue;

            RecursiveDelete(SystemTable, Target, Entry->FileName);
        }
        
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, DirBuff);
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Info);

    Status = uefi_call_wrapper(Target->Delete, 1, Target);
    
    if (EFI_ERROR(Status)) {
        uefi_call_wrapper(Target->Close, 1, Target); 
    }

    return Status;
}

void cmd_rm(EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1, CHAR16* Arg2) {
    if (gCurrentRoot == NULL) return;

    CHAR16* TargetName = GetPathFromArgs(Arg1, Arg2);
    BOOLEAN Recursive = HasFlag(Arg1, Arg2, L"-r");

    if (TargetName == NULL) {
        Print(L"Usage: rm <name> [-r]\n");
        return;
    }

    CHAR16 FullPath[MAX_PATH_LEN];
    ResolvePath(TargetName, FullPath);

    Print(L"Deleting: %s (R=%d)...\n", FullPath, Recursive);

    if (Recursive) {
        CHAR16* RelPath = FullPath;
        if (RelPath[0] == L'\\') RelPath++;

        EFI_STATUS Status = RecursiveDelete(SystemTable, gCurrentRoot, RelPath);
        
        if (EFI_ERROR(Status)) {
            Print(L"Error when deleting (Status: %x). Try -r if has files/folder in that folder.\n", Status);
        } else {
            Print(L"Deleted.\n");
        }
    } 
    else {
        EFI_FILE_PROTOCOL *FileHandle;
        EFI_STATUS Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &FileHandle, FullPath, 
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);

        if (EFI_ERROR(Status)) {
            Print(L"File not found.\n");
            return;
        }

        Status = uefi_call_wrapper(FileHandle->Delete, 1, FileHandle);
        
        if (Status == EFI_SUCCESS) {
             Print(L"File removed.\n");
        } 
        else if (Status == 2) {
             SetColor(SystemTable, COLOR_RED);
             Print(L"Error: The folder is not empty. Use the -r flag to force it.\n");
             SetColor(SystemTable, COLOR_WHITE);
        } 
        else {
             SetColor(SystemTable, COLOR_RED);
             Print(L"Error when deleting (Status: %x).\n", Status);
             SetColor(SystemTable, COLOR_WHITE);

             uefi_call_wrapper(FileHandle->Close, 1, FileHandle);
        }
    }
}

void cmd_cp(EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1, CHAR16* Arg2, CHAR16* Arg3) {
    if (gCurrentRoot == NULL) return;
    
    CHAR16 *SrcIdx = Arg1;
    CHAR16 *DstIdx = Arg2;

    if (SrcIdx[0] == 0 || DstIdx[0] == 0) {
        Print(L"Usage: cp <origin> <destination>\n");
        return;
    }

    CHAR16 SrcPath[MAX_PATH_LEN];
    CHAR16 DstPath[MAX_PATH_LEN];
    ResolvePath(SrcIdx, SrcPath);
    ResolvePath(DstIdx, DstPath);

    EFI_FILE_PROTOCOL *SrcHandle, *DstHandle;
    EFI_STATUS Status;

    Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &SrcHandle, SrcPath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"Error: Origin not found '%s'\n", SrcPath);
        return;
    }

    Status = uefi_call_wrapper(gCurrentRoot->Open, 5, gCurrentRoot, &DstHandle, DstPath, 
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    
    if (EFI_ERROR(Status)) {
        Print(L"Error: npt possible to create destination '%s'\n", DstPath);
        uefi_call_wrapper(SrcHandle->Close, 1, SrcHandle);
        return;
    }

    Print(L"Copy...");
    UINT8 Buffer[4096];
    UINTN BuffSize = sizeof(Buffer);
    UINTN Total = 0;

    while(1) {
        BuffSize = sizeof(Buffer);
        Status = uefi_call_wrapper(SrcHandle->Read, 3, SrcHandle, &BuffSize, Buffer);
        if (EFI_ERROR(Status) || BuffSize == 0) break;

        uefi_call_wrapper(DstHandle->Write, 3, DstHandle, &BuffSize, Buffer);
        Total += BuffSize;
    }

    Print(L" OK! (%d bytes)\n", Total);

    uefi_call_wrapper(SrcHandle->Close, 1, SrcHandle);
    uefi_call_wrapper(DstHandle->Close, 1, DstHandle);
}

EFI_FILE_PROTOCOL* fs_get_root() {
    return gCurrentRoot;
}