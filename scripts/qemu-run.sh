#!/usr/bin/env bash
set -euo pipefail
case ${QEMU_FAULT_TRACE:-0} in
    0) fault_args=(-d guest_errors) ;;
    1) fault_args=(-no-reboot -d guest_errors,int,cpu_reset) ;;
    *) echo 'QEMU_FAULT_TRACE must be 0 or 1' >&2; exit 2 ;;
esac
[[ ${SMP:-4} =~ ^[1-9][0-9]*$ ]] || { echo 'SMP must be a positive integer' >&2; exit 2; }
machine=${MACHINE:-q35}; [[ $machine == q35 || $machine == pc ]] || { echo 'MACHINE must be q35 or pc' >&2; exit 2; }
accel=${ACCEL:-auto}; if [[ $accel == auto ]]; then if [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]; then accel=kvm; else accel=tcg; fi; echo "ACCEL=auto selected $accel"; fi
[[ $accel == tcg || $accel == kvm ]] || { echo 'ACCEL must be auto, kvm, or tcg' >&2; exit 2; }
[[ $accel != kvm || (-e /dev/kvm && -r /dev/kvm && -w /dev/kvm) ]] || { echo 'ACCEL=kvm requested but /dev/kvm is unavailable' >&2; exit 1; }
mkdir -p .qemu; : >.qemu/qemu-serial.log; : >.qemu/qemu-debugcon.log; : >.qemu/qemu-trace.log
cpu=max; [[ $accel == kvm ]] && cpu=host
case ${QEMU_CONTROL_PROFILE:-normal} in
    normal) ;;
    nx-disabled)
        [[ $machine == q35 && $accel == tcg && ${SMP:-4} == 4 ]] || { echo 'nx-disabled requires Q35/TCG/SMP4' >&2; exit 2; }
        cpu=max,-nx ;;
    xapic)
      [[ $machine == q35 && $accel == kvm && ${SMP:-4} == 4 ]] || { echo 'xapic requires Q35/KVM/SMP4' >&2; exit 2; }
      cpu=host,-x2apic ;;
    *) echo 'Unsupported control profile' >&2; exit 2 ;;
esac
source scripts/qemu-ram-profile.sh
configure_ram_profile 4
tcg_thread=${TCG_THREAD:-multi}; [[ $tcg_thread == multi || $tcg_thread == single ]] || exit 2; accel_arg=$accel; [[ $accel == tcg ]] && accel_arg="tcg,thread=$tcg_thread"
fw=(); if [[ -n ${OVMF_FD:-} ]]; then fw=(-bios "$OVMF_FD"); elif [[ -n ${OVMF_CODE:-} && -n ${OVMF_VARS:-} ]]; then cp "$OVMF_VARS" .qemu/OVMF_VARS.fd; fw=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" -drive if=pflash,format=raw,file=.qemu/OVMF_VARS.fd); else for p in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/qemu/OVMF.fd; do [[ -f $p ]] && fw=(-bios "$p") && break; done; fi
((${#fw[@]})) || { echo 'OVMF not found; set OVMF_FD or OVMF_CODE/OVMF_VARS' >&2; exit 1; }
image=${HOBBYOS_IMAGE:-hobbyos.img}; [[ -f $image ]] || { echo "HobbyOS image missing: $image" >&2; exit 1; }
input_devices=()
if [[ $machine == q35 ]]; then
    input_devices=(-device nec-usb-xhci,id=xhci,msi=on,msix=off \
        -device usb-kbd,bus=xhci.0)
fi
launch=("${QEMU:-qemu-system-x86_64}" -machine "$machine_arg" -accel "$accel_arg" -cpu "$cpu" -smp "${SMP:-4},sockets=1,cores=${SMP:-4},threads=1" -m "${MEM:-2G}" "${fw[@]}" -net none -drive "file=$image,format=raw,cache=writeback" -serial file:.qemu/qemu-serial.log -debugcon file:.qemu/qemu-debugcon.log -global isa-debugcon.iobase=0x402 "${fault_args[@]}" -D .qemu/qemu-trace.log "${input_devices[@]}")
if [[ -n ${QEMU_VM_ID:-} ]]; then launch+=(-uuid "$QEMU_VM_ID"); fi
if [[ -n ${QEMU_LAUNCH_RECEIPT:-} ]]; then
    python3 scripts/qemu-launch-receipt.py "$QEMU_LAUNCH_RECEIPT" "$image" "${HOBBYOS_KERNEL:-kernel.elf}" "${launch[@]}"
fi
exec "${launch[@]}"
