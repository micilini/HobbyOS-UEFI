set StartupDelay 0
@echo -off
mode 80 25

cls
if exist .\efi\boot\BOOTX64.EFI then
 .\efi\boot\BOOTX64.EFI
 goto END
endif

if exist fs0:\efi\boot\BOOTX64.EFI then
 fs0:
 echo Found bootloader on fs0:
 efi\boot\BOOTX64.EFI
 goto END
endif

 echo "Unable to find bootloader".
:END