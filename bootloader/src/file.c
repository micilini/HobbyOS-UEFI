#include "file.h"

static EFI_GUID LoadedImageProtocolGuid = LOADED_IMAGE_PROTOCOL;
static EFI_GUID FileSystemProtocolGuid = SIMPLE_FILE_SYSTEM_PROTOCOL;

EFI_FILE *open_root_volume(EFI_HANDLE ImageHandle)
{
    EFI_LOADED_IMAGE *loaded_image;
    EFI_STATUS status;

    status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocolGuid, (void **)&loaded_image);

    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Could not get LoadedImageProtocol (Status: %r)\n", status);
        return NULL;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded_image->DeviceHandle, &FileSystemProtocolGuid, (void **)&FileSystem);

    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Could not get FileSystemProtocol (Status: %r)\n", status);
        return NULL;
    }

    EFI_FILE *Root;
    status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);

    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Could not open Root Volume (Status: %r)\n", status);
        return NULL;
    }

    return Root;
}

EFI_FILE *load_file(EFI_FILE *Directory, CHAR16 *Path)
{
    EFI_FILE *file;

    EFI_STATUS status = uefi_call_wrapper(Directory->Open, 5, Directory, &file, Path, EFI_FILE_MODE_READ, 0ULL);

    if (EFI_ERROR(status))
    {
        Print(L"[-] Error: Could not open file %s (Status: %r)\n", Path, status);
        return NULL;
    }
    return file;
}