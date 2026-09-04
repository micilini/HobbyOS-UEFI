#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=runtime-ready-focused
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){ echo "Usage: $0 host-unit|up-tcg|smp4-tcg|smp4-kvm|smp8-tcg|smp8-kvm|all" >&2; exit 2; }
cleanup(){ stop_qemu; }
trap cleanup EXIT

host_unit(){
  local dir old valid noauto early
  dir=$(mktemp -d)
  old=$dir/old valid=$dir/valid noauto=$dir/noauto early=$dir/early
  printf '%s\n' '[SELFTEST][AUTORUN] BEGIN' '[BOOT][SHELL_READY] PASS' \
    '[BOOT][RUNTIME_READY] PASS cpus=1/1' >"$old"
  ! runtime_validate_boot_log "$old" 1
  printf '%s\n' '[BOOT][SHELL_READY] PASS' \
    '[BOOT][RUNTIME_READY] PASS cpus=1/1' '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][AUTORUN] PASS' \
    '[BOOT][TEST_READY] PASS autorun=1 selftests=1' >"$valid"
  runtime_validate_boot_log "$valid" 1
  printf '%s\n' '[BOOT][SHELL_READY] PASS' \
    '[BOOT][RUNTIME_READY] PASS cpus=1/1' \
    '[BOOT][TEST_READY] PASS autorun=0 selftests=0' >"$noauto"
  runtime_validate_boot_log "$noauto" 0
  printf '%s\n' '[BOOT][SHELL_READY] PASS' \
    '[BOOT][RUNTIME_READY] PASS cpus=1/1' '[HARNESS][BEGIN] seq=1' \
    '[SELFTEST][AUTORUN] BEGIN' '[SELFTEST][AUTORUN] PASS' \
    '[BOOT][TEST_READY] PASS autorun=1 selftests=1' >"$early"
  ! runtime_validate_boot_log "$early" 1
  rm -r "$dir"
  echo '[CL13][RUNTIME_READY_HOST_UNIT] PASS'
}

build_image(){
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
}

run_boot(){
  local name=$1 smp=$2 accel=$3 run=$4 log prefix
  log="$artifact_dir/cl13fix8-runtime-${name}-run${run}.log"
  prefix="$artifact_dir/cl13fix8-runtime-${name}-run${run}"
  start_qemu "$smp" "$accel"
  runtime_validate_boot_log "$serial" 1
  selftest_validate_autorun "$serial" "$prefix"
  framed_send_complete "tasktest readiness-status" \
    "[BOOT][READINESS_STATUS] PASS state=TEST_READY cpus=$smp/$smp" 120 stress
  framed_send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 120 stress
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 120 stress
  cp "$serial" "$log"
  assert_clean_log "$log"
  stop_qemu
  printf 'runtime-ready-focused\t%s-run%s\t%s\t%s\tPASS\t%s\n' \
    "$name" "$run" "$smp" "$accel" "$log" >>"$artifact_dir/cl13-ledger.tsv"
}

group(){
  local name=$1 smp=$2 accel=$3 runs=$4 run
  [[ $accel != kvm || ( -r /dev/kvm && -w /dev/kvm ) ]] || {
    echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: KVM unavailable' >&2
    return 3
  }
  build_image
  harness_lock
  for ((run=1; run<=runs; run++)); do run_boot "$name" "$smp" "$accel" "$run"; done
  harness_unlock
}

case ${1:-} in
  host-unit) host_unit;;
  up-tcg) group up-tcg 1 tcg 2;;
  smp4-tcg) group smp4-tcg 4 tcg 3;;
  smp4-kvm) group smp4-kvm 4 kvm 3;;
  smp8-tcg) group smp8-tcg 8 tcg 3;;
  smp8-kvm) group smp8-kvm 8 kvm 5;;
  all)
    host_unit
    group up-tcg 1 tcg 2
    group smp4-tcg 4 tcg 3
    group smp4-kvm 4 kvm 3
    group smp8-tcg 8 tcg 3
    group smp8-kvm 8 kvm 5
    echo '[CL13][RUNTIME_READY_CLASSIFICATION] PREMATURE_RUNTIME_TESTING_CONFIRMED'
    ;;
  *) usage;;
esac
