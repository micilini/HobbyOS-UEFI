#include "disk.h"
#include "utils.h"

extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiBlockIoProtocolGuid;

void CheckGPT(EFI_BLOCK_IO_PROTOCOL *BlockIo, EFI_SYSTEM_TABLE *SystemTable) {
    UINT32 BlockSize = BlockIo->Media->BlockSize;

    UINT8 *Buffer;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, BlockSize, (void**)&Buffer);

    uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        1, BlockSize, Buffer);

    GPT_HEADER *Header = (GPT_HEADER*)Buffer;

    if (Header->Signature == 0x5452415020494645ULL) {
        UINTN ArraySize = Header->NumberOfPartitionEntries * Header->SizeOfPartitionEntry;

        UINT8 *EntryBuffer;
        uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, ArraySize, (void**)&EntryBuffer);

        uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId,
            Header->PartitionEntryLBA, ArraySize, EntryBuffer);

        GPT_ENTRY *Entries = (GPT_ENTRY*)EntryBuffer;

        SetColor(SystemTable, COLOR_CYAN);
        Print(L"  GPT partitions found:\n");
        SetColor(SystemTable, COLOR_WHITE);

        EFI_GUID EspGuid = {0xC12A7328, 0xF81F, 0x11D2, {0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B}};

        for (UINTN i = 0; i < Header->NumberOfPartitionEntries; i++) {
            if (Entries[i].StartingLBA != 0 && Entries[i].EndingLBA != 0) {
                UINT64 SizeLBA = Entries[i].EndingLBA - Entries[i].StartingLBA + 1;
                UINT64 SizeMB  = (SizeLBA * BlockSize) / (1024 * 1024);

                SetColor(SystemTable, COLOR_WHITE);
                Print(L"    [%02d] ", i);

                if (CompareGuid(&Entries[i].TypeGUID, &EspGuid)) {
                    SetColor(SystemTable, EFI_GREEN);
                    Print(L" [ESP/BOOT] ");
                } else {
                    SetColor(SystemTable, EFI_LIGHTGRAY);
                    Print(L" [DADOS] ");
                }

                SetColor(SystemTable, COLOR_YELLOW);
                Print(L"(%ld MB) [LBA: %ld -> %ld]\n", SizeMB, Entries[i].StartingLBA, Entries[i].EndingLBA);
            }
        }

        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Buffer);
    SetColor(SystemTable, COLOR_WHITE);
}

void cmd_disks(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    UINTN HandleCount;
    EFI_HANDLE *HandleBuffer;
    UINTN i;

    Status = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5, 
        ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &HandleBuffer);

    if (EFI_ERROR(Status)) {
        Print(L"Error listing disks.\n");
        return;
    }

    SetColor(SystemTable, COLOR_CYAN);
    Print(L"IDX  TYPE    STATE    LARGE       DETAILS\n");
    Print(L"---  ------  -------  ----------  ------------------\n");
    SetColor(SystemTable, COLOR_WHITE);

    for (i = 0; i < HandleCount; i++) {
        EFI_BLOCK_IO_PROTOCOL *BlockIo;
        Status = uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
            HandleBuffer[i], &gEfiBlockIoProtocolGuid, (void **)&BlockIo,
            ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

        if (!EFI_ERROR(Status)) {
            EFI_BLOCK_IO_MEDIA *Media = BlockIo->Media;
            
            UINT64 SizeBytes = (Media->LastBlock + 1) * Media->BlockSize;
            UINT64 SizeMB = SizeBytes / (1024 * 1024);
            UINT64 SizeGB = SizeMB / 1024;

            Print(L"%02d   ", i);
            
            if (Media->LogicalPartition) {
                SetColor(SystemTable, COLOR_YELLOW);
                Print(L"[PART]  ");
            } else {
                SetColor(SystemTable, EFI_LIGHTGRAY); 
                Print(L"[DISK]  ");
            }

            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
            EFI_STATUS FsStatus = uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
                HandleBuffer[i], &gEfiSimpleFileSystemProtocolGuid, (void **)&Fs,
                ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

            if (!EFI_ERROR(FsStatus)) {
                SetColor(SystemTable, COLOR_GREEN);
                Print(L"[FAT32]  ");
            } else {
                SetColor(SystemTable, COLOR_RED);
                Print(L"[RAW]    ");
            }
            SetColor(SystemTable, COLOR_WHITE);

            if (SizeGB > 0) Print(L"%4ld GB    ", SizeGB);
            else Print(L"%4ld MB    ", SizeMB);

            if (Media->RemovableMedia) Print(L"USB/Removable");
            else Print(L"HDD Fixed");

            Print(L"\n");

            if (!Media->LogicalPartition) {
                CheckGPT(BlockIo, SystemTable);
            }
        }
    }
    Print(L"\n");
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, HandleBuffer);
}

