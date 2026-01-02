#ifndef DISK_H
#define DISK_H

#include <efi.h>
#include <efilib.h>

#pragma pack(1)
typedef struct {
    UINT8  JmpBoot[3];
    UINT8  OEMName[8];
    UINT16 BytesPerSec;
    UINT8  SecPerClus;
    UINT16 RsvdSecCnt;
    UINT8  NumFATs;
    UINT16 RootEntCnt;
    UINT16 TotSec16;
    UINT8  Media;
    UINT16 FATSz16;
    UINT16 SecPerTrk;
    UINT16 NumHeads;
    UINT32 HiddSec;
    UINT32 TotSec32;
    UINT32 FATSz32;
    UINT16 ExtFlags;
    UINT16 FSVer;
    UINT32 RootClus;
    UINT16 FSInfo;
    UINT16 BkBootSec;
    UINT8  Reserved[12];
    UINT8  DrvNum;
    UINT8  Reserved1;
    UINT8  BootSig;
    UINT32 VolID;
    UINT8  VolLab[11];
    UINT8  FilSysType[8];
    UINT8  Code[420];
    UINT16 Sig;
} FAT32_BOOTSECTOR;

typedef struct {
    UINT32 LeadSig;
    UINT8  Reserved1[480];
    UINT32 StrucSig;
    UINT32 Free_Count;
    UINT32 Nxt_Free;
    UINT8  Reserved2[12];
    UINT32 TrailSig;
} FAT32_FSINFO;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 HeaderCRC32;
    UINT32 Reserved;
    UINT64 MyLBA;
    UINT64 AlternateLBA;
    UINT64 FirstUsableLBA;
    UINT64 LastUsableLBA;
    EFI_GUID DiskGUID;
    UINT64 PartitionEntryLBA;
    UINT32 NumberOfPartitionEntries;
    UINT32 SizeOfPartitionEntry;
    UINT32 PartitionEntryArrayCRC32;
} GPT_HEADER;

typedef struct {
    EFI_GUID TypeGUID;
    EFI_GUID UniqueGUID;
    UINT64 StartingLBA;
    UINT64 EndingLBA;
    UINT64 Attributes;
    CHAR16 PartitionName[36];
} GPT_ENTRY;

typedef struct {
    UINT8  BootIndicator;
    UINT8  StartHead;
    UINT8  StartSector;
    UINT8  StartTrack;
    UINT8  OSIndicator;
    UINT8  EndHead;
    UINT8  EndSector;
    UINT8  EndTrack;
    UINT32 StartingLBA;
    UINT32 SizeInLBA;
} HOBBY_MBR_ENTRY;

typedef struct {
    UINT8  BootCode[440];
    UINT32 UniqueSignature;
    UINT16 Unknown;
    HOBBY_MBR_ENTRY Partition[4];
    UINT16 Signature;
} HOBBY_MBR;
#pragma pack()

#define EFI_PART_TYPE_DATA_GUID \
    { 0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B} }

void cmd_disks(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
void cmd_format(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1, CHAR16* Arg2, CHAR16* Arg3);
void cmd_part(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* ArgHandleIndex, CHAR16* ArgStartLBA, CHAR16* ArgSizeMB);
void cmd_gptinit(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* ArgHandleIndex);
void cmd_install(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* ArgDiskOrTarget, CHAR16* ArgPartIdx);

#endif