#!/usr/bin/env bash
set -euo pipefail
act=${1:?action required}; root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
r=${QEMU_RUNTIME:-.qemu}; pidf=$r/qemu.pid; sock=$r/hmp.sock; envf=$r/launch.env
serial=$r/qemu-serial.log; debug=$r/qemu-debugcon.log; trace=$r/qemu-trace.log
hmp=(python3 scripts/qemu_hmp.py --socket "$sock")
alivepid(){ kill -0 "$1" 2>/dev/null && [[ $(ps -p "$1" -o stat= 2>/dev/null) != Z* ]]; }
live(){ [[ -f $pidf ]] || return 1; local p; p=$(cat "$pidf" 2>/dev/null) || return 1; [[ $p =~ ^[0-9]+$ ]] || return 1; alivepid "$p" || return 1; ps -p "$p" -o args= | grep -F -- "-monitor unix:$sock" >/dev/null; }
show_paths(){ echo "PID file: $pidf"; echo "HMP socket: $sock"; echo "serial: $serial"; echo "debugcon: $debug"; echo "trace: $trace"; }
case $act in
 start)
  SCOPE=qemu scripts/check-deps.sh
  mkdir -p "$r"; live && { echo 'QEMU agent already active' >&2; exit 1; }
  [[ -f $pidf ]] && rm -f "$pidf"; [[ -S $sock ]] && rm -f "$sock"
  [[ ${SMP:-1} =~ ^[1-9][0-9]*$ ]] || { echo 'SMP must be a positive integer' >&2; exit 2; }
  machine=${MACHINE:-q35}; [[ $machine == q35 || $machine == pc ]] || { echo 'MACHINE must be q35 or pc' >&2; exit 2; }
  accel=${ACCEL:-auto}; if [[ $accel == auto ]]; then if [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]; then accel=kvm; else accel=tcg; fi; echo "ACCEL=auto selected $accel"; fi
  [[ $accel == tcg || $accel == kvm ]] || { echo 'ACCEL must be auto, kvm, or tcg' >&2; exit 2; }
  [[ $accel != kvm || (-e /dev/kvm && -r /dev/kvm && -w /dev/kvm) ]] || { echo 'ACCEL=kvm requested but /dev/kvm is unavailable' >&2; exit 1; }
  qemu=${QEMU:-qemu-system-x86_64}; cpu=max; [[ $accel == kvm ]] && cpu=host
  tcg_thread=${TCG_THREAD:-multi}; [[ $tcg_thread == multi || $tcg_thread == single ]] || exit 2; accel_arg=$accel; [[ $accel == tcg ]] && accel_arg="tcg,thread=$tcg_thread"
  fw=(); vars_copy=
  if [[ -n ${OVMF_FD:-} ]]; then fw=(-bios "$OVMF_FD")
  elif [[ -n ${OVMF_CODE:-} && -n ${OVMF_VARS:-} ]]; then vars_copy=$r/OVMF_VARS.fd; cp "$OVMF_VARS" "$vars_copy"; fw=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" -drive "if=pflash,format=raw,file=$vars_copy")
  else
    for p in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/qemu/OVMF.fd; do [[ -f $p ]] && fw=(-bios "$p") && break; done
    if ((${#fw[@]}==0)); then for d in /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/x64; do if [[ -f $d/OVMF_CODE.fd && -f $d/OVMF_VARS.fd ]]; then vars_copy=$r/OVMF_VARS.fd; cp "$d/OVMF_VARS.fd" "$vars_copy"; fw=(-drive "if=pflash,format=raw,readonly=on,file=$d/OVMF_CODE.fd" -drive "if=pflash,format=raw,file=$vars_copy"); break; fi; done; fi
  fi
  ((${#fw[@]})) || { echo 'OVMF not found' >&2; exit 1; }; image=${HOBBYOS_IMAGE:-hobbyos.img}; [[ -f $image ]] || { echo "HobbyOS image missing: $image" >&2; exit 1; }
  input_devices=()
  if [[ $machine == q35 ]]; then
    input_devices=(-device nec-usb-xhci,id=xhci,msi=on,msix=off \
      -device usb-kbd,bus=xhci.0)
  fi
  : >"$serial"; : >"$debug"; : >"$trace"
  "$qemu" -machine "$machine" -accel "$accel_arg" -cpu "$cpu" -smp "${SMP:-1},sockets=1,cores=${SMP:-1},threads=1" -m "${MEM:-2G}" "${fw[@]}" -net none -drive "file=$image,format=raw,cache=writeback" -serial "file:$serial" -debugcon "file:$debug" -global isa-debugcon.iobase=0x402 -d guest_errors -D "$trace" "${input_devices[@]}" -display none -monitor "unix:$sock,server=on,wait=off" -daemonize -pidfile "$pidf"
  qemu_version=$("$qemu" --version | head -n 1)
  image_hash=$(sha256sum "$image" | awk '{print $1}')
  kernel_hash=$(sha256sum kernel.elf | awk '{print $1}')
  printf 'MACHINE=%q\nACCEL=%q\nTCG_THREAD=%q\nSMP=%q\nMEM=%q\nIMAGE=%q\nIMAGE_SHA256=%q\nKERNEL_SHA256=%q\nQEMU_VERSION=%q\n' "$machine" "$accel" "$tcg_thread" "${SMP:-1}" "${MEM:-2G}" "$image" "$image_hash" "$kernel_hash" "$qemu_version" >"$envf"
  for _ in {1..50}; do [[ -S $sock ]] && "${hmp[@]}" command 'info status' >/dev/null 2>&1 && break; sleep .1; done
  live && "${hmp[@]}" command 'info status'; show_paths;;
 status)
  show_paths; [[ -f $envf ]] && sed -n '1,4p' "$envf"; [[ -f $pidf ]] && echo "PID: $(cat "$pidf")" || echo 'PID file: absent'
  if live; then echo 'process: alive'; [[ -S $sock ]] && echo 'socket: present'; "${hmp[@]}" command 'info status'; else echo 'process: inactive or stale'; [[ -S $sock ]] && echo 'socket: stale' || echo 'socket: absent'; exit 1; fi;;
 stop)
  if ! live; then echo 'QEMU agent is not active'; rm -f "$pidf" "$sock"; exit 0; fi
  p=$(cat "$pidf"); "${hmp[@]}" quit || true
  for _ in {1..30}; do alivepid "$p" || break; sleep .1; done
  if alivepid "$p"; then echo 'HMP quit timed out; sending SIGTERM'; kill -TERM "$p"; for _ in {1..20}; do alivepid "$p" || break; sleep .1; done; fi
  if alivepid "$p"; then echo 'SIGTERM timed out; sending SIGKILL'; kill -KILL "$p"; fi
  rm -f "$pidf" "$sock"; echo 'QEMU agent stopped; logs preserved';;
 logs) for f in "$serial" "$debug" "$trace"; do echo "== $f =="; [[ -f $f ]] && tail -n "${LINES:-40}" "$f" || echo '(missing)'; done;;
 *) echo "unknown action: $act" >&2; exit 2;; esac