void cmd_format(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* Arg1, CHAR16* Arg2, CHAR16* Arg3) {
    UINTN DiskIdx = HobbyAtoi(Arg1);

    BOOLEAN HasArg2 = (Arg2 != NULL && Arg2[0] != 0);
    BOOLEAN HasArg3 = (Arg3 != NULL && Arg3[0] != 0);

    enum { MODE_FAST = 0, MODE_FULL = 1 } Mode = MODE_FAST;

    BOOLEAN HasPart = FALSE;
    UINTN PartIdx0 = 0;

    auto BOOLEAN IsModeFast(CHAR16* s) { return (s && (StrCmpCustom(s, L"fast") == 0)); }
    auto BOOLEAN IsModeFull(CHAR16* s) { return (s && (StrCmpCustom(s, L"full") == 0 || StrCmpCustom(s, L"slow") == 0)); }

    if (HasArg2 && (IsModeFast(Arg2) || IsModeFull(Arg2))) {
        HasPart = FALSE;
        Mode = IsModeFull(Arg2) ? MODE_FULL : MODE_FAST;
    } else if (HasArg2) {
        HasPart = TRUE;
        PartIdx0 = HobbyAtoi(Arg2);
        Mode = MODE_FULL;
        if (HasArg3) {
            if (IsModeFast(Arg3)) Mode = MODE_FAST;
            if (IsModeFull(Arg3)) Mode = MODE_FULL;
        }
    } else {
        HasPart = FALSE;
        Mode = MODE_FAST;
    }

    UINTN HandleCount;
    EFI_HANDLE *HandleBuffer;

    uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
        ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &HandleBuffer);

    if (DiskIdx >= HandleCount) {
        Print(L"Invalid Disk.\n");
        return;
    }

    EFI_BLOCK_IO_PROTOCOL *BlockIo;
    uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
        HandleBuffer[DiskIdx], &gEfiBlockIoProtocolGuid, (void **)&BlockIo,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!BlockIo || !BlockIo->Media || !BlockIo->Media->MediaPresent) {
        Print(L"Error: Media not found.\n");
        return;
    }

    UINT32 BlockSize = BlockIo->Media->BlockSize;

    UINT8 *Sector;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, BlockSize, (void**)&Sector);

    UINT64 PartitionStartLBA = 0;
    UINT64 PartitionSizeLBA  = BlockIo->Media->LastBlock + 1;

    if (HasPart) {
        if (BlockIo->Media->LogicalPartition) {
            Print(L"Error: format <disk> <part> it should point to DISK, not partition..\n");
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
            return;
        }

        Print(L"Reading GPT from Disk %d to find Partition [%d]...\n", (int)DiskIdx, (int)PartIdx0);

        uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);
        uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId, 1, BlockSize, Sector);

        GPT_HEADER *Header = (GPT_HEADER*)Sector;
        if (Header->Signature != 0x5452415020494645ULL) {
            Print(L"Error: The disk is not GPT. Use 'gptinit'.\n");
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
            return;
        }

        if (PartIdx0 >= Header->NumberOfPartitionEntries) {
            Print(L"Error: Partition [%d] out of limit.\n", (int)PartIdx0);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
            return;
        }

        UINTN ArraySize = Header->NumberOfPartitionEntries * Header->SizeOfPartitionEntry;

        UINT8 *EntryBuffer;
        uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, ArraySize, (void**)&EntryBuffer);

        uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId,
            Header->PartitionEntryLBA, ArraySize, EntryBuffer);

        GPT_ENTRY *Entries = (GPT_ENTRY*)EntryBuffer;
        GPT_ENTRY *TargetEntry = &Entries[PartIdx0];

        if (TargetEntry->StartingLBA == 0 && TargetEntry->EndingLBA == 0) {
            Print(L"Error: Partition [%d] not found (empty entry).\n", (int)PartIdx0);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
            return;
        }

        PartitionStartLBA = TargetEntry->StartingLBA;
        PartitionSizeLBA  = TargetEntry->EndingLBA - TargetEntry->StartingLBA + 1;

        Print(L"Target: LBA %ld (Large: %ld sections)\n", PartitionStartLBA, PartitionSizeLBA);

        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
    } else {
        PartitionStartLBA = 0;
        PartitionSizeLBA  = BlockIo->Media->LastBlock + 1;
    }

    auto void TelemetryProgress(CHAR16* Label, UINT64 cur, UINT64 total) {
        if (total == 0) return;
        UINT64 pct = (cur * 100) / total;
        Print(L"\r%s: %ld/%ld (%ld%%)", Label, cur, total, pct);
    }

    if (!HasPart) {
        uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);

        UINT64 last = BlockIo->Media->LastBlock;
        UINT64 wipeCount = 34;
        if (last + 1 < wipeCount) wipeCount = last + 1;

        Print(L"Cleaning up old GPT/MBR structures...\n");

        for (UINT64 i = 0; i < wipeCount; i++) {
            uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, i, BlockSize, Sector);
            if ((i % 8) == 0) TelemetryProgress(L"Wipe(INI)", i, wipeCount - 1);
        }
        Print(L"\n");

        UINT64 startEnd = 0;
        if (last >= (wipeCount - 1)) startEnd = last - (wipeCount - 1);
        for (UINT64 i = 0; i < wipeCount; i++) {
            uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, startEnd + i, BlockSize, Sector);
            if ((i % 8) == 0) TelemetryProgress(L"Wipe(FIM)", i, wipeCount - 1);
        }
        Print(L"\n");
    }

    Print(L"Formatting in FAT32 (%s)...\n", (Mode == MODE_FAST ? L"FAST" : L"FULL"));

    UINT8 SecPerClus = 1;
    if (!HasPart) {
        UINT64 TotSec64 = PartitionSizeLBA;
        if (TotSec64 <= 532480ULL)       SecPerClus = 1;
        else if (TotSec64 <= 16777216ULL) SecPerClus = 8;
        else if (TotSec64 <= 33554432ULL) SecPerClus = 16;
        else                              SecPerClus = 32;
    }

    FAT32_BOOTSECTOR Bs;
    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, &Bs, sizeof(FAT32_BOOTSECTOR), 0);

    Bs.JmpBoot[0] = 0xEB; Bs.JmpBoot[1] = 0x58; Bs.JmpBoot[2] = 0x90;
    CopyMem(Bs.OEMName, "MSDOS5.0", 8);

    Bs.BytesPerSec = (UINT16)BlockSize;
    Bs.SecPerClus  = SecPerClus;
    Bs.RsvdSecCnt  = 32;
    Bs.NumFATs     = 2;
    Bs.RootEntCnt  = 0;
    Bs.TotSec16    = 0;
    Bs.Media       = 0xF8;
    Bs.FATSz16     = 0;
    Bs.SecPerTrk   = 63;
    Bs.NumHeads    = 255;

    Bs.HiddSec = (UINT32)PartitionStartLBA;

    if (PartitionSizeLBA > 0xFFFFFFFFULL) Bs.TotSec32 = 0xFFFFFFFF;
    else Bs.TotSec32 = (UINT32)PartitionSizeLBA;

    Bs.RootClus  = 2;
    Bs.FSInfo    = 1;
    Bs.BkBootSec = 6;

    Bs.DrvNum   = 0x80;
    Bs.Reserved1 = 0;
    Bs.BootSig  = 0x29;
    Bs.VolID    = 0x12345678;
    CopyMem(Bs.VolLab, "HOBBYOS    ", 11);
    CopyMem(Bs.FilSysType, "FAT32   ", 8);

    UINT32 TotSec = Bs.TotSec32;
    UINT32 Rsvd   = Bs.RsvdSecCnt;
    UINT32 NFAT   = Bs.NumFATs;

    UINT32 FatSz = 1;
    while (1) {
        UINT32 DataSec = TotSec - Rsvd - (NFAT * FatSz);
        UINT32 ClusCnt = DataSec / Bs.SecPerClus;

        UINT32 FatSzNew = (UINT32)(((UINT64)(ClusCnt + 2) * 4 + (BlockSize - 1)) / BlockSize);
        if (FatSzNew == 0) FatSzNew = 1;

        if (FatSzNew == FatSz) break;
        FatSz = FatSzNew;
    }
    Bs.FATSz32 = FatSz;

    Bs.Sig = 0xAA55;

    FAT32_FSINFO FsInfo;
    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, &FsInfo, sizeof(FAT32_FSINFO), 0);
    FsInfo.LeadSig    = 0x41615252;
    FsInfo.StrucSig   = 0x61417272;
    FsInfo.Free_Count = 0xFFFFFFFF;
    FsInfo.Nxt_Free   = 0xFFFFFFFF;
    FsInfo.TrailSig   = 0xAA550000;

    UINT64 BootLBA      = PartitionStartLBA;
    UINT64 FsInfoLBA    = PartitionStartLBA + Bs.FSInfo;
    UINT64 BkBootLBA    = PartitionStartLBA + Bs.BkBootSec;
    UINT64 BkFsInfoLBA  = BkBootLBA + Bs.FSInfo;

    UINT64 Fat1StartLBA = PartitionStartLBA + Bs.RsvdSecCnt;
    UINT64 Fat2StartLBA = Fat1StartLBA + Bs.FATSz32;
    UINT64 RootDirLBA   = PartitionStartLBA + Bs.RsvdSecCnt + ((UINT64)Bs.NumFATs * Bs.FATSz32);

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);
    CopyMem(Sector, &Bs, sizeof(FAT32_BOOTSECTOR));
    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, BootLBA, BlockSize, Sector);
    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, BkBootLBA, BlockSize, Sector);

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);
    CopyMem(Sector, &FsInfo, sizeof(FAT32_FSINFO));
    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, FsInfoLBA, BlockSize, Sector);
    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, BkFsInfoLBA, BlockSize, Sector);

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);

    if (Mode == MODE_FULL) {
        Print(L"Clearing the entire FAT1...\n");
        for (UINT32 s = 0; s < Bs.FATSz32; s++) {
            uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, Fat1StartLBA + s, BlockSize, Sector);
            if ((s % 128) == 0) TelemetryProgress(L"FAT1", s, (UINT64)(Bs.FATSz32 - 1));
        }
        Print(L"\n");

        Print(L"Clearing the entire FAT2...\n");
        for (UINT32 s = 0; s < Bs.FATSz32; s++) {
            uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, Fat2StartLBA + s, BlockSize, Sector);
            if ((s % 128) == 0) TelemetryProgress(L"FAT2", s, (UINT64)(Bs.FATSz32 - 1));
        }
        Print(L"\n");
    } else {
        UINT32 N = 8;
        if (N > Bs.FATSz32) N = Bs.FATSz32;

        Print(L"FAST: Clearing out the first %d sectors of FAT1/FAT2...\n", (int)N);

        for (UINT32 s = 0; s < N; s++) {
            uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, Fat1StartLBA + s, BlockSize, Sector);
            uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, Fat2StartLBA + s, BlockSize, Sector);
            TelemetryProgress(L"FAST-FAT", s, (UINT64)(N - 1));
        }
        Print(L"\n");
    }

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);
    UINT32 *Fat = (UINT32*)Sector;
    Fat[0] = 0x0FFFFFF8;
    Fat[1] = 0x0FFFFFFF;
    Fat[2] = 0x0FFFFFFF;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, Fat1StartLBA, BlockSize, Sector);
    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, Fat2StartLBA, BlockSize, Sector);

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);
    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId, RootDirLBA, BlockSize, Sector);

    uefi_call_wrapper(BlockIo->FlushBlocks, 1, BlockIo);

    Print(L"OK! FAT32 finished.\n");
    Print(L"INFO: BootLBA=%ld FSInfo=%ld FAT1=%ld FAT2=%ld Root=%ld | SecPerClus=%d | FATSz32=%d\n",
          BootLBA, FsInfoLBA, Fat1StartLBA, Fat2StartLBA, RootDirLBA, (int)Bs.SecPerClus, (int)Bs.FATSz32);

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
}

