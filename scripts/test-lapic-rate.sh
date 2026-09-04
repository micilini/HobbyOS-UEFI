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

host_evidence(){
  local output=artifacts/build/cl10fix3-kvm-host.env
  {
    echo "nproc=$(nproc)"
    echo "loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
    awk '/^Cpus_allowed_list:/{print}' /proc/self/status
    if [[ -r /sys/fs/cgroup/cpu.max ]]; then
      echo "cpu.max=$(< /sys/fs/cgroup/cpu.max)"
    fi
    if [[ -r /sys/fs/cgroup/cpuset.cpus.effective ]]; then
      echo "cpuset.cpus.effective=$(< /sys/fs/cgroup/cpuset.cpus.effective)"
    fi
  } >"$output"
}

run_one(){
  local accel=$1 thread=$2 label=$3 after_load=${4:-1}
  TCG_THREAD=$thread start_qemu 8 "$accel"
  cp .qemu/launch.env "artifacts/build/cl10fix3-lapic-${label}.launch.env"
  send_complete "accounttest lapic-config" "[ACCOUNT][LAPIC_CONFIG] PASS" 60
  send_complete "accounttest lapic-liveness 500" "[ACCOUNT][LAPIC_LIVENESS] PASS" 60
  send_complete "accounttest lapic-rate 2000 5" "[ACCOUNT][LAPIC_RATE] PASS" 120
  if ((after_load)); then
    send_complete "accounttest lapic-after-load" "[ACCOUNT][LAPIC_AFTER_LOAD] PASS" 300
  fi
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  cp "$serial" "artifacts/build/cl10fix3-lapic-${label}.log"
  assert_clean_log "artifacts/build/cl10fix3-lapic-${label}.log"
  stop_qemu
}

negative_one(){
  local macro=$1 command=$2 sentinel=$3 artifact=$4
  make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  make image KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  start_qemu 8 tcg
  send_raw_complete "$command" "$sentinel" 90
  cp "$serial" "artifacts/build/$artifact"
  stop_qemu
  make kernel-check JOBS=2 >/dev/null
  make image >/dev/null
}

matrix(){
  make image >/dev/null
  run_one tcg multi tcg-multi
  run_one tcg single tcg-single
  if [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]; then
    host_evidence
    for run in 1 2 3; do run_one kvm multi "kvm-run${run}"; done
  else
    printf 'SKIP_BY_ENVIRONMENT\n' >artifacts/build/cl10fix3-kvm-host.env
    for run in 1 2 3; do
      printf 'SKIP_BY_ENVIRONMENT\n' >"artifacts/build/cl10fix3-lapic-kvm-run${run}.log"
    done
  fi
}

negatives(){
  negative_one HOBBYOS_LAPIC_NEGATIVE_MASK_ONE_CPU \
    "accounttest lapic-liveness 500" \
    "[ACCOUNT][NEGATIVE] LAPIC_MASKED_CPU_DETECTED" \
    cl10fix3-negative-lapic-masked.log
  negative_one HOBBYOS_LAPIC_NEGATIVE_WRONG_PERIOD \
    "accounttest lapic-config" \
    "[ACCOUNT][NEGATIVE] LAPIC_WRONG_PERIOD_DETECTED" \
    cl10fix3-negative-lapic-period.log
}

case ${1:-all} in
  matrix) matrix;;
  negatives) negatives;;
  all) negatives; matrix;;
  *) echo "usage: $0 matrix|negatives|all" >&2; exit 2;;
esac
