#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=synctest-async-focused
image_ready=0
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){ echo "Usage: $0 smp1-tcg|smp2-tcg|smp2-kvm|smp4-tcg|smp4-kvm|smp8-tcg|smp8-kvm|all" >&2; exit 2; }
cleanup(){ stop_qemu; }
trap cleanup EXIT

prepare_image(){
  ((image_ready)) && return
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  image_ready=1
}

async_suite(){
  framed_synctest_async SEM "synctest sem 20000" \
    "[SYNC][SEM] START" "[SYNC][SEM] PASS" 300 stress
  framed_synctest_async BOUNDARY "synctest boundary 5000" \
    "[SYNC][BOUNDARY] START" "[SYNC][BOUNDARY] PASS" 360 stress
  framed_synctest_async BOUNDARY_NOISE \
    "synctest boundary-noise 5000 5000" \
    "[SYNC][BOUNDARY_NOISE] START" "[SYNC][BOUNDARY_NOISE] PASS" 360 stress
  framed_synctest_async SLEEP "synctest sleep" \
    "[SYNC][SLEEP] START" "[SYNC][SLEEP] PASS" 180 stress
  framed_synctest_async CANCEL "synctest cancel" \
    "[SYNC][CANCEL] START" "[SYNC][CANCEL] PASS" 180 stress
  framed_synctest_async TIMER_CANCEL "synctest timer-cancel" \
    "[SYNC][TIMER_CANCEL] START" "[SYNC][TIMER_CANCEL] PASS" 180 stress
  framed_synctest_async RACE "synctest race 2000" \
    "[SYNC][RACE] START" "[SYNC][RACE] PASS" 300 stress
}

run_boot(){
  local name=$1 smp=$2 accel=$3 run=$4 log runs
  log="$artifact_dir/cl13fix8-sync-${name}-run${run}.log"
  start_qemu "$smp" "$accel"
  selftest_validate_autorun "$serial" "$artifact_dir/cl13fix8-sync-${name}-run${run}"
  async_suite
  framed_send_complete "synctest check" "[SYNC][CHECK] PASS" 120 stress
  framed_send_complete "synctest async-status" \
    "[SYNC][ASYNC_STATUS] PASS" 120 stress
  framed_send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 120 stress
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 120 stress
  cp "$serial" "$log"
  runs=$(awk '
    /^\[SYNC\]\[(SEM|BOUNDARY|BOUNDARY_NOISE|SLEEP|CANCEL|TIMER_CANCEL|RACE)\] START run=/ {
      for (i=1;i<=NF;i++) if ($i ~ /^run=/) { sub(/^run=/,"",$i); print $i }
    }' "$log" | sort -n -u | wc -l)
  [[ $runs == 7 ]]
  ! grep -Eq '^\[SYNC\]\[ASYNC_WAIT\] FAIL|reason=(timeout|unknown-run)' "$log"
  assert_clean_log "$log"
  stop_qemu
  printf 'synctest-async-focused\t%s-run%s\t%s\t%s\tPASS\truns=7\n' \
    "$name" "$run" "$smp" "$accel" >>"$artifact_dir/cl13-ledger.tsv"
}

group(){
  local name=$1 smp=$2 accel=$3 count=$4 run
  [[ $accel != kvm || ( -r /dev/kvm && -w /dev/kvm ) ]] || return 3
  prepare_image; harness_lock
  for ((run=1; run<=count; run++)); do run_boot "$name" "$smp" "$accel" "$run"; done
  harness_unlock
}

case ${1:-} in
  smp1-tcg) group smp1-tcg 1 tcg 3;;
  smp2-tcg) group smp2-tcg 2 tcg 3;;
  smp2-kvm) group smp2-kvm 2 kvm 3;;
  smp4-tcg) group smp4-tcg 4 tcg 5;;
  smp4-kvm) group smp4-kvm 4 kvm 3;;
  smp8-tcg) group smp8-tcg 8 tcg 2;;
  smp8-kvm) group smp8-kvm 8 kvm 2;;
  all)
    group smp1-tcg 1 tcg 3; group smp2-tcg 2 tcg 3; group smp2-kvm 2 kvm 3
    group smp4-tcg 4 tcg 5; group smp4-kvm 4 kvm 3
    group smp8-tcg 8 tcg 2; group smp8-kvm 8 kvm 2
    echo '[CL13][SYNCTEST_ASYNC_CLASSIFICATION] SYNC_WRAPPER_FALSE_REJECTION_CONFIRMED'
    echo '[CL13][FIX8_CLASSIFICATION] PRE_READY_AUTORUN_AND_FRAGMENTED_HARNESS_CONFIRMED'
    ;;
  *) usage;;
esac