void cmd_part(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable,
              CHAR16* ArgHandleIndex, CHAR16* ArgStartLBA, CHAR16* ArgSizeMB) {

    UINTN DiskIndex = HobbyAtoi(ArgHandleIndex);
    UINT64 StartLBA = (UINT64)HobbyAtoi(ArgStartLBA);
    UINTN SizeMB    = HobbyAtoi(ArgSizeMB);

    UINTN HandleCount;
    EFI_HANDLE *HandleBuffer;
    uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
        ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &HandleBuffer);

    if (DiskIndex >= HandleCount) {
        Print(L"Error: Invalid Disk.\n");
        return;
    }

    EFI_BLOCK_IO_PROTOCOL *BlockIo;
    uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
        HandleBuffer[DiskIndex], &gEfiBlockIoProtocolGuid, (void **)&BlockIo,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!BlockIo || !BlockIo->Media || !BlockIo->Media->MediaPresent) {
        Print(L"Error: Media not found.\n");
        return;
    }

    if (BlockIo->Media->LogicalPartition) {
        Print(L"Error: part must be run on the DISK, not on a partition..\n");
        return;
    }

    UINT32 BlockSize = BlockIo->Media->BlockSize;
    UINT64 LastBlock = BlockIo->Media->LastBlock;

    UINT8 *SectorBuffer;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, BlockSize, (void**)&SectorBuffer);

    uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        1, BlockSize, SectorBuffer);

    GPT_HEADER *Header = (GPT_HEADER*)SectorBuffer;

    if (Header->Signature != 0x5452415020494645ULL) {
        Print(L"Error: Disk is not a valid GPT format. Run gptinit first.\n");
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, SectorBuffer);
        return;
    }

    UINTN ArraySize    = Header->NumberOfPartitionEntries * Header->SizeOfPartitionEntry;
    UINTN ArraySectors = (ArraySize + BlockSize - 1) / BlockSize;

    UINT8 *EntryBuffer;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, ArraySize, (void**)&EntryBuffer);

    uefi_call_wrapper(BlockIo->ReadBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        Header->PartitionEntryLBA, ArraySize, EntryBuffer);

    GPT_ENTRY *Entries = (GPT_ENTRY*)EntryBuffer;

    INTN Slot = -1;
    for (UINTN i = 0; i < Header->NumberOfPartitionEntries; i++) {
        if (Entries[i].StartingLBA == 0 && Entries[i].EndingLBA == 0) {
            Slot = (INTN)i;
            break;
        }
    }

    if (Slot < 0) {
        Print(L"Error: Partition table full.\n");
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, SectorBuffer);
        return;
    }

    UINT64 SizeBytes = (UINT64)SizeMB * 1024ULL * 1024ULL;
    UINT64 SizeLBA   = SizeBytes / BlockSize;
    if (SizeBytes % BlockSize) SizeLBA++;
    if (SizeLBA == 0) SizeLBA = 1;

    UINT64 EndLBA = StartLBA + SizeLBA - 1;

    if (StartLBA < Header->FirstUsableLBA || EndLBA > Header->LastUsableLBA || EndLBA <= StartLBA) {
        Print(L"Error: Range outside the usable GPT limit. Usable: %ld -> %ld\n",
              Header->FirstUsableLBA, Header->LastUsableLBA);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, SectorBuffer);
        return;
    }

    for (UINTN i = 0; i < Header->NumberOfPartitionEntries; i++) {
        if (Entries[i].StartingLBA == 0 && Entries[i].EndingLBA == 0) continue;

        if (!(EndLBA < Entries[i].StartingLBA || StartLBA > Entries[i].EndingLBA)) {
            Print(L"Error: Collision with existing partition [%d] LBA %ld -> %ld\n",
                  i, Entries[i].StartingLBA, Entries[i].EndingLBA);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, SectorBuffer);
            return;
        }
    }

    Print(L"Creating Partition %d: LBA %ld -> %ld (%d MB)\n",
          (int)(Slot + 1), StartLBA, EndLBA, (int)SizeMB);

    EFI_GUID TypeGuid = EFI_PART_TYPE_DATA_GUID;
    Entries[Slot].TypeGUID     = TypeGuid;
    Entries[Slot].UniqueGUID   = TypeGuid;
    Entries[Slot].StartingLBA  = StartLBA;
    Entries[Slot].EndingLBA    = EndLBA;
    Entries[Slot].Attributes   = 0;

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3,
        Entries[Slot].PartitionName, sizeof(Entries[Slot].PartitionName), 0);
    Entries[Slot].PartitionName[0] = L'E';
    Entries[Slot].PartitionName[1] = L'S';
    Entries[Slot].PartitionName[2] = L'P';

    UINT32 ArrayCRC = 0;
    uefi_call_wrapper(SystemTable->BootServices->CalculateCrc32, 3,
        EntryBuffer, ArraySize, &ArrayCRC);
    Header->PartitionEntryArrayCRC32 = ArrayCRC;

    Header->HeaderCRC32 = 0;
    UINT32 HeaderCRC = 0;
    uefi_call_wrapper(SystemTable->BootServices->CalculateCrc32, 3,
        Header, Header->HeaderSize, &HeaderCRC);
    Header->HeaderCRC32 = HeaderCRC;

    GPT_HEADER PrimaryCopy = *Header;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        1, BlockSize, SectorBuffer);

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        Header->PartitionEntryLBA, ArraySize, EntryBuffer);

    UINT64 BackupEntryLBA  = LastBlock - ArraySectors;
    UINT64 BackupHeaderLBA = LastBlock;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        BackupEntryLBA, ArraySize, EntryBuffer);

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, SectorBuffer, BlockSize, 0);
    GPT_HEADER *BackupHeader = (GPT_HEADER*)SectorBuffer;

    *BackupHeader = PrimaryCopy;
    BackupHeader->MyLBA = BackupHeaderLBA;
    BackupHeader->AlternateLBA = 1;
    BackupHeader->PartitionEntryLBA = BackupEntryLBA;

    BackupHeader->HeaderCRC32 = 0;
    UINT32 BackupCRC = 0;
    uefi_call_wrapper(SystemTable->BootServices->CalculateCrc32, 3,
        BackupHeader, BackupHeader->HeaderSize, &BackupCRC);
    BackupHeader->HeaderCRC32 = BackupCRC;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        BackupHeaderLBA, BlockSize, SectorBuffer);

    uefi_call_wrapper(BlockIo->FlushBlocks, 1, BlockIo);

    Print(L"SUCCESS! Partition saved (primary + backup).\n");

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, SectorBuffer);
}

