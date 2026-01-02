#include <efi.h>
#include <efilib.h>
#include "utils.h"
#include "commands.h"
#include "disk.h" 
#include "filesystem.h"
#include "editor.h"
#include "graphics.h"

void ParseCommand(CHAR16* Input, CHAR16* Cmd, CHAR16* Arg1, CHAR16* Arg2, CHAR16* Arg3) {
    int i = 0, j = 0;
    
    while(Input[i] != ' ' && Input[i] != '\0' && j < 255) Cmd[j++] = Input[i++];
    Cmd[j] = '\0';

    if(Input[i] == '\0') return; 
    while(Input[i] == ' ') i++;
    j=0; 

    while(Input[i] != ' ' && Input[i] != '\0' && j < 255) Arg1[j++] = Input[i++];
    Arg1[j] = '\0';
    if(Input[i] == '\0') return; 
    while(Input[i] == ' ') i++; 
    j=0;

    while(Input[i] != ' ' && Input[i] != '\0' && j < 255) Arg2[j++] = Input[i++];
    Arg2[j] = '\0';
    if(Input[i] == '\0') return; 
    while(Input[i] == ' ') i++;
    j=0;

    while(Input[i] != ' ' && Input[i] != '\0' && j < 255) Arg3[j++] = Input[i++];
    Arg3[j] = '\0';
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);

    fs_init(ImageHandle, SystemTable);

    if (InitGOP(SystemTable) == EFI_SUCCESS) {
        HOBBY_IMAGE* Logo = LoadBMP(SystemTable, L"\\EFI\\BOOT\\logo.bmp");
        
        if (Logo) {
            uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 3000000);

            ShowSplashScreen(SystemTable, Logo);
            FreeImage(SystemTable, Logo);
        } else {
            uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 5000000);
        }
    } else {
        uefi_call_wrapper(SystemTable->BootServices->Stall, 1, 5000000);
    }

    uefi_call_wrapper(SystemTable->BootServices->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
    uefi_call_wrapper(SystemTable->ConOut->EnableCursor, 2, SystemTable->ConOut, TRUE);

    print_header(SystemTable);

    Print(L"  Available Commands: 'shutdown', 'restart', 'clear', 'ver', 'disks', 'format', 'part' and others...\n\n");

    CHAR16 Buffer[256]; 
    CHAR16 Cmd[256];
    CHAR16 Arg1[256]; 
    CHAR16 Arg2[256];
    CHAR16 Arg3[256];

    while(1) {
        for(int k=0; k<256; k++) { 
            Cmd[k]=0; Arg1[k]=0; Arg2[k]=0; Arg3[k]=0; Buffer[k]=0; 
        }

        SetColor(SystemTable, COLOR_GREEN);
        Print(fs_get_prompt());
        SetColor(SystemTable, COLOR_WHITE);

        HobbyInput(SystemTable, Buffer, 100);
        HistoryAdd(Buffer);
        
        ParseCommand(Buffer, Cmd, Arg1, Arg2, Arg3);

        if (StrCmpCustom(Cmd, L"shutdown") == 0) cmd_shutdown(SystemTable);
        else if (StrCmpCustom(Cmd, L"restart") == 0) cmd_restart(SystemTable);
        else if (StrCmpCustom(Cmd, L"ver") == 0) cmd_ver(SystemTable);
        else if (StrCmpCustom(Cmd, L"clear") == 0) {
             uefi_call_wrapper(SystemTable->ConOut->ClearScreen, 1, SystemTable->ConOut);
             print_header(SystemTable);
        }
        else if (StrCmpCustom(Cmd, L"ls") == 0 || StrCmpCustom(Cmd, L"dir") == 0) {
            cmd_ls(ImageHandle, SystemTable);
        }
        else if (StrCmpCustom(Cmd, L"vol") == 0 || StrCmpCustom(Cmd, L"mount") == 0) {
            cmd_vol(ImageHandle, SystemTable, Arg1);
        }
        else if (StrCmpCustom(Cmd, L"pwd") == 0) {
            cmd_pwd(SystemTable);
        }
        else if (StrCmpCustom(Cmd, L"cd") == 0) {
            if (Arg1[0] != 0) cmd_cd(SystemTable, Arg1);
            else Print(L"Usage: cd <folder> oor cd ..\n");
        }
        else if (StrCmpCustom(Cmd, L"mkdir") == 0) {
            if (Arg1[0] != 0) cmd_mkdir(SystemTable, Arg1);
            else Print(L"Usage: mkdir <name>\n");
        }
        else if (StrCmpCustom(Cmd, L"touch") == 0) {
            if (Arg1[0] != 0) cmd_touch(SystemTable, Arg1);
            else Print(L"Usage: touch <file>\n");
        }
        else if (StrCmpCustom(Cmd, L"cat") == 0 || StrCmpCustom(Cmd, L"type") == 0) {
            if (Arg1[0] != 0) cmd_cat(SystemTable, Arg1);
            else Print(L"Usage: cat <file>\n");
        }
        else if (StrCmpCustom(Cmd, L"rm") == 0) {
            cmd_rm(SystemTable, Arg1, Arg2);
        }
        else if (StrCmpCustom(Cmd, L"cp") == 0 || StrCmpCustom(Cmd, L"copy") == 0) {
            cmd_cp(SystemTable, Arg1, Arg2, Arg3);
        }
        else if (StrCmpCustom(Cmd, L"edit") == 0 || StrCmpCustom(Cmd, L"nano") == 0) {
            cmd_edit(ImageHandle, SystemTable, Arg1);
        }
        else if (StrCmpCustom(Cmd, L"date") == 0 || StrCmpCustom(Cmd, L"time") == 0) {
            cmd_date(SystemTable);
        }
        else if (StrCmpCustom(Cmd, L"mem") == 0 || StrCmpCustom(Cmd, L"free") == 0) {
            cmd_mem(SystemTable);
        }
        else if (StrCmpCustom(Cmd, L"hexdump") == 0 || StrCmpCustom(Cmd, L"hex") == 0) {
            cmd_hexdump(SystemTable, Arg1);
        }
        else if (StrCmpCustom(Cmd, L"disks") == 0) {
            cmd_disks(ImageHandle, SystemTable);
        }
        else if (StrCmpCustom(Cmd, L"format") == 0) {
            if (Arg1[0] == 0) {
                Print(L"Usage:\n");
                Print(L"  format <disk>\n");
                Print(L"  format <disk> <fast|full>\n");
                Print(L"  format <disk> <part0based>\n");
                Print(L"  format <disk> <part0based> <fast|full>\n");
                Print(L"Ex:\n");
                Print(L"  format 2\n");
                Print(L"  format 2 fast\n");
                Print(L"  format 2 0\n");
                Print(L"  format 2 0 full\n");
            } else {
                cmd_format(ImageHandle, SystemTable, Arg1, Arg2, Arg3);
            }
        }
        else if (StrCmpCustom(Cmd, L"part") == 0) {
            cmd_part(ImageHandle, SystemTable, Arg1, Arg2, Arg3);
        }
        else if (StrCmpCustom(Cmd, L"gptinit") == 0) {
             if(Arg1[0] == 0) Print(L"Usage: gptinit <Idx>\n");
             else cmd_gptinit(ImageHandle, SystemTable, Arg1);
        }
        else if (StrCmpCustom(Cmd, L"install") == 0) {
            if(Arg1[0] == 0) {
                Print(L"Usage: install <IdxPartitionHandle>\n");
                Print(L" ou : install <IdxDisk> <IdxPartitionGPT0based>\n");
                Print(L"Ex : install 3\n");
                Print(L"Ex : install 2 0\n");
            }
            else cmd_install(ImageHandle, SystemTable, Arg1, Arg2);
        }
        else if (Cmd[0] != 0) {
            SetColor(SystemTable, COLOR_RED);
            
            Print(L"  Error: The command '");
            Print(Cmd);
            Print(L"' ot exists.\n");
            
            SetColor(SystemTable, COLOR_RESET);
        }
    }
    return EFI_SUCCESS;
}