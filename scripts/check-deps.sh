#!/usr/bin/env bash
set -euo pipefail
scope=${SCOPE:-${1:-all}}; missing=0
CC=${CC:-gcc}; LD=${LD:-ld}; OBJCOPY=${OBJCOPY:-objcopy}; QEMU=${QEMU:-qemu-system-x86_64}; ARCH=${ARCH:-x86_64}
ok(){ printf '[OK] %s\n' "$*"; }; miss(){ printf '[MISSING] %s\n' "$*"; missing=1; }; info(){ printf '[INFO] %s\n' "$*"; }; optional(){ printf '[OPTIONAL] %s\n' "$*"; }
cmd(){ command -v "$1" >/dev/null && ok "$2: $1 ($(command -v "$1"))" || miss "$2: $1; override: $3; Debian/Ubuntu hint: $4"; }
find_efi(){
  if [[ -z ${EFIINC:-} ]]; then for p in /usr/include/efi /usr/local/include/efi; do [[ -d $p ]] && EFIINC=$p && break; done; fi
  if [[ -z ${EFILIB:-} ]]; then
    multi=$($CC -print-multiarch 2>/dev/null || true)
    for p in /usr/lib /usr/lib64 /usr/lib/$multi /usr/lib/gnuefi /usr/local/lib; do [[ -f $p/crt0-efi-${ARCH}.o ]] && EFILIB=$p && break; done
  fi
  [[ -n ${EFIINC:-} && -f ${EFIINC}/efi.h ]] && ok "EFI headers: $EFIINC" || miss "EFI headers; override EFIINC; Debian/Ubuntu hint: gnu-efi"
  [[ -n ${EFILIB:-} && -f ${EFILIB}/crt0-efi-${ARCH}.o ]] && ok "GNU-EFI CRT: $EFILIB" || miss "crt0-efi-${ARCH}.o; override EFILIB; Debian/Ubuntu hint: gnu-efi"
  for f in elf_${ARCH}_efi.lds libgnuefi.a libefi.a; do [[ -n ${EFILIB:-} && -f ${EFILIB}/$f ]] && ok "$f: $EFILIB/$f" || miss "$f; override EFILIB; Debian/Ubuntu hint: gnu-efi"; done
  for c in mformat mmd mcopy dd; do cmd "$c" 'EFI/image' "$c" 'mtools (dd: coreutils)'; done
}
find_ovmf(){
  if [[ -n ${OVMF_FD:-} && -f $OVMF_FD ]]; then ok "OVMF combined: $OVMF_FD"; return; fi
  if [[ -n ${OVMF_CODE:-} && -f $OVMF_CODE && -n ${OVMF_VARS:-} && -f $OVMF_VARS ]]; then ok "OVMF pflash: $OVMF_CODE + $OVMF_VARS"; return; fi
  for p in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/qemu/OVMF.fd; do [[ -f $p ]] && { ok "OVMF combined: $p"; return; }; done
  for d in /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/x64; do
    [[ -f $d/OVMF_CODE.fd && -f $d/OVMF_VARS.fd ]] && { ok "OVMF pflash: $d/OVMF_CODE.fd + $d/OVMF_VARS.fd"; return; }
  done
  miss 'OVMF firmware; override OVMF_FD or OVMF_CODE/OVMF_VARS; Debian/Ubuntu hint: ovmf'
}
case $scope in
 kernel|all) cmd "$CC" 'kernel build' CC build-essential; cmd "$LD" 'kernel build' LD binutils; cmd "$OBJCOPY" 'kernel build' OBJCOPY binutils; cmd make 'kernel build' MAKE make; cmd sha256sum 'kernel gate' SHA256SUM coreutils; cmd tee 'kernel gate' TEE coreutils;; esac
case $scope in image|all) find_efi;; esac
case $scope in qemu|all) cmd "$QEMU" QEMU QEMU qemu-system-x86; cmd python3 QEMU PYTHON python3; find_ovmf; if [[ ${ACCEL:-auto} == kvm ]]; then [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] && ok '/dev/kvm accessible' || miss '/dev/kvm inaccessible (required by ACCEL=kvm)'; else [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] && optional '/dev/kvm accessible; ACCEL=auto may use KVM' || optional '/dev/kvm unavailable; ACCEL=auto will use TCG'; fi;; esac
[[ $scope =~ ^(kernel|image|qemu|all)$ ]] || { echo "invalid SCOPE=$scope" >&2; exit 2; }
info "scope=$scope"
exit "$missing"