void cmd_gptinit(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* ArgHandleIndex) {
    UINTN TargetIndex = HobbyAtoi(ArgHandleIndex);

    UINTN HandleCount;
    EFI_HANDLE *HandleBuffer;
    uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
        ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &HandleBuffer);

    if (TargetIndex >= HandleCount) {
        Print(L"INvalid Disk.\n");
        return;
    }

    EFI_BLOCK_IO_PROTOCOL *BlockIo;
    uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
        HandleBuffer[TargetIndex], &gEfiBlockIoProtocolGuid, (void **)&BlockIo,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (!BlockIo || !BlockIo->Media || !BlockIo->Media->MediaPresent) {
        Print(L"Error: Media not found.\n");
        return;
    }

    if (BlockIo->Media->LogicalPartition) {
        Print(L"Error: gptinit must be run on the DISK, not on a partition.\n");
        return;
    }

    UINT32 BlockSize = BlockIo->Media->BlockSize;
    UINT64 LastBlock = BlockIo->Media->LastBlock;

    UINT8 *SectorBuffer;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, BlockSize, (void**)&SectorBuffer);
    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, SectorBuffer, BlockSize, 0);

    HOBBY_MBR *Mbr = (HOBBY_MBR*)SectorBuffer;
    Mbr->Signature = 0xAA55;

    Mbr->Partition[0].BootIndicator = 0x00;
    Mbr->Partition[0].StartHead     = 0x00;
    Mbr->Partition[0].StartSector   = 0x02;
    Mbr->Partition[0].StartTrack    = 0x00;
    Mbr->Partition[0].OSIndicator   = 0xEE;
    Mbr->Partition[0].EndHead       = 0xFF;
    Mbr->Partition[0].EndSector     = 0xFF;
    Mbr->Partition[0].EndTrack      = 0xFF;
    Mbr->Partition[0].StartingLBA   = 1;

    if (LastBlock > 0xFFFFFFFF) Mbr->Partition[0].SizeInLBA = 0xFFFFFFFF;
    else                        Mbr->Partition[0].SizeInLBA = (UINT32)LastBlock;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        0, BlockSize, SectorBuffer);

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, SectorBuffer, BlockSize, 0);
    GPT_HEADER *Header = (GPT_HEADER*)SectorBuffer;

    Header->Signature              = 0x5452415020494645ULL;
    Header->Revision               = 0x00010000;
    Header->HeaderSize             = 92;
    Header->HeaderCRC32            = 0;
    Header->MyLBA                  = 1;
    Header->AlternateLBA           = LastBlock;
    Header->FirstUsableLBA         = 34;
    Header->LastUsableLBA          = LastBlock - 33;
    Header->PartitionEntryLBA      = 2;
    Header->NumberOfPartitionEntries = 128;
    Header->SizeOfPartitionEntry     = 128;

    Header->DiskGUID.Data1 = 0x12345678;
    Header->DiskGUID.Data2 = 0x1234;
    Header->DiskGUID.Data3 = 0x5678;
    Header->DiskGUID.Data4[0] = 0xAA;
    Header->DiskGUID.Data4[1] = 0xBB;
    Header->DiskGUID.Data4[2] = 0xCC;
    Header->DiskGUID.Data4[3] = 0xDD;
    Header->DiskGUID.Data4[4] = 0x11;
    Header->DiskGUID.Data4[5] = 0x22;
    Header->DiskGUID.Data4[6] = 0x33;
    Header->DiskGUID.Data4[7] = 0x44;

    UINTN ArraySize = Header->NumberOfPartitionEntries * Header->SizeOfPartitionEntry;
    UINT8 *EntryBuffer;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, ArraySize, (void**)&EntryBuffer);
    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, EntryBuffer, ArraySize, 0);

    UINT32 ArrayCRC = 0;
    uefi_call_wrapper(SystemTable->BootServices->CalculateCrc32, 3, EntryBuffer, ArraySize, &ArrayCRC);
    Header->PartitionEntryArrayCRC32 = ArrayCRC;

    UINT32 HeaderCRC = 0;
    Header->HeaderCRC32 = 0;
    uefi_call_wrapper(SystemTable->BootServices->CalculateCrc32, 3, Header, Header->HeaderSize, &HeaderCRC);
    Header->HeaderCRC32 = HeaderCRC;

    GPT_HEADER PrimaryCopy = *Header;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        1, BlockSize, SectorBuffer);

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        Header->PartitionEntryLBA, ArraySize, EntryBuffer);

    UINTN  ArraySectors    = (ArraySize + BlockSize - 1) / BlockSize;
    UINT64 BackupEntryLBA  = LastBlock - ArraySectors;
    UINT64 BackupHeaderLBA = LastBlock;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        BackupEntryLBA, ArraySize, EntryBuffer);

    uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, SectorBuffer, BlockSize, 0);
    GPT_HEADER *BackupHeader = (GPT_HEADER*)SectorBuffer;

    *BackupHeader = PrimaryCopy;
    BackupHeader->MyLBA = BackupHeaderLBA;
    BackupHeader->AlternateLBA = 1;
    BackupHeader->PartitionEntryLBA = BackupEntryLBA;

    BackupHeader->HeaderCRC32 = 0;
    UINT32 BackupCRC = 0;
    uefi_call_wrapper(SystemTable->BootServices->CalculateCrc32, 3,
        BackupHeader, BackupHeader->HeaderSize, &BackupCRC);
    BackupHeader->HeaderCRC32 = BackupCRC;

    uefi_call_wrapper(BlockIo->WriteBlocks, 5, BlockIo, BlockIo->Media->MediaId,
        BackupHeaderLBA, BlockSize, SectorBuffer);

    uefi_call_wrapper(BlockIo->FlushBlocks, 1, BlockIo);

    Print(L"Success! MBR + GPT (primary + backup) created.\n");

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, SectorBuffer);
}

