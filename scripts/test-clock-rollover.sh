#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0
harness_command=boot
source scripts/harness-common.sh
harness_lock
trap stop_qemu EXIT

run_environment(){
  local accel=$1 thread=$2 label=$3
  TCG_THREAD=$thread start_qemu 8 "$accel"
  cp .qemu/launch.env "artifacts/build/cl10fix2-rollover-${label}.launch.env"
  send_complete "accounttest hpet-stats" "[ACCOUNT][HPET_STATS]" 60
  send_complete "accounttest hpet-rollover 1" \
    "[ACCOUNT][HPET_ROLLOVER] PASS" 120
  send_complete "accounttest clock-smp 32 1000000" \
    "[ACCOUNT][CLOCK_SMP] PASS" 300
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  cp "$serial" "artifacts/build/cl10fix2-rollover-${label}.log"
  assert_clean_log "artifacts/build/cl10fix2-rollover-${label}.log"
  stop_qemu
}

negative(){
  make kernel-check JOBS=2 \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_HPET_NEGATIVE_TORN_COUNTER_READ >/dev/null
  make image \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_HPET_NEGATIVE_TORN_COUNTER_READ >/dev/null
  start_qemu 1 tcg
  scripts/wait-for-log.sh "$serial" \
    "[CLOCK][NEGATIVE] TORN_64BIT_COUNTER_READ_DETECTED" 120 >/dev/null
  cp "$serial" artifacts/build/cl10fix2-negative-hpet-torn.log
  stop_qemu
  make kernel-check JOBS=2 >/dev/null
  make image >/dev/null
  ! rg -n "HOBBYOS_HPET_NEGATIVE_TORN_COUNTER_READ" \
    artifacts/build/kernel-check-j2.log
}

matrix(){
  make image >/dev/null
  run_environment tcg multi multi
  run_environment tcg single single
  if [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]; then
    run_environment kvm multi kvm
  else
    printf 'SKIP_BY_ENVIRONMENT\n' \
      >artifacts/build/cl10fix2-rollover-kvm.launch.env
    printf 'SKIP_BY_ENVIRONMENT\n' \
      >artifacts/build/cl10fix2-rollover-kvm.log
  fi
}

soak_run(){
  make image >/dev/null
  TCG_THREAD=multi start_qemu 8 tcg
  cp .qemu/launch.env artifacts/build/cl10fix2-wrap-soak.launch.env
  send_complete "accounttest hpet-wrap-soak 130000" \
    "[ACCOUNT][HPET_WRAP_SOAK] PASS" 240
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  cp "$serial" artifacts/build/cl10fix2-wrap-soak.log
  assert_clean_log artifacts/build/cl10fix2-wrap-soak.log
  stop_qemu
}

case ${1:-all} in
  matrix) matrix;;
  negative) negative;;
  soak) soak_run;;
  all) negative; matrix; soak_run;;
  *) echo "usage: $0 matrix|negative|soak|all" >&2; exit 2;;
esac
