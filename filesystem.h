#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <efi.h>
#include <efilib.h>

#define MAX_PATH_LEN 256

void fs_init(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

CHAR16* fs_get_prompt();

void cmd_vol(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1);
void cmd_ls(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
void cmd_pwd(EFI_SYSTEM_TABLE *SystemTable);
void cmd_cd(EFI_SYSTEM_TABLE *SystemTable, CHAR16* DirName);
void cmd_mkdir(EFI_SYSTEM_TABLE *SystemTable, CHAR16* DirName);
void cmd_touch(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName);
void cmd_cat(EFI_SYSTEM_TABLE *SystemTable, CHAR16* FileName);
void cmd_rm(EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1, CHAR16* Arg2);
void cmd_cp(EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1, CHAR16* Arg2, CHAR16* Arg3);

void ResolvePath(CHAR16* Input, CHAR16* OutputBuffer);
EFI_FILE_PROTOCOL* fs_get_root();

#endif