EFI_FILE_PROTOCOL* GetFileSystemRoot(EFI_HANDLE DeviceHandle, EFI_SYSTEM_TABLE *SystemTable, EFI_HANDLE ImageHandle) {
    EFI_STATUS Status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;

    Status = uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
        DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (void **)&FileSystem,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (EFI_ERROR(Status)) {
        SetColor(SystemTable, COLOR_RED);
        Print(L"   [DEBUG] OpenProtocol Failed! Status: %x\n", Status);
        SetColor(SystemTable, COLOR_WHITE);
        return NULL;
    }

    EFI_FILE_PROTOCOL *Root;
    Status = uefi_call_wrapper(FileSystem->OpenVolume, 2, FileSystem, &Root);
    if (EFI_ERROR(Status)) return NULL;

    return Root;
}

void EnsureDirectory(EFI_FILE_PROTOCOL *Root, CHAR16 *DirName) {
    EFI_FILE_PROTOCOL *DirHandle;
    EFI_STATUS Status;

    Status = uefi_call_wrapper(Root->Open, 5, Root, &DirHandle, DirName, EFI_FILE_MODE_READ, 0);
    
    if (EFI_ERROR(Status)) {
        Status = uefi_call_wrapper(Root->Open, 5, Root, &DirHandle, DirName, 
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 
            EFI_FILE_DIRECTORY);
            
        if (!EFI_ERROR(Status)) {
            Print(L"   [MKDIR] Folder Created: %s\n", DirName);
            uefi_call_wrapper(DirHandle->Close, 1, DirHandle);
        } else {
            Print(L"   [ERRO] When creating folder: %s\n", DirName);
        }
    } else {
        uefi_call_wrapper(DirHandle->Close, 1, DirHandle);
    }
}

void FileCopy(EFI_FILE_PROTOCOL *SourceRoot, CHAR16 *SrcPath, EFI_FILE_PROTOCOL *DestRoot, CHAR16 *DstPath, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_FILE_PROTOCOL *SrcFile, *DstFile;

    Status = uefi_call_wrapper(SourceRoot->Open, 5, SourceRoot, &SrcFile, SrcPath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Print(L"   [ERRO] Origin file not found: %s\n", SrcPath);
        return;
    }

    Status = uefi_call_wrapper(DestRoot->Open, 5, DestRoot, &DstFile, DstPath, 
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        
    if (EFI_ERROR(Status)) {
        Print(L"   [ERRO] It was not possible to create a destination: %s\n", DstPath);
        uefi_call_wrapper(SrcFile->Close, 1, SrcFile);
        return;
    }

    UINT8 Buffer[4096];
    UINTN BufferSize = sizeof(Buffer);
    UINTN TotalBytes = 0;

    Print(L"   Copying %s -> %s...", SrcPath, DstPath);

    while (TRUE) {
        BufferSize = sizeof(Buffer);
        Status = uefi_call_wrapper(SrcFile->Read, 3, SrcFile, &BufferSize, Buffer);
        
        if (EFI_ERROR(Status) || BufferSize == 0) break;

        uefi_call_wrapper(DstFile->Write, 3, DstFile, &BufferSize, Buffer);
        TotalBytes += BufferSize;
    }

    uefi_call_wrapper(DstFile->Flush, 1, DstFile);
    uefi_call_wrapper(SrcFile->Close, 1, SrcFile);
    uefi_call_wrapper(DstFile->Close, 1, DstFile);

    Print(L" OK! (%ld bytes)\n", TotalBytes);
}

void cmd_install(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, CHAR16* ArgDiskOrTarget, CHAR16* ArgPartIdx) {
    BOOLEAN HasPartArg = (ArgPartIdx != NULL && ArgPartIdx[0] != 0);

    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
        ImageHandle, &gEfiLoadedImageProtocolGuid, (void **)&LoadedImage,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    EFI_HANDLE SourceHandle = LoadedImage->DeviceHandle;

    EFI_HANDLE DestHandle = NULL;
    UINTN HandleCount;
    EFI_HANDLE *HandleBuffer;

    uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
        ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &HandleBuffer);

    if (!HasPartArg) {
        UINTN TargetIdx = HobbyAtoi(ArgDiskOrTarget);

        if (TargetIdx >= HandleCount) { Print(L"Invalid Destinantion. \n"); return; }

        DestHandle = HandleBuffer[TargetIdx];

        Print(L"Starting installation on Drive %d...\n", TargetIdx);
    } else {
        UINTN DiskIdx = HobbyAtoi(ArgDiskOrTarget);
        UINTN PartIdx = HobbyAtoi(ArgPartIdx);

        if (DiskIdx >= HandleCount) {
            Print(L"Invalid Disk.\n");
            return;
        }

        EFI_BLOCK_IO_PROTOCOL *DiskBlockIo;
        uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
            HandleBuffer[DiskIdx], &gEfiBlockIoProtocolGuid, (void **)&DiskBlockIo,
            ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

        if (!DiskBlockIo || !DiskBlockIo->Media || !DiskBlockIo->Media->MediaPresent) {
            Print(L"Error: Media not found.\n");
            return;
        }

        if (DiskBlockIo->Media->LogicalPartition) {
            Print(L"Error: install <Disco> <Part> should point to a DISK, not a partition.\n");
            return;
        }

        UINT32 BlockSize = DiskBlockIo->Media->BlockSize;

        UINT8 *Sector;
        uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, BlockSize, (void**)&Sector);
        uefi_call_wrapper(SystemTable->BootServices->SetMem, 3, Sector, BlockSize, 0);

        uefi_call_wrapper(DiskBlockIo->ReadBlocks, 5, DiskBlockIo, DiskBlockIo->Media->MediaId,
            1, BlockSize, Sector);

        GPT_HEADER *Header = (GPT_HEADER*)Sector;

        if (Header->Signature != 0x5452415020494645ULL) {
            Print(L"Error: Disk is not a valid GPT. Run gptinit/part and format the partition.\n");
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
            return;
        }

        if (PartIdx >= Header->NumberOfPartitionEntries) {
            Print(L"Error: Partition [%d] out of limit.\n", (int)PartIdx);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
            return;
        }

        UINTN ArraySize = Header->NumberOfPartitionEntries * Header->SizeOfPartitionEntry;

        UINT8 *EntryBuffer;
        uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3, EfiLoaderData, ArraySize, (void**)&EntryBuffer);

        uefi_call_wrapper(DiskBlockIo->ReadBlocks, 5, DiskBlockIo, DiskBlockIo->Media->MediaId,
            Header->PartitionEntryLBA, ArraySize, EntryBuffer);

        GPT_ENTRY *Entries = (GPT_ENTRY*)EntryBuffer;
        GPT_ENTRY *Target = &Entries[PartIdx];

        if (Target->StartingLBA == 0 && Target->EndingLBA == 0) {
            Print(L"Error: Partition [%d] don't exists (empty entry).\n", (int)PartIdx);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);
            return;
        }

        UINT64 PartStart = Target->StartingLBA;
        UINT64 PartSize  = (Target->EndingLBA - Target->StartingLBA + 1);

        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, EntryBuffer);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, Sector);

        #pragma pack(1)
        typedef struct {
            UINT8  Type;
            UINT8  SubType;
            UINT8  Length[2];
        } DP_HDR;

        typedef struct {
            UINT8  Type;
            UINT8  SubType;
            UINT8  Length[2];
            UINT32 PartitionNumber;
            UINT64 PartitionStart;
            UINT64 PartitionSize;
            UINT8  Signature[16];
            UINT8  MBRType;
            UINT8  SignatureType;
        } HARDDRIVE_DP;
        #pragma pack()

        #define DP_LEN(dp) ((UINT16)((dp)->Length[0] | ((dp)->Length[1] << 8)))
        #define DP_TYPE_MEDIA 0x04
        #define DP_SUBTYPE_HARDDRIVE 0x01
        #define DP_TYPE_END 0x7F
        #define DP_SUBTYPE_END_ENTIRE 0xFF

        UINTN FsCount = 0;
        EFI_HANDLE *FsHandles = NULL;

        EFI_STATUS FsStatus = uefi_call_wrapper(SystemTable->BootServices->LocateHandleBuffer, 5,
            ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &FsCount, &FsHandles);

        if (EFI_ERROR(FsStatus) || FsCount == 0) {
            Print(L"[ERROR] No SimpleFileSystem found. Format the partition to FAT32 and restart.\n");
            return;
        }

        for (UINTN i = 0; i < FsCount; i++) {
            EFI_BLOCK_IO_PROTOCOL *PartBlockIo = NULL;
            EFI_DEVICE_PATH_PROTOCOL *DevPath = NULL;

            uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
                FsHandles[i], &gEfiBlockIoProtocolGuid, (void **)&PartBlockIo,
                ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

            uefi_call_wrapper(SystemTable->BootServices->OpenProtocol, 6,
                FsHandles[i], &gEfiDevicePathProtocolGuid, (void **)&DevPath,
                ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);

            if (!PartBlockIo || !PartBlockIo->Media || !PartBlockIo->Media->MediaPresent) continue;
            if (!PartBlockIo->Media->LogicalPartition) continue;
            if (!DevPath) continue;

            DP_HDR *node = (DP_HDR*)DevPath;
            while (1) {
                if (node->Type == DP_TYPE_END && node->SubType == DP_SUBTYPE_END_ENTIRE) break;

                if (node->Type == DP_TYPE_MEDIA && node->SubType == DP_SUBTYPE_HARDDRIVE) {
                    HARDDRIVE_DP *hd = (HARDDRIVE_DP*)node;

                    if (hd->PartitionStart == PartStart && hd->PartitionSize == PartSize) {
                        DestHandle = FsHandles[i];
                        break;
                    }
                }

                UINT16 step = DP_LEN(node);
                if (step == 0) break;
                node = (DP_HDR*)((UINT8*)node + step);
            }

            if (DestHandle != NULL) break;
        }

        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, FsHandles);

        if (DestHandle == NULL) {
            SetColor(SystemTable, COLOR_RED);
            Print(L"[ERROR] NI couldn't find the FAT32 handle for partition [%d] on the disk. %d.\n", (int)PartIdx, (int)DiskIdx);
            Print(L"Possible reasons::\n");
            Print(L"1. The partition is still RAW (it has not been formatted with: format %d %d)\n", (int)DiskIdx, (int)PartIdx);
            Print(L"2. You do not restart or reconnect the USB drive after formatting.\n");
            SetColor(SystemTable, COLOR_WHITE);
            return;
        }

        Print(L"Starting installation on disk %d, partition [%d]...\n", (int)DiskIdx, (int)PartIdx);
    }

    EFI_FILE_PROTOCOL *SourceRoot = GetFileSystemRoot(SourceHandle, SystemTable, ImageHandle);
    EFI_FILE_PROTOCOL *DestRoot   = GetFileSystemRoot(DestHandle,  SystemTable, ImageHandle);

    if (!SourceRoot) { 
        SetColor(SystemTable, COLOR_RED);
        Print(L"[CRITICAL ERROR] I couldn't read the Source (HobbyOS USB drive).\n"); 
        SetColor(SystemTable, COLOR_WHITE);
        return; 
    }

    if (!DestRoot) {
        SetColor(SystemTable, COLOR_RED);
        Print(L"[CRITICAL ERROR] Failed to read the destination.\n");
        Print(L"Possible reasons:\n");
        Print(L"1. The destination was not formatted as FAT32.\n");
        Print(L"2. You did NOT reboot the computer after formatting.\n");
        SetColor(SystemTable, COLOR_WHITE);
        return;
    }

    EnsureDirectory(DestRoot, L"EFI");
    EnsureDirectory(DestRoot, L"EFI\\BOOT");

    EFI_FILE_PROTOCOL *CheckHandle;
    EFI_STATUS Status = uefi_call_wrapper(DestRoot->Open, 5, DestRoot, &CheckHandle, L"EFI\\BOOT", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        SetColor(SystemTable, COLOR_RED);
        Print(L"[ERROR] Failed to verify EFI\\BOOT directory. Aborting.\n");
        SetColor(SystemTable, COLOR_WHITE);
        return;
    }
    uefi_call_wrapper(CheckHandle->Close, 1, CheckHandle);

    FileCopy(SourceRoot, L"\\EFI\\BOOT\\BOOTX64.EFI", DestRoot, L"\\EFI\\BOOT\\BOOTX64.EFI", SystemTable);

    SetColor(SystemTable, COLOR_GREEN);
    Print(L"\n----------------------------------------------\n");
    Print(L"        INSTALLATION COMPLETED SUCCESSFULLY!  \n");
    Print(L"----------------------------------------------\n");
    SetColor(SystemTable, COLOR_WHITE);
    Print(L"You may now remove this USB drive and boot from the new disk.\n");

    uefi_call_wrapper(SourceRoot->Close, 1, SourceRoot);
    uefi_call_wrapper(DestRoot->Close, 1, DestRoot);

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, HandleBuffer);
}